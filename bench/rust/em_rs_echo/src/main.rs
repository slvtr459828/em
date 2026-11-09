// Echo server for bench/run.sh: argv[1] = port, argv[2] = workers (default 1). Same shape as
// bench/em_echo.c: every worker listens with SO_REUSEPORT, one fiber per connection.
use std::io::{Read, Write};
use std::net::{Ipv4Addr, SocketAddr};

fn main() -> std::io::Result<()> {
    let mut args = std::env::args().skip(1);
    let port: u16 = args.next().expect("usage: em_rs_echo port [workers]").parse().unwrap();
    let workers: usize = args.next().map_or(1, |n| n.parse().unwrap());
    let addr = SocketAddr::from((Ipv4Addr::LOCALHOST, port));
    em::run(workers, || {
        let l = em::TcpListener::bind(addr)?;
        em::scope(|s| loop {
            let mut c = l.accept()?;
            s.spawn(move || {
                let mut buf = [0u8; 16384];
                // a dropped connection must not cancel the others: swallow its error
                while let Ok(n @ 1..) = c.read(&mut buf) {
                    if c.write_all(&buf[..n]).is_err() {
                        break;
                    }
                }
                Ok(())
            })?;
        })
    })
}
