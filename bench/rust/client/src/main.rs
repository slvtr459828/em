// Closed-loop ping-pong load generator, same for every server.
// usage: client addr conns size seconds threads [churn] [tls|tlsres] [http [path=/|/4k] [pipe=N]]
// Each connection sends `size` bytes, reads exactly `size` back, compares, repeats.
// churn: each of `conns` loops instead does connect, one round-trip, close with RST
// (zero linger: no TIME_WAIT, so ephemeral ports do not run out); counts connections.
// tls: every connection does a full TLS handshake first (rustls via tokio-rustls, client session
// resumption disabled, certificate not verified: the servers use throwaway self-signed ones);
// churn then counts handshake + round-trip. tlsres: same, but the client keeps a session store and
// resumes when it can. With either, a second output line says what was negotiated and how many
// handshakes were full / resumed ("tls <version> <suite> <kx group> <cert key> full=N resumed=M").
// http (plans/http.md): `size` is the expected response body length. Each connection sends
// `pipe` (default 1) "GET <path> HTTP/1.1" requests in one write, reads that many responses (httparse),
// requires status 200 and Content-Length == size, and counts `pipe` round-trips per batch (the batch's
// latency is recorded for each). churn: connect, one request, RST. A second output line says how big the
// first response was: "http response_bytes=N" (header + body). With tls: the same over TLS (a full
// handshake per connection, as in the echo cells; the keep-alive cell handshakes once per connection).
// Prints "total_round_trips round_trips_per_sec p50_us p99_us p999_us"; exits 1 if any echo is
// short or differs. Latency of every round-trip goes into a per-thread log-linear histogram
// (128 sub-buckets per power of two, < 1% error; fixed array, nothing allocated while measuring).
use std::cell::RefCell;
use std::sync::atomic::{AtomicU64, Ordering::Relaxed};
use std::sync::{Arc, OnceLock};
use std::time::{Duration, Instant};
use rustls::client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier};
use rustls::pki_types::{CertificateDer, ServerName, UnixTime};
use rustls::client::Resumption;
use rustls::{ClientConfig, HandshakeKind, DigitallySignedStruct, SignatureScheme};
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio_rustls::client::TlsStream;
use tokio_rustls::TlsConnector;

#[derive(Debug)]
struct AcceptAny(Arc<rustls::crypto::CryptoProvider>);

impl ServerCertVerifier for AcceptAny {
    fn verify_server_cert(&self, _: &CertificateDer<'_>, _: &[CertificateDer<'_>], _: &ServerName<'_>, _: &[u8], _: UnixTime) -> Result<ServerCertVerified, rustls::Error> {
        Ok(ServerCertVerified::assertion())
    }
    fn verify_tls12_signature(&self, m: &[u8], c: &CertificateDer<'_>, d: &DigitallySignedStruct) -> Result<HandshakeSignatureValid, rustls::Error> {
        rustls::crypto::verify_tls12_signature(m, c, d, &self.0.signature_verification_algorithms)
    }
    fn verify_tls13_signature(&self, m: &[u8], c: &CertificateDer<'_>, d: &DigitallySignedStruct) -> Result<HandshakeSignatureValid, rustls::Error> {
        rustls::crypto::verify_tls13_signature(m, c, d, &self.0.signature_verification_algorithms)
    }
    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        self.0.signature_verification_algorithms.supported_schemes()
    }
}

static FULL: AtomicU64 = AtomicU64::new(0);
static RESUMED: AtomicU64 = AtomicU64::new(0);
static RESP_BYTES: AtomicU64 = AtomicU64::new(0);
static NEGOTIATED: OnceLock<String> = OnceLock::new();

fn tls_connector(resume: bool) -> TlsConnector {
    let provider = Arc::new(rustls::crypto::ring::default_provider());
    let cfg = ClientConfig::builder_with_provider(provider.clone())
        .with_safe_default_protocol_versions()
        .unwrap()
        .dangerous()
        .with_custom_certificate_verifier(Arc::new(AcceptAny(provider)))
        .with_no_client_auth();
    let mut cfg = cfg;
    if !resume {
        cfg.resumption = Resumption::disabled();
    }
    TlsConnector::from(Arc::new(cfg))
}

enum Conn {
    Plain(TcpStream),
    Tls(Box<TlsStream<TcpStream>>),
}

