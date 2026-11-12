/* HTTP/1.x connection semantics: keep-alive per version, Connection header, max_requests. */
#include "http_util.h"

static void get(rd_t *r, const char *req, const char *status, const char *body, const char *conn)
{
    send_str(r->fd, req);
    expect_resp(r, status, body, conn);
}

static int main_fn(void *arg)
{
    (void)arg;
    em_scope dl, sc;
    struct hsrv s;
    uint16_t port;
    em_scope_open(&dl, em_now() + 20000000000ll); /* a hang fails the test instead of stalling it */
    http_start(&s, &sc, &port);

    /* 1.1: kept by default, many requests on one connection; no Connection header needed */
    rd_t *r = rd_open(port);
    for (int i = 0; i < 5; i++) {
        char req[64], body[32];
        snprintf(req, sizeof req, "GET /k%d HTTP/1.1\r\nHost: x\r\n\r\n", i);
        snprintf(body, sizeof body, "GET /k%d", i);
        get(r, req, "HTTP/1.1 200 ", body, NULL);
    }
    rd_close(r);

    /* 1.0: closed by default, answered as 1.0 */
    r = rd_open(port);
    get(r, "GET /a HTTP/1.0\r\n\r\n", "HTTP/1.0 200 ", "GET /a", "close");
    expect_eof(r);
    rd_close(r);

    /* 1.0 + keep-alive: kept, the response says so; the next request without it closes */
    r = rd_open(port);
    get(r, "GET /a HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", "HTTP/1.0 200 ", "GET /a", "keep-alive");
    get(r, "GET /b HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n", "HTTP/1.0 200 ", "GET /b", "keep-alive");
    get(r, "GET /c HTTP/1.0\r\n\r\n", "HTTP/1.0 200 ", "GET /c", "close");
    expect_eof(r);
    rd_close(r);

    /* 1.1 + close (any case, in a token list): answered and closed */
    r = rd_open(port);
    get(r, "GET /a HTTP/1.1\r\nHost: x\r\nconnection: CLOSE\r\n\r\n", "HTTP/1.1 200 ", "GET /a", "close");
    expect_eof(r);
    rd_close(r);
    r = rd_open(port);
    get(r, "GET /a HTTP/1.1\r\nConnection: te, close\r\n\r\n", "HTTP/1.1 200 ", "GET /a", "close");
    expect_eof(r);
    rd_close(r);

    /* Content-Length: 0 on a GET is a request without a body */
    r = rd_open(port);
    get(r, "GET /a HTTP/1.1\r\nContent-Length: 0\r\n\r\n", "HTTP/1.1 200 ", "GET /a", NULL);
    rd_close(r);

    int r1 = http_stop(&s);
    assert(r1 == 0);

    /* max_requests_per_conn = 3: the third response carries close */
    http_cfg.max_requests_per_conn = 3;
    http_start(&s, &sc, &port);
    r = rd_open(port);
    get(r, "GET /1 HTTP/1.1\r\n\r\n", "HTTP/1.1 200 ", "GET /1", NULL);
    get(r, "GET /2 HTTP/1.1\r\n\r\n", "HTTP/1.1 200 ", "GET /2", NULL);
    get(r, "GET /3 HTTP/1.1\r\n\r\n", "HTTP/1.1 200 ", "GET /3", "close");
    expect_eof(r);
    rd_close(r);
    r1 = http_stop(&s);
    http_cfg.max_requests_per_conn = 0;

    int r2 = em_scope_close(&dl);
    return r1 ? r1 : r2;
}

int main(void)
{
    assert(em_run(1, main_fn, NULL) == 0);
    puts("http keepalive: version semantics verified");
    return 0;
}
