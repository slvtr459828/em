/* A TCP close without close_notify is -ECONNRESET (truncation), never 0. With close_notify: 0. */
#include <stdio.h>
#include "tls_util.h"

static SSL_CTX *sctx, *cctx;
static uint16_t port;

static int server(void *arg)
{
    (void)arg;
    int lfd = listen_local(&port);
    for (int i = 0; i < 2; i++) {
        int fd = em_accept(lfd);
        if (fd < 0) return fd;
        em_tls *t;
        int r = em_tls_accept(&t, sctx, fd);
        if (r < 0) return r;
        if ((r = em_tls_send_all(t, "hello", 5)) < 0) return r;
        if (i == 1 && (r = em_tls_shutdown(t)) < 0) return r; /* 2nd connection ends cleanly */
        em_tls_free(t);
        em_close(fd);                                         /* 1st: FIN, no close_notify */
    }
    em_close(lfd);
    return 0;
}

static int read_all(uint16_t p, ssize_t *end)
{
    int fd = connect_local(p);
    assert(fd >= 0);
    em_tls *t;
    assert(em_tls_connect(&t, cctx, fd, "localhost") == 0);
    char b[16];
    ssize_t n = em_tls_recv(t, b, sizeof b);
    assert(n == 5 && memcmp(b, "hello", 5) == 0);
    *end = em_tls_recv(t, b, sizeof b);
    ssize_t again = em_tls_recv(t, b, sizeof b); /* the answer is stable */
    assert(again == *end);
    em_tls_free(t);
    em_close(fd);
    return 0;
}

static int client(void *arg)
{
    (void)arg;
    while (!port) em_yield();
    ssize_t end;
    read_all(port, &end);
    printf("no close_notify -> %zd\n", end);
    assert(end == -ECONNRESET);
    read_all(port, &end);
    printf("close_notify    -> %zd\n", end);
    assert(end == 0);
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
    return 0;
}
