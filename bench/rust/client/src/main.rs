// Closed-loop ping-pong load generator, same for every server.
// usage: client addr conns size seconds threads [churn]
// Each connection sends `size` bytes, reads exactly `size` back, compares, repeats.
// churn: each of `conns` loops instead does connect, one round-trip, close with RST
// (zero linger: no TIME_WAIT, so ephemeral ports do not run out); counts connections.
// Prints "total_round_trips round_trips_per_sec p50_us p99_us p999_us"; exits 1 if any echo is
// short or differs. Latency of every round-trip goes into a per-thread log-linear histogram
// (128 sub-buckets per power of two, < 1% error; fixed array, nothing allocated while measuring).
use std::cell::RefCell;
use std::time::{Duration, Instant};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;

const SUB: usize = 128; // values below SUB ns are exact; above, 7 bits of mantissa
const NB: usize = (64 - 6) * SUB;

fn bucket(ns: u64) -> usize {
    if ns < SUB as u64 {
        return ns as usize;
    }
    let e = 63 - ns.leading_zeros() as usize; // >= 7
    (e - 6) * SUB + (ns >> (e - 7)) as usize - SUB
}

fn lower(i: usize) -> u64 {
    if i < SUB {
        return i as u64;
    }
    ((i % SUB + SUB) as u64) << (i / SUB + 6 - 7)
}

thread_local!(static HIST: RefCell<Vec<u64>> = RefCell::new(vec![0; NB]));

async fn conn(addr: String, size: usize, deadline: Instant, churn: bool) -> std::io::Result<u64> {
    let mut s = TcpStream::connect(&addr).await?;
    s.set_nodelay(true)?;
    let out: Vec<u8> = (0..size).map(|i| (i * 31 + 7) as u8).collect();
    let mut inb = vec![0u8; size];
    let mut n = 0u64;
    let mut t = Instant::now();
    while t < deadline {
        if churn && n > 0 {
            s.set_zero_linger()?;
            drop(s);
            s = TcpStream::connect(&addr).await?;
            s.set_nodelay(true)?;
        }
        s.write_all(&out).await?;
        s.read_exact(&mut inb).await?;
        let t2 = Instant::now();
        if inb != out {
            return Err(std::io::Error::new(std::io::ErrorKind::InvalidData, "echo mismatch"));
        }
        HIST.with(|h| h.borrow_mut()[bucket((t2 - t).as_nanos() as u64)] += 1);
        t = t2;
        n += 1;
    }
    Ok(n)
}

fn main() {
    let a: Vec<String> = std::env::args().collect();
    if a.len() != 6 && !(a.len() == 7 && a[6] == "churn") {
        eprintln!("usage: client addr conns size seconds threads [churn]");
        std::process::exit(2);
    }
    let conns: usize = a[2].parse().unwrap();
    let size: usize = a[3].parse().unwrap();
    let secs: f64 = a[4].parse().unwrap();
    let threads: usize = a[5].parse().unwrap();
    let churn = a.len() == 7;
    let start = Instant::now();
    let deadline = start + Duration::from_secs_f64(secs);
    let hs: Vec<_> = (0..threads)
        .map(|t| {
            let addr = a[1].clone();
            let mine = conns / threads + (t < conns % threads) as usize;
            std::thread::spawn(move || {
                let rt = tokio::runtime::Builder::new_current_thread().enable_all().build().unwrap();
                rt.block_on(async {
                    let js: Vec<_> = (0..mine).map(|_| tokio::spawn(conn(addr.clone(), size, deadline, churn))).collect();
                    let mut total = 0;
                    for j in js {
                        total += j.await.unwrap()?;
                    }
                    Ok::<(u64, Vec<u64>), std::io::Error>((total, HIST.take()))
                })
            })
        })
        .collect();
    let mut total = 0;
    let mut hist = vec![0u64; NB];
    for h in hs {
        match h.join().unwrap() {
            Ok((n, th)) => {
                total += n;
                hist.iter_mut().zip(th).for_each(|(a, b)| *a += b);
            }
            Err(e) => {
                eprintln!("client: {e}");
                std::process::exit(1);
            }
        }
    }
    let rps = total as f64 / start.elapsed().as_secs_f64();
    let n: u64 = hist.iter().sum();
    let q = |p: f64| {
        let want = ((n as f64 * p).ceil() as u64).max(1);
        let mut acc = 0;
        let i = hist.iter().position(|&c| {
            acc += c;
            acc >= want
        });
        i.map_or(0.0, |i| lower(i) as f64 / 1e3)
    };
    println!("{total} {rps:.0} {:.1} {:.1} {:.1}", q(0.5), q(0.99), q(0.999));
}