impl Conn {
    async fn open(addr: &str, tls: Option<&TlsConnector>, linger0: bool) -> std::io::Result<Conn> {
        let tcp = TcpStream::connect(addr).await?;
        tcp.set_nodelay(true)?;
        if linger0 {
            tcp.set_zero_linger()?; // drop = RST
        }
        Ok(match tls {
            None => Conn::Plain(tcp),
            Some(c) => {
                let s = c.connect(ServerName::try_from("localhost").unwrap(), tcp).await?;
                let cc = s.get_ref().1;
                match cc.handshake_kind() {
                    Some(HandshakeKind::Resumed) => RESUMED.fetch_add(1, Relaxed),
                    _ => FULL.fetch_add(1, Relaxed),
                };
                NEGOTIATED.get_or_init(|| {
                    // P-256 OID 1.2.840.10045.3.1.7 inside the server's certificate
                    const P256: [u8; 10] = [0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07];
                    let key = cc.peer_certificates().and_then(|c| c.first()).map_or("?", |c| {
                        if c.windows(P256.len()).any(|w| w == P256) { "ecdsa-p256" } else { "other" }
                    });
                    format!("{:?} {:?} {:?} {key}", cc.protocol_version().unwrap(), cc.negotiated_cipher_suite().unwrap().suite(),
                            cc.negotiated_key_exchange_group().map(|g| g.name()))
                });
                Conn::Tls(Box::new(s))
            }
        })
    }

    async fn round_trip(&mut self, out: &[u8], inb: &mut [u8]) -> std::io::Result<()> {
        match self {
            Conn::Plain(s) => {
                s.write_all(out).await?;
                s.read_exact(inb).await?;
            }
            Conn::Tls(s) => {
                s.write_all(out).await?;
                s.flush().await?;
                s.read_exact(inb).await?;
            }
        }
        Ok(())
    }
}

const SUB: usize = 128; // values below SUB ns are exact; above, 7 bits of mantissa
const NB: usize = (64 - 6) * SUB;

fn bucket(ns: u64) -> usize {
    if ns < SUB as u64 {
        return ns as usize;
    }
    let e = 63 - ns.leading_zeros() as usize; // >= 7
    (e - 6) * SUB + (ns >> (e - 7)) as usize - SUB
}

fn lower(i: usize) -> u64 {
    if i < SUB {
        return i as u64;
    }
    ((i % SUB + SUB) as u64) << (i / SUB + 6 - 7)
}

#[derive(Clone)]
struct Http {
    req: Vec<u8>, // `pipe` requests back to back
    pipe: usize,
    body: usize,
}

fn bad(m: &str) -> std::io::Error {
    std::io::Error::new(std::io::ErrorKind::InvalidData, m.to_string())
}

// One batch: write all requests, read until `pipe` complete responses arrived and checked.
async fn http_batch<S: AsyncRead + AsyncWrite + Unpin>(s: &mut S, h: &Http, buf: &mut [u8]) -> std::io::Result<()> {
    s.write_all(&h.req).await?;
    s.flush().await?; // TLS: the records are written here
    let (mut len, mut got) = (0, 0);
    while got < h.pipe {
        let n = s.read(&mut buf[len..]).await?;
        if n == 0 {
            return Err(std::io::ErrorKind::UnexpectedEof.into());
        }
        len += n;
        let mut off = 0;
        while got < h.pipe {
            let mut hdrs = [httparse::EMPTY_HEADER; 32];
            let mut r = httparse::Response::new(&mut hdrs);
            let hl = match r.parse(&buf[off..len]).map_err(|_| bad("http: bad response"))? {
                httparse::Status::Complete(hl) => hl,
                httparse::Status::Partial => break,
            };
            if r.code != Some(200) {
                return Err(bad("http: status is not 200"));
            }
            let cl = r.headers.iter().find(|x| x.name.eq_ignore_ascii_case("content-length"));
            let cl = cl.and_then(|x| std::str::from_utf8(x.value).ok()?.trim().parse::<usize>().ok());
            if cl != Some(h.body) {
                return Err(bad("http: missing or wrong Content-Length"));
            }
            if len - off < hl + h.body {
                break;
            }
            RESP_BYTES.store((hl + h.body) as u64, Relaxed);
            off += hl + h.body;
            got += 1;
        }
        buf.copy_within(off..len, 0);
        len -= off;
    }
    if len != 0 {
        return Err(bad("http: bytes after the last response"));
    }
    Ok(())
}

async fn http_conn(addr: String, h: Http, deadline: Instant, churn: bool, tls: Option<TlsConnector>) -> std::io::Result<u64> {
    let mut s = Conn::open(&addr, tls.as_ref(), churn).await?;
    let mut buf = vec![0u8; h.pipe * (h.body + 1024)];
    let mut n = 0u64;
    let mut t = Instant::now();
    while t < deadline {
        if churn && n > 0 {
            drop(s);
            s = Conn::open(&addr, tls.as_ref(), true).await?;
        }
        match &mut s {
            Conn::Plain(c) => http_batch(c, &h, &mut buf).await?,
            Conn::Tls(c) => http_batch(&mut **c, &h, &mut buf).await?,
        }
        let t2 = Instant::now();
        HIST.with(|x| x.borrow_mut()[bucket((t2 - t).as_nanos() as u64)] += h.pipe as u64);
        t = t2;
        n += h.pipe as u64;
    }
    Ok(n)
}

