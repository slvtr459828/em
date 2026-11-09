//! TLS needs no code in this crate: `rustls::StreamOwned` runs on `em::TcpStream` because that is
//! `Read + Write` and parks the fiber inside. (C users get `em_tls`; see include/em_tls.h.)
mod common;

use std::io::{self, Read, Write};
use std::sync::Arc;

use rustls::pki_types::{PrivatePkcs8KeyDer, ServerName};
use rustls::{ClientConfig, ClientConnection, RootCertStore, ServerConfig, ServerConnection, StreamOwned};

fn io_err(e: rustls::Error) -> io::Error {
    io::Error::new(io::ErrorKind::Other, e)
}

/// Self-signed ECDSA P-256 certificate for "localhost", in memory; the client trusts only it.
fn configs() -> (Arc<ServerConfig>, Arc<ClientConfig>) {
    let ck = rcgen::generate_simple_self_signed(vec!["localhost".to_string()]).unwrap();
    let cert = ck.cert.der().clone();
    let key = PrivatePkcs8KeyDer::from(ck.key_pair.serialize_der());
    let provider = Arc::new(rustls::crypto::ring::default_provider());
    let server = ServerConfig::builder_with_provider(provider.clone())
        .with_safe_default_protocol_versions()
        .unwrap()
        .with_no_client_auth()
        .with_single_cert(vec![cert.clone()], key.into())
        .unwrap();
    let mut roots = RootCertStore::empty();
    roots.add(cert).unwrap();
    let client = ClientConfig::builder_with_provider(provider)
        .with_safe_default_protocol_versions()
        .unwrap()
        .with_root_certificates(roots)
        .with_no_client_auth();
    (Arc::new(server), Arc::new(client))
}

// conformance: same semantics as C test t40 (echo)
#[test]
fn tls_echo_over_tcpstream() {
    let addr = common::local_addr();
    let (sc, cc) = configs();
    em::run(1, || {
        let l = em::TcpListener::bind(addr)?;
        em::scope(|s| {
            s.spawn(|| {
                let tcp = l.accept()?;
                let mut tls = StreamOwned::new(ServerConnection::new(sc.clone()).map_err(io_err)?, tcp);
                let mut buf = [0u8; 4096];
                loop {
                    let n = tls.read(&mut buf)?; // Ok(0) = close_notify; truncation would be an error
                    if n == 0 {
                        break;
                    }
                    tls.write_all(&buf[..n])?;
                }
                tls.conn.send_close_notify();
                tls.flush()
            })?;
            s.spawn(|| {
                let tcp = em::TcpStream::connect(addr)?;
                let name = ServerName::try_from("localhost").unwrap();
                let mut tls = StreamOwned::new(ClientConnection::new(cc.clone(), name).map_err(io_err)?, tcp);
                for i in 0..50usize {
                    let n = 1 + (i * 997) % 40000;
                    let msg: Vec<u8> = (0..n).map(|j| (j * 31 + i) as u8).collect();
                    tls.write_all(&msg)?;
                    let mut got = vec![0u8; n];
                    tls.read_exact(&mut got)?;
                    assert_eq!(got, msg);
                }
                tls.conn.send_close_notify();
                tls.flush()?;
                let mut rest = Vec::new();
                assert_eq!(tls.read_to_end(&mut rest)?, 0); // the server's close_notify
                Ok(())
            })?;
            Ok(())
        })
    })
    .unwrap();
}
