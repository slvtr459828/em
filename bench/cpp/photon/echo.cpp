// Photon echo server for bench/run.sh (plans/bench-cpp.md): argv[1] = port, argv[2] = workers
// (default 1). Built four times: -DTLS (OpenSSL through Photon's tls-stream) and -DURING (io_uring
// event engine and io_uring sockets; else epoll). One vCPU (OS thread with its own photon::init) per
// worker, each listening on the port with SO_REUSEPORT and one Photon thread per connection.
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <vector>
#include <photon/common/alog.h>
#include <photon/net/socket.h>
#include <photon/photon.h>
#ifdef TLS
#include <openssl/ssl.h>
#include <photon/net/security-context/tls-stream.h>
extern "C" void photon_cert_pem(char **cert, char **key); // cert.c: tests/tls_util.h certificate as PEM
#endif

using namespace photon;

static uint16_t port;
static int nodelay = 1; // NODELAY=0 turns it off (bench/tls_ab.sh Nagle cell)
#ifdef TLS
static net::TLSContext *tctx;
#endif

// Photon calls a handler as f(obj, stream): a plain function takes the (unused) obj first
static int echo(void *, net::ISocketStream *s)
{
    // After the handshake the server writes the session tickets and then the echo: without NODELAY
    // Nagle holds the echo behind the unacked tickets until the client's delayed ACK (~40 ms).
    s->setsockopt<int>(IPPROTO_TCP, TCP_NODELAY, nodelay);
    char buf[16384];
    for (;;) {
        ssize_t n = s->recv(buf, sizeof buf);
        if (n <= 0 || s->write(buf, (size_t)n) != n) break;
    }
    return 0; // the server deletes the stream; a dropped connection must not stop the others
}

static void worker()
{
#ifdef URING
    if (photon::init(INIT_EVENT_IOURING, INIT_IO_NONE) < 0) { fprintf(stderr, "photon::init(io_uring) failed\n"); exit(1); }
    net::ISocketServer *srv = net::new_iouring_tcp_server();
#else
    if (photon::init(INIT_EVENT_EPOLL, INIT_IO_NONE) < 0) { fprintf(stderr, "photon::init(epoll) failed\n"); exit(1); }
    net::ISocketServer *srv = net::new_tcp_socket_server();
#endif
    if (!srv) { fprintf(stderr, "new socket server failed\n"); exit(1); }
#ifdef TLS
    srv = net::new_tls_server(tctx, srv, true);
    if (!srv) { fprintf(stderr, "new_tls_server failed\n"); exit(1); }
#endif
    srv->set_handler({(void *)nullptr, echo});
    if (srv->setsockopt<int>(SOL_SOCKET, SO_REUSEPORT, 1) < 0 || srv->bind_v4localhost(port) < 0 || srv->listen(4096) < 0) {
        perror("bind/listen");
        exit(1);
    }
    srv->start_loop(true);
    delete srv;
    photon::fini();
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) { fprintf(stderr, "usage: %s port [workers]\n", argv[0]); return 2; }
    port = (uint16_t)atoi(argv[1]);
    int workers = argc == 3 ? atoi(argv[2]) : 1;
    const char *nd = getenv("NODELAY");
    if (nd && !strcmp(nd, "0")) nodelay = 0;
    set_log_output_level(ALOG_FATAL); // churn ends with RST: Photon logs every failed connection
#ifdef TLS
    char *cert, *key;
    photon_cert_pem(&cert, &key);
    // SSLv23 = TLS_method(): Photon's default TLSv1_2_method would pin TLS 1.2, the other servers speak 1.3
    tctx = net::new_tls_context(cert, key, nullptr, net::TLSVersion::SSL23);
    if (!tctx) { fprintf(stderr, "new_tls_context failed\n"); return 1; }
    // TLS_TICKETS: as bench/em_tls_echo.c. Photon keeps its SSL_CTX private, so reach it through the
    // object layout (vtable pointer, then `SSL_CTX *ctx` of TLSContextImpl in net/security-context/
    // tls-stream.cpp at the pinned version) and check it holds the certificate and key we set.
    // Debt: a layout change in a newer Photon makes this read a wrong pointer (crash or exit 2) when
    // TLS_TICKETS is set; the default run does not touch it. Gone when Photon exposes the SSL_CTX or
    // an option setter.
    const char *tk = getenv("TLS_TICKETS");
    if (tk) {
        SSL_CTX *c = ((SSL_CTX **)(void *)tctx)[1];
        if (SSL_CTX_get0_certificate(c) == nullptr || SSL_CTX_get0_privatekey(c) == nullptr) {
            fprintf(stderr, "TLS_TICKETS: unexpected TLSContext layout\n");
            return 2;
        }
        if (!strcmp(tk, "0")) SSL_CTX_set_num_tickets(c, 0);
        else if (!strcmp(tk, "stateless")) SSL_CTX_set_num_tickets(c, 2);
        else if (!strcmp(tk, "stateful")) {
#ifdef OPENSSL_IS_AWSLC
            fprintf(stderr, "TLS_TICKETS=stateful: not supported on AWS-LC (no stateful TLS 1.3 tickets)\n");
            return 2;
#endif
            SSL_CTX_set_options(c, SSL_OP_NO_TICKET);
            SSL_CTX_set_session_cache_mode(c, SSL_SESS_CACHE_SERVER);
            SSL_CTX_set_session_id_context(c, (const unsigned char *)"em", 2);
            SSL_CTX_set_num_tickets(c, 2);
        } else { fprintf(stderr, "TLS_TICKETS=%s: want 0, stateless or stateful\n", tk); return 2; }
    }
#endif
    std::vector<std::thread> th;
    for (int i = 1; i < workers; i++) th.emplace_back(worker);
    worker();
    for (auto &t : th) t.join();
    return 0;
}
