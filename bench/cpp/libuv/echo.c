/* libuv echo server for bench/run.sh: argv[1] = port, argv[2] = workers (default 1). Built twice
 * (build.sh): plain, and -DWITH_TLS with OpenSSL/AWS-LC over memory BIOs (libuv has no TLS).
 *
 * One uv_loop per worker thread (epoll), each listening on 127.0.0.1:port with SO_REUSEPORT.
 * Plain: alloc_cb hands out the connection's own 16 KiB buffer; a read is written back with
 *   uv_try_write first (the common case on loopback: no request, no completion callback) and only the
 *   remainder goes through uv_write. Reading is stopped while a uv_write is pending, so the buffer is
 *   not reused under it (one in-flight echo per connection, as in the other servers).
 * TLS: per connection one SSL with a read mem BIO (bytes from the socket) and a write mem BIO (bytes for
 *   the socket). Each read callback feeds the read BIO, drives SSL_accept / SSL_read+SSL_write until
 *   SSL_ERROR_WANT_READ, then sends everything the write BIO collected with one uv_try_write (uv_write
 *   for the remainder). The socket read buffer and the plaintext buffer are per thread: both are
 *   consumed inside the callback. */
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#ifdef WITH_TLS
#include "../../../tests/tls_util.h"
#endif

#define BUF 16384

typedef struct conn {
    uv_tcp_t h;
    uv_write_t w;
#ifdef WITH_TLS
    SSL *ssl;
    BIO *rbio, *wbio;
    int hs_done;
#else
    char buf[BUF];
#endif
} conn;

static struct sockaddr_in addr;
static int nodelay = 1; /* NODELAY=0 turns it off */
#ifdef WITH_TLS
static SSL_CTX *sctx;
static int stateful; /* TLS_TICKETS=stateful */
static __thread char rbuf[BUF], pbuf[BUF];
#endif

static void on_close(uv_handle_t *h)
{
    conn *c = (conn *)h;
#ifdef WITH_TLS
    /* The churn client ends with RST, no close_notify: SSL_free would then drop the session from the
     * server cache and stateful resumption would almost never hit. Same workaround as em_tls_echo.c. */
    if (c->ssl) {
        if (stateful) SSL_set_shutdown(c->ssl, SSL_SENT_SHUTDOWN);
        SSL_free(c->ssl); /* frees both BIOs */
    }
#endif
    free(c);
}

static void close_conn(conn *c)
{
    if (!uv_is_closing((uv_handle_t *)&c->h)) uv_close((uv_handle_t *)&c->h, on_close);
}

static void alloc_cb(uv_handle_t *h, size_t hint, uv_buf_t *b)
{
    (void)hint;
#ifdef WITH_TLS
    (void)h;
    *b = uv_buf_init(rbuf, BUF);
#else
    *b = uv_buf_init(((conn *)h)->buf, BUF);
#endif
}

static void read_cb(uv_stream_t *s, ssize_t n, const uv_buf_t *b);

static void write_cb(uv_write_t *w, int status)
{
    conn *c = (conn *)((char *)w - offsetof(conn, w));
    if (status < 0) { close_conn(c); return; }
#ifdef WITH_TLS
    BIO_reset(c->wbio);
#endif
    if (uv_read_start((uv_stream_t *)&c->h, alloc_cb, read_cb) < 0) close_conn(c);
}

/* Send b[0..len): try inline, queue the remainder and pause reading until it is written. When `last`
 * the connection is about to be closed: a remainder is dropped, not queued. Returns 0 when everything
 * is written, 1 when a uv_write is pending, -1 on error (closed). */
static int send_all(conn *c, char *p, size_t len, int last)
{
    uv_buf_t b = uv_buf_init(p, (unsigned)len);
    int r = uv_try_write((uv_stream_t *)&c->h, &b, 1);
    if (r == UV_EAGAIN) r = 0;
    if (r < 0) { close_conn(c); return -1; }
    if ((size_t)r == len) return 0;
    if (last) return 0;
    uv_read_stop((uv_stream_t *)&c->h);
    b = uv_buf_init(p + r, (unsigned)(len - (size_t)r));
    if (uv_write(&c->w, (uv_stream_t *)&c->h, &b, 1, write_cb) < 0) { close_conn(c); return -1; }
    return 1;
}

