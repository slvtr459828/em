use crate::ffi;
use crate::{cvt, errno};
use libc::{c_int, c_void, sockaddr, sockaddr_in, sockaddr_in6, sockaddr_storage, socklen_t};
use std::io::{self, Read, Write};
use std::marker::PhantomData;
use std::mem::{size_of, zeroed};
use std::net::{Ipv4Addr, Ipv6Addr, Shutdown, SocketAddr, SocketAddrV4, SocketAddrV6};
use std::os::fd::{AsFd, AsRawFd, BorrowedFd, RawFd};

/// An fd owned by the fiber runtime of one worker. `*const ()` makes it `!Send + !Sync`: the fd
/// table and `em_close` belong to the worker that created it (D2), so it must never cross
/// threads. Closed with `em_close` on drop (a plain `close` outside a fiber).
struct Fd(c_int, PhantomData<*const ()>);

impl Fd {
    fn new(r: c_int) -> io::Result<Fd> {
        cvt(r as isize).map(|fd| Fd(fd as c_int, PhantomData))
    }
}

impl Drop for Fd {
    fn drop(&mut self) {
        // SAFETY: the fd is ours; em_close always closes it and is safe outside a fiber.
        unsafe { ffi::em_close(self.0) };
    }
}

fn raw_addr(a: &SocketAddr) -> (sockaddr_storage, socklen_t) {
    // SAFETY: all-zero is a valid sockaddr_storage; the casts target types that fit in it
    // (it is at least as large and as aligned as every sockaddr_*).
    let mut st: sockaddr_storage = unsafe { zeroed() };
    let len = match a {
        SocketAddr::V4(a) => {
            let sa = sockaddr_in {
                sin_family: libc::AF_INET as _,
                sin_port: a.port().to_be(),
                sin_addr: libc::in_addr { s_addr: u32::from_ne_bytes(a.ip().octets()) },
                sin_zero: [0; 8],
            };
            unsafe { std::ptr::write(&mut st as *mut _ as *mut sockaddr_in, sa) };
            size_of::<sockaddr_in>()
        }
        SocketAddr::V6(a) => {
            let sa = sockaddr_in6 {
                sin6_family: libc::AF_INET6 as _,
                sin6_port: a.port().to_be(),
                sin6_flowinfo: a.flowinfo(),
                sin6_addr: libc::in6_addr { s6_addr: a.ip().octets() },
                sin6_scope_id: a.scope_id(),
            };
            unsafe { std::ptr::write(&mut st as *mut _ as *mut sockaddr_in6, sa) };
            size_of::<sockaddr_in6>()
        }
    };
    (st, len as socklen_t)
}

fn sock_addr(st: &sockaddr_storage, len: socklen_t) -> io::Result<SocketAddr> {
    let len = len as usize;
    // SAFETY: the kernel filled `len` bytes of `st`, and we only read what `len` covers.
    match st.ss_family as c_int {
        libc::AF_INET if len >= size_of::<sockaddr_in>() => {
            let a = unsafe { &*(st as *const _ as *const sockaddr_in) };
            let ip = Ipv4Addr::from(a.sin_addr.s_addr.to_ne_bytes());
            Ok(SocketAddr::V4(SocketAddrV4::new(ip, u16::from_be(a.sin_port))))
        }
        libc::AF_INET6 if len >= size_of::<sockaddr_in6>() => {
            let a = unsafe { &*(st as *const _ as *const sockaddr_in6) };
            let ip = Ipv6Addr::from(a.sin6_addr.s6_addr);
            Ok(SocketAddr::V6(SocketAddrV6::new(ip, u16::from_be(a.sin6_port), a.sin6_flowinfo, a.sin6_scope_id)))
        }
        _ => Err(io::Error::new(io::ErrorKind::InvalidData, "unexpected socket address family")),
    }
}

/// A listening TCP socket of the current worker (`SO_REUSEPORT`, backlog 1024: every worker
/// can bind the same address and the kernel spreads connections).
pub struct TcpListener(Fd);

impl TcpListener {
    pub fn bind(addr: SocketAddr) -> io::Result<TcpListener> {
        let (st, len) = raw_addr(&addr);
        // SAFETY: st/len describe a valid address for the duration of the call.
        Fd::new(unsafe { ffi::em_tcp_listen(&st as *const _ as *const sockaddr, len, 1024) }).map(TcpListener)
    }

    /// Waits for a connection. Cancellable like every blocking em call.
    pub fn accept(&self) -> io::Result<TcpStream> {
        Fd::new(unsafe { ffi::em_accept((self.0).0) }).map(TcpStream)
    }
}

