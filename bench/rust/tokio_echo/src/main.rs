// Echo server for bench/run.sh: argv[1] = port. Multi-thread runtime, 2 workers.
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpListener;

fn main() -> std::io::Result<()> {
    let port: u16 = std::env::args().nth(1).expect("usage: tokio_echo port").parse().unwrap();
    tokio::runtime::Builder::new_multi_thread().worker_threads(2).enable_all().build()?.block_on(async {
        let l = TcpListener::bind(("127.0.0.1", port)).await?;
        loop {
            let (mut s, _) = l.accept().await?;
            tokio::spawn(async move {
                let mut buf = [0u8; 16384];
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
            });
        }
    })
}
