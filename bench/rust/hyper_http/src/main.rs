// hyper as the HTTP rival of bench/run.sh (plans/http.md): usage `hyper_http port [workers] [tls]`.
// http1 only, current_thread runtime (one thread; workers is accepted for the common launch line),
// pipeline_flush(true) like TechEmpower's hyper. tls: tokio-rustls with tls_cfg::server_config(), the
// configuration of every Rust TLS server of the bench.
// GET / -> "Hello, World!" (13 B), GET /4k -> 4096 B, both text/plain; else 404. hyper adds Content-Length and Date.
use std::convert::Infallible;
use bytes::Bytes;
use http_body_util::Full;
use hyper::server::conn::http1;
use hyper::service::service_fn;
use hyper::{header, Request, Response, StatusCode};
use hyper_util::rt::TokioIo;
use tokio::net::TcpListener;
use tokio_rustls::TlsAcceptor;

static BIG: [u8; 4096] = [b'x'; 4096];

async fn handle(req: Request<hyper::body::Incoming>) -> Result<Response<Full<Bytes>>, Infallible> {
    let (status, body) = match req.uri().path() {
        "/" => (StatusCode::OK, Bytes::from_static(b"Hello, World!")),
        "/4k" => (StatusCode::OK, Bytes::from_static(&BIG)),
        _ => (StatusCode::NOT_FOUND, Bytes::from_static(b"Not Found\n")),
    };
    let mut r = Response::new(Full::new(body));
    *r.status_mut() = status;
    r.headers_mut().insert(header::CONTENT_TYPE, header::HeaderValue::from_static("text/plain"));
    Ok(r)
}

fn main() -> std::io::Result<()> {
    let a: Vec<String> = std::env::args().collect();
    if a.len() < 2 || a.len() > 4 || (a.len() == 4 && a[3] != "tls") {
        eprintln!("usage: {} port [workers] [tls]", a[0]);
        std::process::exit(2);
    }
    let port: u16 = a[1].parse().expect("port");
    let acceptor = (a.len() == 4).then(|| TlsAcceptor::from(tls_cfg::server_config()));
    let nodelay = tls_cfg::nodelay();
    let rt = tokio::runtime::Builder::new_current_thread().enable_all().build()?;
    rt.block_on(async {
        let l = TcpListener::bind(("127.0.0.1", port)).await?;
        loop {
            let (s, _) = l.accept().await?;
            s.set_nodelay(nodelay)?;
            let acceptor = acceptor.clone();
            tokio::spawn(async move {
                let mut b = http1::Builder::new();
                b.pipeline_flush(true);
                let _ = match acceptor {
                    None => b.serve_connection(TokioIo::new(s), service_fn(handle)).await,
                    Some(t) => match t.accept(s).await {
                        Ok(s) => b.serve_connection(TokioIo::new(s), service_fn(handle)).await,
                        Err(_) => return,
                    },
                };
            });
        }
    })
}