/// A connected TCP socket. `Read + Write` for both `TcpStream` and `&TcpStream`, so one fiber
/// can read while another writes the same socket (as `std::net::TcpStream`).
pub struct TcpStream(Fd);

impl TcpStream {
    pub fn connect(addr: SocketAddr) -> io::Result<TcpStream> {
        let (st, len) = raw_addr(&addr);
        Fd::new(unsafe { ffi::em_connect(&st as *const _ as *const sockaddr, len) }).map(TcpStream)
    }

    pub fn shutdown(&self, how: Shutdown) -> io::Result<()> {
        let how = match how {
            Shutdown::Read => libc::SHUT_RD,
            Shutdown::Write => libc::SHUT_WR,
            Shutdown::Both => libc::SHUT_RDWR,
        };
        cvt(unsafe { ffi::em_shutdown((self.0).0, how) } as isize).map(drop)
    }
}

impl Read for &TcpStream {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        // SAFETY: the kernel writes straight into buf; the fiber does not resume until it is done.
        cvt(unsafe { ffi::em_recv((self.0).0, buf.as_mut_ptr() as *mut c_void, buf.len()) })
    }
}

impl Write for &TcpStream {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        cvt(unsafe { ffi::em_send((self.0).0, buf.as_ptr() as *const c_void, buf.len()) })
    }
    fn write_all(&mut self, buf: &[u8]) -> io::Result<()> {
        let r = unsafe { ffi::em_send_all((self.0).0, buf.as_ptr() as *const c_void, buf.len()) };
        if r < 0 { Err(errno(r)) } else { Ok(()) }
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

impl Read for TcpStream {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        (&*self).read(buf)
    }
}

impl Write for TcpStream {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        (&*self).write(buf)
    }
    fn write_all(&mut self, buf: &[u8]) -> io::Result<()> {
        (&*self).write_all(buf)
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

/// A UDP socket of the current worker. A nonzero port is bound with `SO_REUSEPORT`; port 0
/// (client socket) is not, so the ephemeral port stays unique.
pub struct UdpSocket(Fd);

impl UdpSocket {
    pub fn bind(addr: SocketAddr) -> io::Result<UdpSocket> {
        let (st, len) = raw_addr(&addr);
        Fd::new(unsafe { ffi::em_udp_bind(&st as *const _ as *const sockaddr, len) }).map(UdpSocket)
    }

    /// A datagram longer than `buf` is truncated.
    pub fn recv_from(&self, buf: &mut [u8]) -> io::Result<(usize, SocketAddr)> {
        let mut st: sockaddr_storage = unsafe { zeroed() };
        let mut len = size_of::<sockaddr_storage>() as socklen_t;
        let n = cvt(unsafe {
            ffi::em_recvfrom((self.0).0, buf.as_mut_ptr() as *mut c_void, buf.len(), &mut st as *mut _ as *mut sockaddr, &mut len)
        })?;
        Ok((n, sock_addr(&st, len)?))
    }

    pub fn send_to(&self, buf: &[u8], addr: SocketAddr) -> io::Result<usize> {
        let (st, len) = raw_addr(&addr);
        cvt(unsafe { ffi::em_sendto((self.0).0, buf.as_ptr() as *const c_void, buf.len(), &st as *const _ as *const sockaddr, len) })
    }
}

/// Reads from any fd (signalfd, eventfd, pipe) without blocking the worker. The fd stays
/// the caller's; on the epoll backend the first call sets `O_NONBLOCK` on it. A regular file
/// is read synchronously there and blocks the worker (D6).
///
/// On epoll, close such an fd only after every `read_fd` on it has returned, and from the same
/// worker: em keeps per-fd state for it, and an fd closed behind its back leaves that state to
/// the next fd with the same number, whose first wait would never wake (see `em_close` in em.h).
pub fn read_fd(fd: BorrowedFd<'_>, buf: &mut [u8]) -> io::Result<usize> {
    cvt(unsafe { ffi::em_read(fd.as_raw_fd(), buf.as_mut_ptr() as *mut c_void, buf.len()) })
}

macro_rules! fd_traits {
    ($($t:ty),*) => {$(
        impl AsRawFd for $t {
            fn as_raw_fd(&self) -> RawFd {
                (self.0).0
            }
        }
        impl AsFd for $t {
            fn as_fd(&self) -> BorrowedFd<'_> {
                // SAFETY: the fd is open for as long as `self` is.
                unsafe { BorrowedFd::borrow_raw((self.0).0) }
            }
        }
    )*};
}
fd_traits!(TcpListener, TcpStream, UdpSocket);
