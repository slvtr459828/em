mod common;

use std::io::{BufRead, BufReader, Read, Write};
use std::net::SocketAddr;
use std::time::Duration;

/// 1. Existing sync code (`BufReader::read_line`) runs unchanged on `&TcpStream`.
// conformance: same semantics as C test t7 (echo)
#[test]
fn echo_lines_through_bufreader() {
    let addr = common::local_addr();
    em::run(1, || {
        let l = em::TcpListener::bind(addr)?;
        em::scope(|s| {
            s.spawn(|| {
                let c = l.accept()?;
                let mut r = BufReader::new(&c);
                let mut line = String::new();
                while r.read_line(&mut line)? > 0 {
                    (&c).write_all(line.to_uppercase().as_bytes())?;
                    line.clear();
                }
                Ok(())
            })?;
            s.spawn(|| {
                let mut c = em::TcpStream::connect(addr)?;
                let mut r = BufReader::new(&c);
                for i in 0..100 {
                    let msg = format!("hello {i}\n");
                    (&c).write_all(msg.as_bytes())?;
                    let mut got = String::new();
                    r.read_line(&mut got)?;
                    assert_eq!(got, msg.to_uppercase());
                }
                c.shutdown(std::net::Shutdown::Write)?;
                let mut rest = Vec::new();
                assert_eq!(c.read_to_end(&mut rest)?, 0);
                Ok(())
            })?;
            Ok(())
        })
    })
    .unwrap();
}

/// 7. UDP both ways.
// conformance: same semantics as C test t21 (udp)
#[test]
fn udp_ping_pong() {
    let srv_addr = common::local_addr();
    em::run(1, || {
        let srv = em::UdpSocket::bind(srv_addr)?;
        let cli = em::UdpSocket::bind("127.0.0.1:0".parse::<SocketAddr>().unwrap())?;
        em::scope_timeout(Duration::from_secs(5), |s| {
            s.spawn(|| {
                let mut b = [0u8; 64];
                for _ in 0..10 {
                    let (n, from) = srv.recv_from(&mut b)?;
                    assert_eq!(srv.send_to(&b[..n], from)?, n);
                }
                Ok(())
            })?;
            s.spawn(|| {
                let mut b = [0u8; 64];
                for i in 0..10u8 {
                    assert_eq!(cli.send_to(&[i, 7, 7], srv_addr)?, 3);
                    let (n, from) = cli.recv_from(&mut b)?;
                    assert_eq!((&b[..n], from), (&[i, 7, 7][..], srv_addr));
                }
                Ok(())
            })?;
            Ok(())
        })
    })
    .unwrap();
}

/// A signalfd/eventfd-like fd through read_fd (a pipe here), written from another fiber.
// conformance: same semantics as C test t20 (em_read on an fd)
#[test]
fn read_fd_pipe() {
    use std::os::fd::{AsFd, AsRawFd, FromRawFd, OwnedFd};
    let mut fds = [0; 2];
    assert_eq!(unsafe { libc_pipe(fds.as_mut_ptr()) }, 0);
    let (rd, wr) = unsafe { (OwnedFd::from_raw_fd(fds[0]), OwnedFd::from_raw_fd(fds[1])) };
    em::run(1, || {
        em::scope(|s| {
            s.spawn(|| {
                let mut b = [0u8; 8];
                let n = em::read_fd(rd.as_fd(), &mut b)?;
                assert_eq!(&b[..n], b"ping");
                Ok(())
            })?;
            s.spawn(|| {
                em::sleep(Duration::from_millis(5))?;
                let w = unsafe { libc_write(wr.as_raw_fd(), b"ping".as_ptr(), 4) };
                assert_eq!(w, 4);
                Ok(())
            })?;
            Ok(())
        })
    })
    .unwrap();
}

extern "C" {
    #[link_name = "pipe"]
    fn libc_pipe(fds: *mut i32) -> i32;
    #[link_name = "write"]
    fn libc_write(fd: i32, buf: *const u8, n: usize) -> isize;
}
