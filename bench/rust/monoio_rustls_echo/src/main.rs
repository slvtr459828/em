// TLS echo server for bench/run.sh: argv[1] = port, argv[2] = uring|epoll, argv[3] = threads (1).
// monoio_echo plus monoio-rustls; config, certificate and buffer from tls_cfg.
use monoio::buf::IoBuf;
use monoio::io::{AsyncReadRent, AsyncWriteRentExt};
use monoio::net::{ListenerOpts, TcpListener};
use monoio::{IoUringDriver, LegacyDriver, RuntimeBuilder};
use monoio_rustls::TlsAcceptor;

async fn serve(port: u16, acceptor: TlsAcceptor) {
    let nodelay = tls_cfg::nodelay();
    let opts = ListenerOpts::new().reuse_port(true).backlog(4096);
    let l = TcpListener::bind_with_config(("127.0.0.1", port), &opts).unwrap();
    loop {
        let (s, _) = l.accept().await.unwrap();
        if nodelay {
            s.set_nodelay(true).unwrap();
        }
        let acceptor = acceptor.clone();
        monoio::spawn(async move {
            let Ok(mut s) = acceptor.accept(s).await else { return };
            let mut buf = tls_cfg::echo_buf();
            loop {
                let (r, b) = s.read(buf).await;
                buf = b;
                match r {
                    Ok(n) if n > 0 => {
                        let (w, b) = s.write_all(buf.slice(..n)).await;
                        buf = b.into_inner();
                        if w.is_err() {
                            break;
                        }
                    }
                    _ => break,
                }
            }
        });
    }
}

fn main() {
    let a: Vec<String> = std::env::args().collect();
    let (port, uring): (u16, bool) = match a.get(1..3) {
        Some([p, d]) if d == "uring" || d == "epoll" => (p.parse().unwrap(), d == "uring"),
        _ => panic!("usage: monoio_rustls_echo port uring|epoll [threads]"),
    };
    let n: usize = a.get(3).map_or(1, |n| n.parse().unwrap());
    let cfg = tls_cfg::server_config();
    let ts: Vec<_> = (0..n)
        .map(|_| {
            let acceptor = TlsAcceptor::from(cfg.clone());
            std::thread::spawn(move || {
                if uring {
                    RuntimeBuilder::<IoUringDriver>::new().enable_all().build().unwrap().block_on(serve(port, acceptor))
                } else {
                    RuntimeBuilder::<LegacyDriver>::new().enable_all().build().unwrap().block_on(serve(port, acceptor))
                }
            })
        })
        .collect();
    for t in ts {
        t.join().unwrap();
    }
}
