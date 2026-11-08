// Echo server for bench/run.sh: argv[1] = port, argv[2] = uring|epoll. 2 threads, each
// with its own runtime (io_uring or legacy/epoll driver) and SO_REUSEPORT listener.
use monoio::buf::IoBuf;
use monoio::io::{AsyncReadRent, AsyncWriteRentExt};
use monoio::net::{ListenerOpts, TcpListener};
use monoio::{IoUringDriver, LegacyDriver, RuntimeBuilder};

async fn serve(port: u16) {
    let opts = ListenerOpts::new().reuse_port(true).backlog(4096);
    let l = TcpListener::bind_with_config(("127.0.0.1", port), &opts).unwrap();
    loop {
        let (mut s, _) = l.accept().await.unwrap();
        monoio::spawn(async move {
            let mut buf = vec![0u8; 16384];
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
    let (port, uring): (u16, bool) = match a.as_slice() {
        [_, p, d] if d == "uring" || d == "epoll" => (p.parse().unwrap(), d == "uring"),
        _ => panic!("usage: monoio_echo port uring|epoll"),
    };
    let ts: Vec<_> = (0..2)
        .map(|_| {
            std::thread::spawn(move || {
                if uring {
                    RuntimeBuilder::<IoUringDriver>::new().enable_all().build().unwrap().block_on(serve(port))
                } else {
                    RuntimeBuilder::<LegacyDriver>::new().enable_all().build().unwrap().block_on(serve(port))
                }
            })
        })
        .collect();
    for t in ts {
        t.join().unwrap();
    }
}
