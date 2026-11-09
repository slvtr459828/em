/* Name check: a client with SSL_VERIFY_PEER fails with -EPROTO for a host the certificate does not
 * name (a DNS name and an IP literal) and the OpenSSL error queue says why; the right name works. */
#include <stdio.h>
#include "tls_util.h"

static SSL_CTX *sctx, *cctx;
static uint16_t port;
static int srv_res[3];

static int server(void *arg)
{
    (void)arg;
    int lfd = listen_local(&port);
    for (int i = 0; i < 3; i++) {
        int fd = em_accept(lfd);
        if (fd < 0) return fd;
        em_tls *t;
        int r = em_tls_accept(&t, sctx, fd);
        srv_res[i] = r;
        if (r == 0) {
            char b;
            assert(em_tls_recv(t, &b, 1) == 0); /* client's close_notify */
            em_tls_free(t);
        }
        em_close(fd);
    }
    em_close(lfd);
    return 0;
}

static int try_host(const char *host)
{
    int fd = connect_local(port);
    assert(fd >= 0);
    em_tls *t = NULL;
    ERR_clear_error();
    int r = em_tls_connect(&t, cctx, fd, host);
    if (r == 0) {
        assert(em_tls_shutdown(t) == 0);
        em_tls_free(t);
    } else {
        assert(t == NULL);
        unsigned long e = ERR_peek_error();
        char msg[256];
        ERR_error_string_n(e, msg, sizeof msg);
        printf("  %s -> %d, OpenSSL: %s\n", host, r, msg);
        assert(e != 0 && ERR_GET_REASON(e) == SSL_R_CERTIFICATE_VERIFY_FAILED);
    }
    em_close(fd);
    return r;
}

static int client(void *arg)
{
    (void)arg;
    while (!port) em_yield();
    assert(try_host("example.org") == -EPROTO);
    assert(try_host("127.0.0.1") == -EPROTO);
    assert(try_host("localhost") == 0);
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
    for (int i = 0; i < 2; i++) assert(srv_res[i] == -EPROTO || srv_res[i] == -ECONNRESET); /* alert or close */
    assert(srv_res[2] == 0);
    printf("tls verify: wrong names -> -EPROTO, right name -> 0\n");
    return 0;
}
