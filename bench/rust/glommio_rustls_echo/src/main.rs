// TLS echo server for bench/run.sh: argv[1] = port, argv[2] = executors (1). glommio_echo plus
// futures-rustls on glommio's TcpStream; config, certificate and buffer from tls_cfg.
use futures_lite::io::{AsyncReadExt, AsyncWriteExt};
use futures_rustls::TlsAcceptor;
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
    let port: u16 = std::env::args().nth(1).expect("usage: glommio_rustls_echo port [executors]").parse().unwrap();
    let n: usize = std::env::args().nth(2).map_or(1, |n| n.parse().unwrap());
    let cpus = allowed_cpus();
    assert!(n <= cpus.len(), "{n} executors but only {} allowed CPUs", cpus.len());
    let cfg = tls_cfg::server_config();
    let nodelay = tls_cfg::nodelay();
    let hs: Vec<_> = cpus[..n]
        .iter()
        .map(|&cpu| {
            let acceptor = TlsAcceptor::from(cfg.clone());
            LocalExecutorBuilder::new(Placement::Fixed(cpu))
                .spawn(move || async move {
                    let l = TcpListener::bind(("127.0.0.1", port)).unwrap();
                    loop {
                        let s = l.accept().await.unwrap();
                        if nodelay {
                            s.set_nodelay(true).unwrap();
                        }
                        let acceptor = acceptor.clone();
                        glommio::spawn_local(async move {
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
