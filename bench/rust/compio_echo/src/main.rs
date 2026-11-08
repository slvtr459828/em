// Echo server for bench/run.sh: argv[1] = port, argv[2] = threads (1). Each thread has its own
// runtime (driver picked by the uring/epoll feature) and SO_REUSEPORT listener.
use compio::buf::{IntoInner, IoBuf};
use compio::io::{AsyncRead, AsyncWriteExt};
use compio::net::{TcpListener, TcpSocket};

async fn listen(port: u16) -> TcpListener {
    let s = TcpSocket::new_v4().await.unwrap();
    s.set_reuseaddr(true).unwrap();
    s.set_reuseport(true).unwrap();
    s.bind(([127, 0, 0, 1], port).into()).await.unwrap();
    s.listen(4096).await.unwrap()
}

fn main() {
    let port: u16 = std::env::args().nth(1).expect("usage: compio_echo port [threads]").parse().unwrap();
    let n: usize = std::env::args().nth(2).map_or(1, |n| n.parse().unwrap());
    let ts: Vec<_> = (0..n)
        .map(|_| {
            std::thread::spawn(move || {
                compio::runtime::Runtime::new().unwrap().block_on(async move {
                    let l = listen(port).await;
                    loop {
                        let (mut s, _) = l.accept().await.unwrap();
                        compio::runtime::spawn(async move {
                            let mut buf = vec![0u8; 16384];
                            loop {
                                let (r, b) = s.read(buf).await.into();
                                buf = b;
                                match r {
                                    Ok(n) if n > 0 => {
                                        let (w, b) = s.write_all(buf.slice(..n)).await.into();
                                        buf = b.into_inner();
                                        if w.is_err() {
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
