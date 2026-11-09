// TLS echo server for bench/run.sh: argv[1] = port, argv[2] = workers (1). Multi-thread runtime,
// tokio-rustls; self-signed certificate generated at startup (the bench client accepts any).
use std::sync::Arc;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpListener;
use tokio_rustls::rustls::pki_types::PrivatePkcs8KeyDer;
use tokio_rustls::rustls::ServerConfig;
use tokio_rustls::TlsAcceptor;

fn main() -> std::io::Result<()> {
    let port: u16 = std::env::args().nth(1).expect("usage: tokio_rustls_echo port").parse().unwrap();
    let n: usize = std::env::args().nth(2).map_or(1, |n| n.parse().unwrap());
    let ck = rcgen::generate_simple_self_signed(vec!["localhost".to_string()]).unwrap();
    let key = PrivatePkcs8KeyDer::from(ck.key_pair.serialize_der());
    let provider = Arc::new(rustls::crypto::ring::default_provider());
    let cfg = ServerConfig::builder_with_provider(provider)
        .with_safe_default_protocol_versions()
        .unwrap()
        .with_no_client_auth()
        .with_single_cert(vec![ck.cert.der().clone()], key.into())
        .unwrap();
    let acceptor = TlsAcceptor::from(Arc::new(cfg));
    tokio::runtime::Builder::new_multi_thread().worker_threads(n).enable_all().build()?.block_on(async {
        let l = TcpListener::bind(("127.0.0.1", port)).await?;
        loop {
            let (s, _) = l.accept().await?;
            let acceptor = acceptor.clone();
            s.set_nodelay(true)?; // else Nagle holds the echo behind the session tickets for a delayed ACK (~40 ms)
            tokio::spawn(async move {
                let Ok(mut s) = acceptor.accept(s).await else { return };
                let mut buf = [0u8; 16384];
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
