/* A writer that meets a reader which is mid-flush must wait for it, and wake when it ends.
 * Setup: the server's kernel send path to the client is filled with raw bytes (the client never
 * reads TLS again), the client sends a garbage TLS record, so the server's em_tls_recv fails and
 * has an alert to flush: it blocks in em_send as the flusher. (A KeyUpdate answer would do too,
 * but OpenSSL sends that one with the next write, not from inside SSL_read.) Then the server's
 * writer calls em_tls_send_all: it must stay parked behind the flusher.
 *  - client drains the socket: the flush completes and both calls return.
 *  - client resets the connection: the flush fails and both calls return too (no hang).
 * Both calls report -EPROTO (the stream is in a fatal state anyway); what is checked is that the
 * writer waited (>= 50 ms) and woke. */
#include <stdio.h>
#include <sys/socket.h>
#include "tls_util.h"

static SSL_CTX *sctx, *cctx;
static uint16_t port;
static int rres[2], wres[2];
static int64_t wms[2];
static int done[2];

static void small_buffers(int fd)
{
    int v = 16384;
    assert(setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &v, sizeof v) == 0);
    assert(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &v, sizeof v) == 0);
}

struct h { em_tls *t; int i; };

static int reader(void *arg)
{
    struct h *h = arg;
    char b[16];
    ssize_t n = em_tls_recv(h->t, b, sizeof b);
    rres[h->i] = (int)n;
    return 0;
}

static int writer(void *arg)
{
    struct h *h = arg;
    em_sleep_until(em_now() + 400000000); /* the reader is blocked in its flush by now */
    int64_t t0 = em_now();
    wres[h->i] = em_tls_send_all(h->t, "y", 1);
    wms[h->i] = (em_now() - t0) / 1000000;
    return 0;
}

static int server(void *arg)
{
    (void)arg;
    int lfd = listen_local(&port);
    for (int i = 0; i < 2; i++) {
        int fd = em_accept(lfd);
        if (fd < 0) return fd;
        small_buffers(fd);
        em_tls *t;
        int r = em_tls_accept(&t, sctx, fd);
        if (r < 0) return r;
        char junk[4096] = {0};
        /* fill until nothing more is accepted for 4 rounds of 30 ms (the kernel keeps taking a little more for ~100 ms: window updates) */
        for (int quiet = 0; quiet < 4; ) {
            int sent = 0;
            while (send(fd, junk, sizeof junk, MSG_DONTWAIT | MSG_NOSIGNAL) > 0) sent = 1;
            while (send(fd, junk, 1, MSG_DONTWAIT | MSG_NOSIGNAL) > 0) sent = 1;
            quiet = sent ? 0 : quiet + 1;
            em_sleep_until(em_now() + 30000000);
        }
        struct h h = { t, i };
        em_scope s;
        em_scope_open(&s, 0);
        assert(em_spawn(&s, reader, &h) == 0);
        assert(em_spawn(&s, writer, &h) == 0);
        em_scope_close(&s);
        em_tls_free(t);
        em_close(fd);
        done[i] = 1;
    }
    em_close(lfd);
    return 0;
}

static int client(void *arg)
{
    (void)arg;
    while (!port) em_yield();
    for (int i = 0; i < 2; i++) {
        int fd = connect_local(port);
        assert(fd >= 0);
        small_buffers(fd);
        em_tls *t;
        assert(em_tls_connect(&t, cctx, fd, "localhost") == 0);
        em_sleep_until(em_now() + 500000000); /* server has filled its send path */
        assert(em_send_all(fd, "\x99\x03\x03\x00\x05hello", 10) == 0); /* not a TLS record type */
        em_sleep_until(em_now() + 400000000);
        if (i == 0) { /* drain what the server sent, until it goes quiet */
            char b[4096];
            for (;;) {
                em_scope s;
                em_scope_open(&s, em_now() + 50000000);
                ssize_t n = em_recv(fd, b, sizeof b);
                em_scope_close(&s);
                if (n <= 0) break;
            }
        } else {
            struct linger l = { 1, 0 };
            setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof l);
            em_close(fd); /* RST: the server's flush fails */
            fd = -1;
        }
        while (!done[i]) em_sleep_until(em_now() + 1000000);
        em_tls_free(t);
        if (fd >= 0) em_close(fd);
    }
    return 0;
}

static int main_fn(void *arg)
{
    (void)arg;
    em_scope s;
    em_scope_open(&s, 0);
    assert(em_spawn(&s, server, NULL) == 0);
    assert(em_spawn(&s, client, NULL) == 0);
    return em_scope_close(&s);
}

int main(void)
{
    tls_test_ctx(&sctx, &cctx);
    assert(em_run(1, main_fn, NULL) == 0);
    printf("drain: reader %d, writer %d after %lld ms\n", rres[0], wres[0], (long long)wms[0]);
    printf("reset: reader %d, writer %d after %lld ms\n", rres[1], wres[1], (long long)wms[1]);
    for (int i = 0; i < 2; i++) assert(rres[i] == -EPROTO && wres[i] < 0 && wms[i] >= 50);
    SSL_CTX_free(sctx);
    SSL_CTX_free(cctx);
    return 0;
}
