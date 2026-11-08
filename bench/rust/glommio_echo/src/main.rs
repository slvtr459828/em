// Echo server for bench/run.sh: argv[1] = port, argv[2] = executors (1). Executors are pinned to the first
// CPUs of the allowed set (taskset), each with its own SO_REUSEPORT listener (glommio sets it in TcpListener::bind).
use futures_lite::io::{AsyncReadExt, AsyncWriteExt};
use glommio::net::TcpListener;
use glommio::{LocalExecutorBuilder, Placement};

// CPUs this process may run on, from the "Cpus_allowed_list" line, e.g. "0-1,4".
fn allowed_cpus() -> Vec<usize> {
    let st = std::fs::read_to_string("/proc/self/status").unwrap();
    let l = st.lines().find_map(|l| l.strip_prefix("Cpus_allowed_list:")).unwrap();
    l.trim()
        .split(',')
        .flat_map(|r| {
            let (a, b) = r.split_once('-').unwrap_or((r, r));
            a.parse::<usize>().unwrap()..=b.parse().unwrap()
        })
        .collect()
}

fn main() {
    let port: u16 = std::env::args().nth(1).expect("usage: glommio_echo port [executors]").parse().unwrap();
    let n: usize = std::env::args().nth(2).map_or(1, |n| n.parse().unwrap());
    let cpus = allowed_cpus();
    assert!(n <= cpus.len(), "{n} executors but only {} allowed CPUs", cpus.len());
    let hs: Vec<_> = cpus[..n]
        .iter()
        .map(|&cpu| {
            LocalExecutorBuilder::new(Placement::Fixed(cpu))
                .spawn(move || async move {
                    let l = TcpListener::bind(("127.0.0.1", port)).unwrap();
                    loop {
                        let mut s = l.accept().await.unwrap();
                        glommio::spawn_local(async move {
                            let mut buf = vec![0u8; 16384];
                            loop {
                                match s.read(&mut buf).await {
                                    Ok(n) if n > 0 => {
                                        if s.write_all(&buf[..n]).await.is_err() {
                                            break;
                                        }
                                    }
                                    _ => break,
                                }
                            }
                        })
                        .detach();
                    }
                })
                .unwrap()
        })
        .collect();
    for h in hs {
        h.join().unwrap();
    }
}
