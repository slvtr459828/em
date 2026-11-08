// Closed-loop ping-pong load generator, same for every server.
// usage: client addr conns size seconds threads
// Each connection sends `size` bytes, reads exactly `size` back, compares, repeats.
// Prints "total_round_trips round_trips_per_sec"; exits 1 if any echo is short or differs.
use std::time::{Duration, Instant};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;

async fn conn(addr: String, size: usize, deadline: Instant) -> std::io::Result<u64> {
    let mut s = TcpStream::connect(addr).await?;
    s.set_nodelay(true)?;
    let out: Vec<u8> = (0..size).map(|i| (i * 31 + 7) as u8).collect();
    let mut inb = vec![0u8; size];
    let mut n = 0u64;
    while Instant::now() < deadline {
        s.write_all(&out).await?;
        s.read_exact(&mut inb).await?;
        if inb != out {
            return Err(std::io::Error::new(std::io::ErrorKind::InvalidData, "echo mismatch"));
        }
        n += 1;
    }
    Ok(n)
}

fn main() {
    let a: Vec<String> = std::env::args().collect();
    if a.len() != 6 {
        eprintln!("usage: client addr conns size seconds threads");
        std::process::exit(2);
    }
    let conns: usize = a[2].parse().unwrap();
    let size: usize = a[3].parse().unwrap();
    let secs: f64 = a[4].parse().unwrap();
    let threads: usize = a[5].parse().unwrap();
    let start = Instant::now();
    let deadline = start + Duration::from_secs_f64(secs);
    let hs: Vec<_> = (0..threads)
        .map(|t| {
            let addr = a[1].clone();
            let mine = conns / threads + (t < conns % threads) as usize;
            std::thread::spawn(move || {
                let rt = tokio::runtime::Builder::new_current_thread().enable_all().build().unwrap();
                rt.block_on(async {
                    let js: Vec<_> = (0..mine).map(|_| tokio::spawn(conn(addr.clone(), size, deadline))).collect();
                    let mut total = 0;
                    for j in js {
                        total += j.await.unwrap()?;
                    }
                    Ok::<u64, std::io::Error>(total)
                })
            })
        })
        .collect();
    let mut total = 0;
    for h in hs {
        match h.join().unwrap() {
            Ok(n) => total += n,
            Err(e) => {
                eprintln!("client: {e}");
                std::process::exit(1);
            }
        }
    }
    println!("{total} {:.0}", total as f64 / start.elapsed().as_secs_f64());
}
