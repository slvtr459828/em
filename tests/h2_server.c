/* HTTP/2 server for tests/h2spec.sh and tests/http_curl.sh: h2c (prior knowledge) and h1, or with
 * argv[3] = "tls" h2 and h1 through ALPN. argv[1] = port, argv[2] = workers. Same handler as
 * bench/em_http.c:
 * GET /   -> 200 text/plain "Hello, World!"
 * GET /4k -> 200 text/plain 4096 x 'a'
 * else    -> 404 */
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tls_util.h"
#include "../include/em_http.h"

static struct sockaddr_in addr;
static char body4k[4096];
static SSL_CTX *sctx; /* NULL = plain */

static int handle(em_http_conn *c, const em_http_req *r, void *arg)
{
    (void)arg;
    static const em_http_header text = { "Content-Type", 12, "text/plain", 10 };
    if (r->path_len == 1 && r->path[0] == '/') return em_http_respond(c, 200, &text, 1, "Hello, World!", 13);
    if (r->path_len == 3 && memcmp(r->path, "/4k", 3) == 0) return em_http_respond(c, 200, &text, 1, body4k, sizeof body4k);
    return em_http_respond(c, 404, &text, 1, "Not Found\n", 10);
}

static int conn(void *arg)
{
    int fd = (int)(intptr_t)arg;
    em_tls *t = NULL;
    if (!sctx || em_tls_accept(&t, sctx, fd) == 0) {
        em_http_serve(fd, t, NULL, handle, NULL);
        if (t) { em_tls_shutdown(t); em_tls_free(t); }
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
        if (em_spawn(&sc, conn, (void *)(intptr_t)fd) != 0) { em_close(fd); r = -ENOMEM; }
    }
    em_scope_cancel(&sc);
    em_close(lfd);
    em_scope_close(&sc);
    return r;
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 4 || (argc == 4 && strcmp(argv[3], "tls"))) {
        fprintf(stderr, "usage: %s port [workers] [tls]\n", argv[0]);
        return 2;
    }
    int workers = argc >= 3 ? atoi(argv[2]) : 1;
    addr = (struct sockaddr_in){ .sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(argv[1])),
                                 .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    memset(body4k, 'a', sizeof body4k);
    if (argc == 4) {
        SSL_CTX *cctx;
        tls_test_ctx(&sctx, &cctx);
        SSL_CTX_free(cctx);
        SSL_CTX_set_alpn_select_cb(sctx, tls_test_alpn, NULL);
    }
    int r = em_run(workers, worker, NULL);
    if (r) fprintf(stderr, "em_run: %d\n", r);
    return r != 0;
}
