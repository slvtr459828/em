// h2o as the HTTP rival of bench/run.sh (plans/http.md): libh2o (event loop, picohttpparser, HTTP/1
// state machine, response writer) with the same two in-memory responses the other servers return.
// Why a libh2o program, not the h2o binary: h2o serves a file per request (open/fstat/read) and mruby
// adds an interpreter, so either would charge h2o for something em_http, nginx `return` and haproxy
// `return` do from memory. Here the body is a static buffer handed to h2o_send: no copy, no file.
// usage: h2o_hello port [workers]   (workers is accepted for the common launch line; h2o runs 1 thread)
// GET / -> "Hello, World!" (13 B), GET /4k -> 4096 B, both text/plain with Content-Length; else 404.
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include "h2o.h"

static h2o_globalconf_t config;
static h2o_context_t ctx;
static h2o_accept_ctx_t accept_ctx;
static char big[4096];

static int on_req(h2o_handler_t *self __attribute__((unused)), h2o_req_t *req)
{
    static h2o_generator_t generator = {NULL, NULL};
    h2o_iovec_t body;
    if (!h2o_memis(req->method.base, req->method.len, H2O_STRLIT("GET")))
        return -1;
    if (h2o_memis(req->path_normalized.base, req->path_normalized.len, H2O_STRLIT("/")))
        body = h2o_iovec_init(H2O_STRLIT("Hello, World!"));
    else if (h2o_memis(req->path_normalized.base, req->path_normalized.len, H2O_STRLIT("/4k")))
        body = h2o_iovec_init(big, sizeof(big));
    else
        return -1;
    req->res.status = 200;
    req->res.reason = "OK";
    req->res.content_length = body.len; // else HTTP/1 falls back to chunked
    h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_CONTENT_TYPE, NULL, H2O_STRLIT("text/plain"));
    h2o_resp_add_date_header(req); // h2o's file handler adds it; a bare handler has to, and the other servers send one
    h2o_start_response(req, &generator);
    h2o_send(req, &body, 1, H2O_SEND_STATE_FINAL);
    return 0;
}

static void on_accept(h2o_socket_t *listener, const char *err)
{
    if (err != NULL)
        return;
    for (int i = 0; i < 16; i++) { // like h2o's own accept loop: drain a burst per wakeup
        h2o_socket_t *sock = h2o_evloop_socket_accept(listener);
        if (sock == NULL)
            return;
        h2o_accept(&accept_ctx, sock);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s port [workers]\n", argv[0]);
        return 2;
    }
    memset(big, 'x', sizeof(big));
    signal(SIGPIPE, SIG_IGN);

    h2o_config_init(&config);
    config.http1.req_timeout = 3600 * 1000; // ms; the default 10 s would close idle keep-alive connections
    h2o_hostconf_t *hostconf = h2o_config_register_host(&config, h2o_iovec_init(H2O_STRLIT("default")), 65535);
    h2o_pathconf_t *pathconf = h2o_config_register_path(hostconf, "/", 0);
    h2o_create_handler(pathconf, sizeof(h2o_handler_t))->on_req = on_req;
    h2o_context_init(&ctx, h2o_evloop_create(), &config);
    accept_ctx.ctx = &ctx;
    accept_ctx.hosts = config.hosts;

    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(atoi(argv[1])), .sin_addr.s_addr = htonl(0x7f000001)};
    int one = 1, fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0 ||
        bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, SOMAXCONN) != 0) {
        perror("h2o_hello: listen");
        return 1;
    }
    h2o_socket_read_start(h2o_evloop_socket_create(ctx.loop, fd, H2O_SOCKET_FLAG_DONT_READ), on_accept);
    while (h2o_evloop_run(ctx.loop, INT32_MAX) == 0)
        ;
    return 1;
}
