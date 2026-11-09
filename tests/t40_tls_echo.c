/* TLS echo, 1000 round-trips of 1 B..64 KiB, byte-checked. Once with client and server on the same
 * worker, once on two workers. */
#include <stdatomic.h>
#include <stdio.h>
#include "tls_util.h"

#define ROUNDS 1000
#define MAXSZ  (64u * 1024u)

static SSL_CTX *sctx, *cctx;
extern unsigned long em_tls_dbg_partial; /* src/tls.c test hook */
static _Atomic int port;
static int nw;

static int server(void *arg)
{
    (void)arg;
    uint16_t p;
    int lfd = listen_local(&p);
    atomic_store(&port, p);
    int fd = em_accept(lfd);
    if (fd < 0) return fd;
    em_tls *t;
    int r = em_tls_accept(&t, sctx, fd);
    if (r < 0) return r;
    char buf[16384];
    for (;;) {
        ssize_t n = em_tls_recv(t, buf, sizeof buf);
        if (n < 0) { r = (int)n; break; }
        if (n == 0) { r = em_tls_shutdown(t); break; } /* clean end: answer with our close_notify */
        if ((r = em_tls_send_all(t, buf, (size_t)n)) < 0) break;
    }
    em_tls_free(t);
    em_close(fd);
    em_close(lfd);
    return r;
}

static int client(void *arg)
{
    (void)arg;
    while (!atomic_load(&port)) em_sleep_until(em_now() + 1000000);
    int fd = connect_local((uint16_t)atomic_load(&port));
    if (fd < 0) return fd;
    em_tls *t;
    int r = em_tls_connect(&t, cctx, fd, "localhost");
    if (r < 0) return r;
    static char out[MAXSZ], in[MAXSZ];
    uint64_t s = 88172645463325252ull;
    for (int i = 0; i < ROUNDS && !r; i++) {
        size_t n = i % 50 == 0 ? MAXSZ : 1 + xs_next(&s) % (i % 3 ? 3000 : 64);
        for (size_t j = 0; j < n; j++) out[j] = (char)(xs_next(&s) >> 24);
        if ((r = em_tls_send_all(t, out, n)) < 0) break;
        for (size_t got = 0; got < n;) {
            ssize_t k = em_tls_recv(t, in + got, n - got);
            if (k <= 0) { r = k < 0 ? (int)k : -EIO; break; }
            got += (size_t)k;
        }
        if (!r && memcmp(in, out, n) != 0) r = -EBADMSG;
    }
    if (!r) r = em_tls_shutdown(t);
    if (!r) { ssize_t k = em_tls_recv(t, in, 1); if (k != 0) r = -EIO; } /* server's close_notify */
    em_tls_free(t);
    em_close(fd);
    return r;
}

static int main_fn(void *arg)
{
    (void)arg;
    int id = em_worker_id();
    em_scope s;
    em_scope_open(&s, 0);
    if (nw == 1 || id == 0) assert(em_spawn(&s, server, NULL) == 0);
    if (nw == 1 || id == 1) assert(em_spawn(&s, client, NULL) == 0);
    return em_scope_close(&s);
}

int main(void)
{
    tls_test_ctx(&sctx, &cctx);
    for (int pass = 0; pass < 4; pass++) {
        nw = pass % 2 + 1;
        if (pass == 2) {
            /* largest output per SSL_write: TLS 1.2, CBC + SHA384 (biggest per-record expansion),
             * 512 B records (32 per call). It must still fit the out buffer whole. */
            SSL_CTX_set_max_proto_version(cctx, TLS1_2_VERSION);
#ifdef OPENSSL_IS_AWSLC
            /* AWS-LC has no CBC-SHA256/384 suites; CBC + SHA1 is its largest per-record expansion */
#define CBC_SUITE "ECDHE-ECDSA-AES256-SHA"
#else
#define CBC_SUITE "ECDHE-ECDSA-AES256-SHA384:@SECLEVEL=0"
#endif
            assert(SSL_CTX_set_cipher_list(cctx, CBC_SUITE) == 1);
            assert(SSL_CTX_set_cipher_list(sctx, CBC_SUITE) == 1);
            SSL_CTX_set_max_send_fragment(sctx, 512);
            SSL_CTX_set_max_send_fragment(cctx, 512);
        }
        atomic_store(&port, 0);
        assert(em_run(nw, main_fn, NULL) == 0);
        printf("tls echo: %d round-trips over %d worker(s)%s verified\n", ROUNDS, nw,
               pass >= 2 ? ", TLS 1.2 CBC-SHA384 512 B records," : "");
    }
    assert(em_tls_dbg_partial == 0); /* no SSL_write ever left a record half-written */
    SSL_CTX_free(sctx);
    SSL_CTX_free(cctx);
    return 0;
}