#ifdef WITH_TLS
static void tls_drive(conn *c)
{
    int fail = 0;
    for (;;) {
        if (!c->hs_done) {
            int r = SSL_accept(c->ssl);
            if (r == 1) { c->hs_done = 1; continue; }
            if (SSL_get_error(c->ssl, r) != SSL_ERROR_WANT_READ) fail = 1;
            break;
        }
        int n = SSL_read(c->ssl, pbuf, sizeof pbuf);
        if (n > 0) {
            if (SSL_write(c->ssl, pbuf, n) != n) { fail = 1; break; }
            continue;
        }
        int e = SSL_get_error(c->ssl, n);
        if (e == SSL_ERROR_WANT_READ) break;
        if (e == SSL_ERROR_ZERO_RETURN) SSL_shutdown(c->ssl); /* queue our close_notify */
        fail = 1;
        break;
    }
    char *p;
    long len = BIO_get_mem_data(c->wbio, &p);
    if (len > 0) {
        int r = send_all(c, p, (size_t)len, fail);
        if (r != 0) return; /* pending write (it resets the BIO) or closed */
        BIO_reset(c->wbio);
    }
    if (fail) close_conn(c);
}
#endif

static void read_cb(uv_stream_t *s, ssize_t n, const uv_buf_t *b)
{
    conn *c = (conn *)s;
    if (n < 0) { close_conn(c); return; } /* UV_EOF or error */
    if (n == 0) return;
#ifdef WITH_TLS
    if (BIO_write(c->rbio, b->base, (int)n) != (int)n) { close_conn(c); return; }
    tls_drive(c);
#else
    send_all(c, b->base, (size_t)n, 0);
#endif
}

static void connection_cb(uv_stream_t *srv, int status)
{
    if (status < 0) return;
    conn *c = calloc(1, sizeof *c);
    if (!c) return;
    uv_tcp_init(srv->loop, &c->h);
    if (uv_accept(srv, (uv_stream_t *)&c->h) < 0) { uv_close((uv_handle_t *)&c->h, on_close); return; }
    uv_tcp_nodelay(&c->h, nodelay);
#ifdef WITH_TLS
    c->ssl = SSL_new(sctx);
    c->rbio = BIO_new(BIO_s_mem());
    c->wbio = BIO_new(BIO_s_mem());
    if (!c->ssl || !c->rbio || !c->wbio) { /* not yet owned by the SSL: free them here */
        SSL_free(c->ssl); BIO_free(c->rbio); BIO_free(c->wbio);
        c->ssl = NULL;
        close_conn(c);
        return;
    }
    SSL_set_bio(c->ssl, c->rbio, c->wbio);
    SSL_set_accept_state(c->ssl);
#endif
    if (uv_read_start((uv_stream_t *)&c->h, alloc_cb, read_cb) < 0) close_conn(c);
}

static void worker(void *arg)
{
    (void)arg;
    uv_loop_t loop;
    uv_tcp_t srv;
    int r = uv_loop_init(&loop);
    if (!r) r = uv_tcp_init(&loop, &srv);
    if (!r) r = uv_tcp_bind(&srv, (struct sockaddr *)&addr, UV_TCP_REUSEPORT);
    if (!r) r = uv_listen((uv_stream_t *)&srv, 4096, connection_cb);
    if (r) { fprintf(stderr, "listen: %s\n", uv_strerror(r)); exit(1); }
    uv_run(&loop, UV_RUN_DEFAULT);
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) { fprintf(stderr, "usage: %s port [workers]\n", argv[0]); return 2; }
    int workers = argc == 3 ? atoi(argv[2]) : 1;
    if (workers < 1) workers = 1;
    addr = (struct sockaddr_in){ .sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(argv[1])),
                                 .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    signal(SIGPIPE, SIG_IGN);
    const char *nd = getenv("NODELAY");
    if (nd && !strcmp(nd, "0")) nodelay = 0;
#ifdef WITH_TLS
    SSL_CTX *cctx;
    tls_test_ctx(&sctx, &cctx);
    SSL_CTX_free(cctx);
    /* TLS_TICKETS as in bench/em_tls_echo.c */
    const char *tk = getenv("TLS_TICKETS");
    if (tk) {
        if (!strcmp(tk, "0")) SSL_CTX_set_num_tickets(sctx, 0);
        else if (!strcmp(tk, "stateless")) SSL_CTX_set_num_tickets(sctx, 2);
        else if (!strcmp(tk, "stateful")) {
#ifdef OPENSSL_IS_AWSLC
            fprintf(stderr, "TLS_TICKETS=stateful: not supported on AWS-LC (no stateful TLS 1.3 tickets)\n");
            return 2;
#endif
            SSL_CTX_set_options(sctx, SSL_OP_NO_TICKET);
            SSL_CTX_set_session_cache_mode(sctx, SSL_SESS_CACHE_SERVER);
            SSL_CTX_set_session_id_context(sctx, (const unsigned char *)"em", 2);
            SSL_CTX_set_num_tickets(sctx, 2);
            stateful = 1;
        } else { fprintf(stderr, "TLS_TICKETS=%s: want 0, stateless or stateful\n", tk); return 2; }
    }
#endif
    uv_thread_t th[64];
    if (workers > 64) workers = 64;
    for (int i = 1; i < workers; i++)
        if (uv_thread_create(&th[i], worker, NULL)) { fprintf(stderr, "thread create failed\n"); return 1; }
    worker(NULL);
    return 1;
}