thread_local!(static HIST: RefCell<Vec<u64>> = RefCell::new(vec![0; NB]));

async fn conn(addr: String, size: usize, deadline: Instant, churn: bool, tls: Option<TlsConnector>) -> std::io::Result<u64> {
    let mut s = Conn::open(&addr, tls.as_ref(), churn).await?;
    let out: Vec<u8> = (0..size).map(|i| (i * 31 + 7) as u8).collect();
    let mut inb = vec![0u8; size];
    let mut n = 0u64;
    let mut t = Instant::now();
    while t < deadline {
        if churn && n > 0 {
            drop(s);
            s = Conn::open(&addr, tls.as_ref(), true).await?;
        }
        s.round_trip(&out, &mut inb).await?;
        let t2 = Instant::now();
        if inb != out {
            return Err(std::io::Error::new(std::io::ErrorKind::InvalidData, "echo mismatch"));
        }
        HIST.with(|h| h.borrow_mut()[bucket((t2 - t).as_nanos() as u64)] += 1);
        t = t2;
        n += 1;
    }
    Ok(n)
}

fn main() {
    let a: Vec<String> = std::env::args().collect();
    let flags = &a[a.len().min(6)..];
    let http = flags.iter().any(|f| f == "http");
    let opt = |k: &str| flags.iter().find_map(|f| f.strip_prefix(k));
    let pipe: usize = opt("pipe=").map_or(1, |v| v.parse().unwrap_or(0));
    let path = opt("path=").unwrap_or("/");
    let known = |f: &String| matches!(f.as_str(), "churn" | "tls" | "tlsres" | "http") || f.starts_with("path=") || f.starts_with("pipe=");
    if a.len() < 6 || !flags.iter().all(known) || pipe == 0 || (!http && (opt("path=").is_some() || opt("pipe=").is_some())) {
        eprintln!("usage: client addr conns size seconds threads [churn] [tls|tlsres] [http [path=/|/4k] [pipe=N]]");
        std::process::exit(2);
    }
    let resume = flags.iter().any(|f| f == "tlsres");
    let tls = resume || flags.iter().any(|f| f == "tls");
    let conns: usize = a[2].parse().unwrap();
    let size: usize = a[3].parse().unwrap();
    let secs: f64 = a[4].parse().unwrap();
    let threads: usize = a[5].parse().unwrap();
    let churn = flags.iter().any(|f| f == "churn");
    let h = http.then(|| Http { req: format!("GET {path} HTTP/1.1\r\nHost: localhost\r\n\r\n").repeat(if churn { 1 } else { pipe }).into(), pipe: if churn { 1 } else { pipe }, body: size });
    let start = Instant::now();
    let deadline = start + Duration::from_secs_f64(secs);
    let hs: Vec<_> = (0..threads)
        .map(|t| {
            let addr = a[1].clone();
            let h = h.clone();
            let conn_tls = tls.then(|| tls_connector(resume));
            let mine = conns / threads + (t < conns % threads) as usize;
            std::thread::spawn(move || {
                let rt = tokio::runtime::Builder::new_current_thread().enable_all().build().unwrap();
                rt.block_on(async {
                    let js: Vec<_> = (0..mine).map(|_| {
                        match &h {
                            Some(h) => tokio::spawn(http_conn(addr.clone(), h.clone(), deadline, churn, conn_tls.clone())),
                            None => tokio::spawn(conn(addr.clone(), size, deadline, churn, conn_tls.clone())),
                        }
                    }).collect();
                    let mut total = 0;
                    for j in js {
                        total += j.await.unwrap()?;
                    }
                    Ok::<(u64, Vec<u64>), std::io::Error>((total, HIST.take()))
                })
            })
        })
        .collect();
    let mut total = 0;
    let mut hist = vec![0u64; NB];
    for h in hs {
        match h.join().unwrap() {
            Ok((n, th)) => {
                total += n;
                hist.iter_mut().zip(th).for_each(|(a, b)| *a += b);
            }
            Err(e) => {
                eprintln!("client: {e}");
                std::process::exit(1);
            }
        }
    }
    let rps = total as f64 / start.elapsed().as_secs_f64();
    let n: u64 = hist.iter().sum();
    let q = |p: f64| {
        let want = ((n as f64 * p).ceil() as u64).max(1);
        let mut acc = 0;
        let i = hist.iter().position(|&c| {
            acc += c;
            acc >= want
        });
        i.map_or(0.0, |i| lower(i) as f64 / 1e3)
    };
    println!("{total} {rps:.0} {:.1} {:.1} {:.1}", q(0.5), q(0.99), q(0.999));
    if http {
        println!("http response_bytes={}", RESP_BYTES.load(Relaxed));
    }
    if let Some(n) = NEGOTIATED.get() {
        println!("tls {n} full={} resumed={}", FULL.load(Relaxed), RESUMED.load(Relaxed));
    }
}
