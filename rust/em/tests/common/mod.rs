use std::net::{Ipv4Addr, SocketAddr};

/// A loopback address with a port that was free a moment ago (listeners set SO_REUSEPORT).
pub fn local_addr() -> SocketAddr {
    let l = std::net::TcpListener::bind((Ipv4Addr::LOCALHOST, 0)).unwrap();
    l.local_addr().unwrap()
}
