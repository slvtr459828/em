// TLS echo server for bench/run.sh: argv[1] = port, argv[2] = workers (1). tokio-rustls; config, certificate
// and buffer from tls_cfg. TOKIO_RT=current_thread: a current_thread runtime instead of multi_thread (the
// server `tokio-rustls-ct`); n is ignored there (always 1 thread).
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpListener;
use tokio_rustls::TlsAcceptor;

fn main() -> std::io::Result<()> {
    let port: u16 = std::env::args().nth(1).expect("usage: tokio_rustls_echo port").parse().unwrap();
    let n: usize = std::env::args().nth(2).map_or(1, |n| n.parse().unwrap());
    let nodelay = tls_cfg::nodelay();
    let acceptor = TlsAcceptor::from(tls_cfg::server_config());
    let rt = match std::env::var("TOKIO_RT").as_deref() {
        Err(_) | Ok("multi_thread") => tokio::runtime::Builder::new_multi_thread().worker_threads(n).enable_all().build()?,
        Ok("current_thread") => tokio::runtime::Builder::new_current_thread().enable_all().build()?,
        Ok(v) => {
            eprintln!("TOKIO_RT={v}: want multi_thread or current_thread");
            std::process::exit(2);
        }
    };
    rt.block_on(async {
        let l = TcpListener::bind(("127.0.0.1", port)).await?;
        loop {
            let (s, _) = l.accept().await?;
            let acceptor = acceptor.clone();
            s.set_nodelay(nodelay)?; // else Nagle holds the echo behind the session tickets for a delayed ACK (~40 ms)
            tokio::spawn(async move {
                let Ok(mut s) = acceptor.accept(s).await else { return };
                let mut buf = tls_cfg::echo_buf();
                loop {
                    match s.read(&mut buf).await {
                        Ok(n) if n > 0 => {
                            if s.write_all(&buf[..n]).await.is_err() || s.flush().await.is_err() {
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
