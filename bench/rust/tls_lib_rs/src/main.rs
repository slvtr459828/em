// rustls + ring twin of bench/tls_lib.c (plans/tls-perf.md, step 2): the server's TLS work alone, in
// memory, same ops, config and CSV. usage: tls_lib_rs <tickets: 0|stateless|stateful> <iters>
// stdout: lib,op,tickets,variant,ns_per_op,instr_per_op,cycles_per_op (see tls_lib.c for the meaning).
// rustls has no BIO: the server's read_tls/process_new_packets/reader/writer/write_tls are the
// counterpart of SSL_read/SSL_write/SSL_do_handshake, and are what is measured.
use std::io::{Read, Write};
use std::sync::Arc;
use std::time::Instant;

use rustls::client::Resumption;
use rustls::crypto::{ring, CryptoProvider};
use rustls::pki_types::{PrivatePkcs8KeyDer, ServerName};
use rustls::{
    ClientConfig, ClientConnection, ConnectionCommon, HandshakeKind, RootCertStore, ServerConfig, ServerConnection,
    SideData,
};

const BASE: usize = 0;
const SYS: usize = 1;
const TOTAL: usize = 2;
const VAR: [&str; 3] = ["base", "sys", "total"];

// perf_event_attr as 8 u64 (PERF_ATTR_SIZE_VER0): type|size<<32, config, _, _, read_format, flags
const PERF_FORMAT_GROUP: u64 = 8;
const DISABLED: u64 = 1;
const EXCLUDE_KERNEL_HV: u64 = 1 << 5 | 1 << 6;
const IOC_ENABLE: libc::c_ulong = 0x2400;
const IOC_DISABLE: libc::c_ulong = 0x2401;
const IOC_RESET: libc::c_ulong = 0x2403;
const IOC_FLAG_GROUP: libc::c_ulong = 1;

struct Conn {
    s: ServerConnection,
    c: ClientConnection,
}

struct B {
    var: usize,
    pfd: i32, // perf group leader (instructions); -1 = no PMU
    ns: f64,
    t0: Instant,
    sc: Arc<ServerConfig>,
    cfull: Arc<ClientConfig>,
    cres: Arc<ClientConfig>,
    cbuf: Vec<u8>,
    sbuf: Vec<u8>,
    buf: Vec<u8>,
    rk: Option<Conn>,
    rsz: usize,
}

fn perf_open() -> i32 {
    let open = |config: u64, flags: u64, group: i32| unsafe {
        let a: [u64; 8] = [64 << 32, config, 0, 0, PERF_FORMAT_GROUP, flags | EXCLUDE_KERNEL_HV, 0, 0];
        libc::syscall(libc::SYS_perf_event_open, a.as_ptr(), 0, -1, group, 0) as i32
    };
    let l = open(1, DISABLED, -1); // PERF_COUNT_HW_INSTRUCTIONS
    if l >= 0 && open(0, 0, l) < 0 {
        unsafe { libc::close(l) };
        return -1;
    }
    l
}

impl B {
    fn pc(&self, req: libc::c_ulong) {
        if self.pfd >= 0 {
            unsafe { libc::ioctl(self.pfd, req as _, IOC_FLAG_GROUP) };
        }
    }
    fn mb(&mut self) {
        if self.var == TOTAL {
            return;
        }
        if self.var == SYS {
            unsafe { libc::syscall(libc::SYS_getppid) };
        }
        self.pc(IOC_ENABLE);
        self.t0 = Instant::now();
    }
    fn me(&mut self) {
        if self.var == TOTAL {
            return;
        }
        self.ns += self.t0.elapsed().as_nanos() as f64;
        self.pc(IOC_DISABLE);
    }
}

fn feed<S: SideData>(c: &mut ConnectionCommon<S>, mut b: &[u8]) {
    while !b.is_empty() {
        c.read_tls(&mut b).unwrap();
        c.process_new_packets().unwrap();
    }
}
fn drain<S: SideData>(c: &mut ConnectionCommon<S>, out: &mut Vec<u8>) {
    while c.wants_write() {
        c.write_tls(out).unwrap();
    }
}

fn connect(b: &mut B, resume: bool) -> Conn {
    b.mb();
    let mut s = ServerConnection::new(b.sc.clone()).unwrap();
    b.me();
    let cc = if resume { b.cres.clone() } else { b.cfull.clone() };
    let mut c = ClientConnection::new(cc, ServerName::try_from("localhost").unwrap()).unwrap();
    for round in 0.. {
        assert!(round < 8, "handshake did not finish");
        b.cbuf.clear();
        drain(&mut c, &mut b.cbuf);
        b.sbuf.clear();
        if !b.cbuf.is_empty() {
            b.mb();
            feed(&mut s, &b.cbuf);
            drain(&mut s, &mut b.sbuf);
            b.me();
        }
        if b.cbuf.is_empty() && b.sbuf.is_empty() {
            break;
        }
        feed(&mut c, &b.sbuf); // the last round carries the tickets
    }
    assert!(!s.is_handshaking() && !c.is_handshaking());
    Conn { s, c }
}

fn disconnect(b: &mut B, k: Conn, resumed: bool) {
    let want = if resumed { HandshakeKind::Resumed } else { HandshakeKind::Full };
    assert!(k.s.handshake_kind() == Some(want), "expected {want:?} handshake");
    let mut s = k.s;
    b.sbuf.clear();
    b.mb();
    s.send_close_notify(); // em_tls sends close_notify before freeing
    drain(&mut s, &mut b.sbuf);
    drop(s);
    b.me();
}

