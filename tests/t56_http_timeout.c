/* header_timeout_ns, idle_timeout_ns, and the caller's scope staying the caller's. */
#include "http_util.h"

#define MS 1000000ll

static int64_t since(int64_t t0) { return (em_now() - t0) / MS; }

/* serve returns `want` for a caller scope that ends the wait: cancel, or a deadline */
static void caller_scope(int cancel)
{
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    em_scope sc;
    em_scope_open(&sc, cancel ? 0 : em_now() + 100 * MS);
    int64_t t0 = em_now();
    if (cancel) em_scope_cancel(&sc);
    int rc = em_http_serve(sv[0], NULL, &http_cfg, test_handler, NULL); /* idle_timeout is 10 s */
    em_scope_close(&sc);
    assert(rc == (cancel ? -ECANCELED : -ETIMEDOUT));
    assert(since(t0) < 2000);
    em_close(sv[0]);
    em_close(sv[1]);
}

static int main_fn(void *arg)
{
    (void)arg;
    em_scope dl, sc;
    struct hsrv s;
    uint16_t port;
    em_scope_open(&dl, em_now() + 20000 * MS);
    http_start(&s, &sc, &port);
    rd_t *r;
    int64_t t0;

    http_cfg.header_timeout_ns = 100 * MS;
    http_cfg.idle_timeout_ns = 10000 * MS;

    /* slowloris: a partial head that stalls -> 408 + close, measured from the first byte */
    r = rd_open(port);
    t0 = em_now();
    send_str(r->fd, "GET /slow HTTP/1.1\r\nHo");
    expect_resp(r, "HTTP/1.1 408 ", "", "close");
    expect_eof(r);
    assert(since(t0) >= 90 && since(t0) < 2000);
    rd_close(r);

    /* a drip that keeps the connection "active" still hits the head deadline */
    r = rd_open(port);
    t0 = em_now();
    send_str(r->fd, "GET /drip HTTP/1.1\r\n");
    for (int i = 0; i < 2; i++) { assert(em_sleep_until(em_now() + 40 * MS) == 0); send_str(r->fd, "X: y\r\n"); }
    expect_resp(r, "HTTP/1.1 408 ", "", "close");
    expect_eof(r);
    rd_close(r);

    /* a head that arrives in time is fine, however long the idle gap before it (idle is 10 s here);
     * the clock starts at its first byte, not at the previous response */
    r = rd_open(port);
    send_str(r->fd, "GET /a HTTP/1.1\r\n\r\n");
    expect_resp(r, "HTTP/1.1 200 ", "GET /a", NULL);
    assert(em_sleep_until(em_now() + 150 * MS) == 0);
    send_str(r->fd, "GET /b HTTP/1.1\r\n\r\n");
    expect_resp(r, "HTTP/1.1 200 ", "GET /b", NULL);
    rd_close(r);

    /* header_timeout -1: a partial head waits (idle_timeout does not apply to it either) */
    http_cfg.header_timeout_ns = -1;
    r = rd_open(port);
    send_str(r->fd, "GET /h HTTP/1.1\r\nHo");
    assert(em_sleep_until(em_now() + 200 * MS) == 0);
    send_str(r->fd, "st: x\r\n\r\n");
    expect_resp(r, "HTTP/1.1 200 ", "GET /h", NULL);
    rd_close(r);

    /* idle between requests, and before the first: silent close */
    http_cfg.header_timeout_ns = 10000 * MS;
    http_cfg.idle_timeout_ns = 100 * MS;
    r = rd_open(port);
    send_str(r->fd, "GET /a HTTP/1.1\r\n\r\n");
    expect_resp(r, "HTTP/1.1 200 ", "GET /a", NULL);
    t0 = em_now();
    expect_eof(r);
    assert(since(t0) >= 90 && since(t0) < 2000);
    rd_close(r);
    r = rd_open(port);
    t0 = em_now();
    expect_eof(r);
    assert(since(t0) >= 90 && since(t0) < 2000);
    rd_close(r);

    /* stall in the middle of a body (both framings): 408 + close; a slow but steady body passes */
    r = rd_open(port);
    t0 = em_now();
    send_str(r->fd, "POST /echo HTTP/1.1\r\nContent-Length: 10\r\n\r\nabc");
    expect_resp(r, "HTTP/1.1 408 ", "", "close");
    expect_eof(r);
    assert(since(t0) >= 90 && since(t0) < 2000);
    rd_close(r);
    r = rd_open(port);
    send_str(r->fd, "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n");
    expect_resp(r, "HTTP/1.1 408 ", "", "close");
    expect_eof(r);
    rd_close(r);
    r = rd_open(port); /* Expect: the wait for the body starts at the 100 */
    send_str(r->fd, "POST /echo HTTP/1.1\r\nExpect: 100-continue\r\nContent-Length: 4\r\n\r\n");
    expect_100(r);
    expect_resp(r, "HTTP/1.1 408 ", "", "close");
    rd_close(r);
    r = rd_open(port);
    send_str(r->fd, "POST /echo HTTP/1.1\r\nContent-Length: 6\r\n\r\n");
    for (int i = 0; i < 6; i++) { assert(em_sleep_until(em_now() + 40 * MS) == 0); send_str(r->fd, "x"); }
    expect_resp(r, "HTTP/1.1 200 ", "xxxxxx", NULL); /* 240 ms in all, no single gap near 100 ms */
    rd_close(r);
    /* a stall while the unread body is drained: closed without a second answer */
    r = rd_open(port);
    send_str(r->fd, "POST /noread HTTP/1.1\r\nContent-Length: 10\r\n\r\nabc");
    expect_resp(r, "HTTP/1.1 200 ", "noread", NULL);
    expect_eof(r);
    rd_close(r);

    /* the caller's scope is not ours: cancel -> -ECANCELED, its deadline -> -ETIMEDOUT */
    http_cfg.header_timeout_ns = 10000 * MS;
    http_cfg.idle_timeout_ns = 10000 * MS;
    caller_scope(1);
    caller_scope(0);

    int r1 = http_stop(&s);
    int r2 = em_scope_close(&dl);
    return r1 ? r1 : r2;
}

int main(void)
{
    assert(em_run(1, main_fn, NULL) == 0);
    puts("http timeout: 408 on slow head and body, silent idle close, caller scope kept verified");
    return 0;
}
