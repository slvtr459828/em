// TLS echo server for bench/run.sh: argv[1] = port, argv[2] = threads (1). compio_echo plus compio's
// rustls integration (compio-tls); config, certificate and buffer from tls_cfg.
use compio::buf::{IntoInner, IoBuf};
use compio::io::{AsyncRead, AsyncWrite, AsyncWriteExt};
use compio::net::{TcpListener, TcpSocket};
use compio::tls::TlsAcceptor;

async fn listen(port: u16) -> TcpListener {
    let s = TcpSocket::new_v4().await.unwrap();
    s.set_reuseaddr(true).unwrap();
    s.set_reuseport(true).unwrap();
    s.bind(([127, 0, 0, 1], port).into()).await.unwrap();
    s.listen(4096).await.unwrap()
}

fn main() {
    let port: u16 = std::env::args().nth(1).expect("usage: compio_rustls_echo port [threads]").parse().unwrap();
    let n: usize = std::env::args().nth(2).map_or(1, |n| n.parse().unwrap());
    let cfg = tls_cfg::server_config();
    let nodelay = tls_cfg::nodelay();
    let ts: Vec<_> = (0..n)
        .map(|_| {
            let acceptor = TlsAcceptor::from(cfg.clone());
            std::thread::spawn(move || {
                compio::runtime::Runtime::new().unwrap().block_on(async move {
                    let l = listen(port).await;
                    loop {
                        let (s, _) = l.accept().await.unwrap();
                        if nodelay {
                            s.set_nodelay(true).unwrap();
                        }
                        let acceptor = acceptor.clone();
                        compio::runtime::spawn(async move {
                            let Ok(mut s) = acceptor.accept(s).await else { return };
                            let mut buf = tls_cfg::echo_buf();
                            loop {
                                let (r, b) = s.read(buf).await.into();
                                buf = b;
                                match r {
                                    Ok(n) if n > 0 => {
                                        let (w, b) = s.write_all(buf.slice(..n)).await.into();
                                        buf = b.into_inner();
                                        // the compat layer under compio-tls buffers writes: flush sends the record
                                        if w.is_err() || s.flush().await.is_err() {
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
            })
        })
        .collect();
    for t in ts {
        t.join().unwrap();
    }
}