fn it_rec(b: &mut B) {
    let mut k = b.rk.take().unwrap();
    let n = b.rsz;
    k.c.writer().write_all(&b.buf[..n]).unwrap();
    b.cbuf.clear();
    drain(&mut k.c, &mut b.cbuf);
    b.sbuf.clear();
    b.mb();
    feed(&mut k.s, &b.cbuf);
    let mut tmp = [0u8; 16384];
    let r = k.s.reader().read(&mut tmp).unwrap();
    k.s.writer().write_all(&tmp[..r]).unwrap();
    drain(&mut k.s, &mut b.sbuf);
    b.me();
    assert!(r == n);
    feed(&mut k.c, &b.sbuf);
    assert!(k.c.reader().read(&mut tmp).unwrap() == n);
    b.rk = Some(k);
}
fn it_full(b: &mut B) {
    let k = connect(b, false);
    disconnect(b, k, false);
}
fn it_resume(b: &mut B) {
    let k = connect(b, true);
    disconnect(b, k, true);
}

fn run(b: &mut B, tickets: &str, op: &str, it: fn(&mut B), iters: u64) {
    for v in BASE..=TOTAL {
        b.var = v;
        for _ in 0..iters / 10 + 10 {
            it(b);
        }
        b.pc(IOC_RESET);
        b.ns = 0.0;
        if v == TOTAL {
            b.pc(IOC_ENABLE);
            b.t0 = Instant::now();
        }
        for _ in 0..iters {
            it(b);
        }
        if v == TOTAL {
            b.ns = b.t0.elapsed().as_nanos() as f64;
            b.pc(IOC_DISABLE);
        }
        let (mut ins, mut cyc) = ("n/a".to_string(), "n/a".to_string());
        let mut g = [0u64; 3];
        if b.pfd >= 0 && unsafe { libc::read(b.pfd, g.as_mut_ptr().cast(), 24) } == 24 {
            ins = format!("{:.1}", g[1] as f64 / iters as f64);
            cyc = format!("{:.1}", g[2] as f64 / iters as f64);
        }
        println!("rustls-ring,{op},{tickets},{},{:.1},{ins},{cyc}", VAR[v], b.ns / iters as f64);
    }
}

fn main() {
    let mut args = std::env::args().skip(1);
    let usage = "usage: tls_lib_rs 0|stateless|stateful iters";
    let tickets = args.next().expect(usage);
    let iters: u64 = args.next().expect(usage).parse().unwrap();
    let provider = Arc::new(CryptoProvider {
        cipher_suites: vec![ring::cipher_suite::TLS13_AES_256_GCM_SHA384],
        kx_groups: vec![ring::kx_group::X25519],
        ..ring::default_provider()
    });
    let ck = rcgen::generate_simple_self_signed(vec!["localhost".to_string()]).unwrap();
    let key = PrivatePkcs8KeyDer::from(ck.key_pair.serialize_der());
    let mut sc = ServerConfig::builder_with_provider(provider.clone())
        .with_protocol_versions(&[&rustls::version::TLS13])
        .unwrap()
        .with_no_client_auth()
        .with_single_cert(vec![ck.cert.der().clone()], key.into())
        .unwrap();
    sc.send_tls13_tickets = 2;
    match tickets.as_str() {
        "0" => sc.send_tls13_tickets = 0,
        "stateless" => sc.ticketer = ring::Ticketer::new().unwrap(),
        "stateful" => {} // rustls default: ServerSessionMemoryCache, ticket = session id
        _ => panic!("{usage}"),
    }
    let mut roots = RootCertStore::empty();
    roots.add(ck.cert.der().clone()).unwrap();
    let mut cfull = ClientConfig::builder_with_provider(provider)
        .with_protocol_versions(&[&rustls::version::TLS13])
        .unwrap()
        .with_root_certificates(roots)
        .with_no_client_auth();
    let cres = Arc::new(cfull.clone()); // default: in-memory session store
    cfull.resumption = Resumption::disabled();

    let mut b = B {
        var: BASE,
        pfd: perf_open(),
        ns: 0.0,
        t0: Instant::now(),
        sc: Arc::new(sc),
        cfull: Arc::new(cfull),
        cres,
        cbuf: vec![],
        sbuf: vec![],
        buf: vec![b'x'; 16384],
        rk: None,
        rsz: 0,
    };
    // a first resumable handshake: checks the negotiated parameters and fills the client's session store
    let k = connect(&mut b, true);
    eprintln!(
        "# lib=rustls-ring (rustls 0.23) tickets={tickets} iters={iters} proto={:?} cipher={:?} perf={}",
        k.s.protocol_version().unwrap(),
        k.s.negotiated_cipher_suite().unwrap().suite(),
        if b.pfd >= 0 { "on" } else { "n/a" }
    );
    disconnect(&mut b, k, false);
    b.rk = Some(connect(&mut b, false));
    b.rsz = 128;
    run(&mut b, &tickets, "rec128", it_rec, iters);
    b.rsz = 4096;
    run(&mut b, &tickets, "rec4096", it_rec, iters);
    run(&mut b, &tickets, "full", it_full, iters);
    if tickets != "0" {
        // fresh tickets: the full ops above may have pushed the first ones out of the server's cache
        let k = connect(&mut b, true);
        drop(k);
        run(&mut b, &tickets, "resume", it_resume, iters);
    } else {
        eprintln!("# resume skipped: no tickets, nothing to resume");
    }
}
