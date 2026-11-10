// Server TLS configuration and echo buffer shared by every TLS echo server of bench/run.sh, so the
// servers differ only in runtime and TLS glue: self-signed certificate, aws-lc-rs provider, stateless
// tickets (2 per connection, like em_tls on AWS-LC), 16 KiB echo buffer.
use std::sync::Arc;
use rustls::crypto::aws_lc_rs::{default_provider, Ticketer};
use rustls::pki_types::PrivatePkcs8KeyDer;
use rustls::ServerConfig;

pub const ECHO_BUF: usize = 16384;

/// The echo buffer: one heap allocation, the same in every server.
pub fn echo_buf() -> Vec<u8> {
    vec![0u8; ECHO_BUF]
}

pub fn server_config() -> Arc<ServerConfig> {
    let ck = rcgen::generate_simple_self_signed(vec!["localhost".to_string()]).unwrap();
    let key = PrivatePkcs8KeyDer::from(ck.key_pair.serialize_der());
    let mut cfg = ServerConfig::builder_with_provider(Arc::new(default_provider()))
        .with_safe_default_protocol_versions()
        .unwrap()
        .with_no_client_auth()
        .with_single_cert(vec![ck.cert.der().clone()], key.into())
        .unwrap();
    // TLS_TICKETS (bench/tls_ab.sh): unset or stateless = Ticketer (encrypted tickets), 2 per connection;
    // 0 = no tickets; stateful = rustls's default memory cache.
    match std::env::var("TLS_TICKETS").as_deref() {
        Err(_) | Ok("stateless") => {
            cfg.ticketer = Ticketer::new().unwrap();
            cfg.send_tls13_tickets = 2;
        }
        Ok("0") => cfg.send_tls13_tickets = 0,
        Ok("stateful") => cfg.send_tls13_tickets = 2,
        Ok(v) => {
            eprintln!("TLS_TICKETS={v}: want 0, stateless or stateful");
            std::process::exit(2);
        }
    }
    Arc::new(cfg)
}

/// TCP_NODELAY is set per connection by the servers (else Nagle holds the echo behind the session
/// tickets for a delayed ACK, ~40 ms); NODELAY=0 turns it off (bench/tls_ab.sh).
pub fn nodelay() -> bool {
    std::env::var("NODELAY").as_deref() != Ok("0")
}
