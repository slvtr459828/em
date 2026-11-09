/* Deadlines and cancellation.
 *  1. handshake with a silent peer under a 20 ms deadline: -ETIMEDOUT after ~20 ms, nothing leaks
 *     (ASan/LSan).
 *  2. em_tls_recv cancelled while waiting for bytes: intact, the next call gets the data.
 *  3. em_tls_send_all cancelled mid-send: broken, every later call is -EPIPE. */
#include <stdio.h>
#include "tls_util.h"

static SSL_CTX *sctx, *cctx;
static uint16_t port;

static int server(void *arg)
{
    (void)arg;
    int lfd = listen_local(&port);
    int fd0 = em_accept(lfd); /* 1: accepted, then silence */
    assert(fd0 >= 0);
    for (int i = 0; i < 2; i++) {
        int fd = em_accept(lfd);
        if (fd < 0) return fd;
        em_tls *t;
        int r = em_tls_accept(&t, sctx, fd);
        if (r < 0) return r;
        if (i == 0) { /* 2: speak late */
            em_sleep_until(em_now() + 80000000);
            if ((r = em_tls_send_all(t, "late", 4)) < 0) return r;
            char b;
            while (em_tls_recv(t, &b, 1) > 0) {} /* until the client leaves */
        } else {      /* 3: never read */
            em_sleep_until(em_now() + 400000000);
        }
        em_tls_free(t);
        em_close(fd);
    }
    em_close(fd0);
    em_close(lfd);
    return 0;
}

static int dial(em_tls **t)
{
    int fd = connect_local(port);
    assert(fd >= 0);
    assert(em_tls_connect(t, cctx, fd, "localhost") == 0);
    return fd;
}

static int client(void *arg)
{
    (void)arg;
    while (!port) em_yield();
    em_tls *t = NULL;
    char buf[64];

    int fd = connect_local(port);
    assert(fd >= 0);
    em_scope s;
    int64_t t0 = em_now();
    em_scope_open(&s, t0 + 20000000);
    assert(em_tls_connect(&t, cctx, fd, "localhost") == -ETIMEDOUT);
    int64_t ms = (em_now() - t0) / 1000000;
    em_scope_close(&s);
    printf("handshake with silent peer: -ETIMEDOUT after %lld ms\n", (long long)ms);
    assert(t == NULL && ms >= 20 && ms < 120);
    em_close(fd);

    fd = dial(&t);
    em_scope_open(&s, em_now() + 20000000);
    assert(em_tls_recv(t, buf, sizeof buf) == -ETIMEDOUT);
    em_scope_close(&s);
    assert(em_tls_recv(t, buf, sizeof buf) == 4 && memcmp(buf, "late", 4) == 0); /* retry works */
    em_tls_free(t);
    em_close(fd);

    fd = dial(&t);
    static char big[64u << 20];
    em_scope_open(&s, em_now() + 50000000);
    assert(em_tls_send_all(t, big, sizeof big) == -ETIMEDOUT);
    em_scope_close(&s);
    assert(em_tls_send_all(t, "x", 1) == -EPIPE);
    assert(em_tls_recv(t, buf, sizeof buf) == -EPIPE);
    assert(em_tls_shutdown(t) == -EPIPE);
    printf("cancelled recv retried fine; cancelled send -> later calls -EPIPE\n");
    em_tls_free(t);
    em_close(fd);
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
    SSL_CTX_free(sctx);
    SSL_CTX_free(cctx);
    return 0;
}
