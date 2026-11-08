// Echo server for bench/run.sh: argv[1] = port. 2 executors pinned to CPUs 0 and 1,
// each with its own SO_REUSEPORT listener (glommio sets it in TcpListener::bind).
use futures_lite::io::{AsyncReadExt, AsyncWriteExt};
use glommio::net::TcpListener;
use glommio::{LocalExecutorBuilder, Placement};

fn main() {
    let port: u16 = std::env::args().nth(1).expect("usage: glommio_echo port").parse().unwrap();
    let hs: Vec<_> = (0..2)
        .map(|cpu| {
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
