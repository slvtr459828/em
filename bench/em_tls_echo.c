/* TLS echo server for bench/run.sh: argv[1] = port, argv[2] = workers (default 1). Same shape as
 * em_echo.c with em_tls on every connection. The certificate is generated at startup
 * (tests/tls_util.h); the bench client accepts any certificate. */
#include <netinet/tcp.h>
#include <stdio.h>
#include "../tests/tls_util.h"

static struct sockaddr_in addr;
static SSL_CTX *sctx;

static int echo(void *arg)
{
    int fd = (int)(intptr_t)arg;
    /* After the handshake the server writes the session tickets and then the echo: without NODELAY
     * Nagle holds the echo behind the unacked tickets until the client's delayed ACK (~40 ms) */
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &(int){ 1 }, sizeof(int));
    em_tls *t;
    if (em_tls_accept(&t, sctx, fd) == 0) {
        char buf[16384];
        for (;;) {
            ssize_t n = em_tls_recv(t, buf, sizeof buf);
            if (n <= 0 || em_tls_send_all(t, buf, (size_t)n) < 0) break;
        }
        em_tls_free(t);
    }
    em_close(fd);
    return 0; /* a dropped connection must not cancel the others */
}

static int worker(void *arg)
{
    (void)arg;
    int lfd = em_tcp_listen((struct sockaddr *)&addr, sizeof addr, 4096);
    if (lfd < 0) return lfd;
    em_scope sc;
    em_scope_open(&sc, 0);
    int r = 0;
    while (!r) {
        int fd = em_accept(lfd);
        if (fd < 0) { r = fd; break; }
        if (em_spawn(&sc, echo, (void *)(intptr_t)fd) != 0) { em_close(fd); r = -ENOMEM; }
    }
    em_scope_cancel(&sc);
    em_close(lfd);
    em_scope_close(&sc);
    return r;
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) { fprintf(stderr, "usage: %s port [workers]\n", argv[0]); return 2; }
    int workers = argc == 3 ? atoi(argv[2]) : 1;
    addr = (struct sockaddr_in){ .sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(argv[1])),
                                 .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    SSL_CTX *cctx;
    tls_test_ctx(&sctx, &cctx);
    SSL_CTX_free(cctx);
    int r = em_run(workers, worker, NULL);
    if (r) fprintf(stderr, "em_run: %d\n", r);
    return r != 0;
}
