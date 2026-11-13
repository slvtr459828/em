/* Expect: 100-continue is answered when, and only when, the handler reads the body. */
#include "http_util.h"

static int main_fn(void *arg)
{
    (void)arg;
    em_scope dl, sc;
    struct hsrv s;
    uint16_t port;
    em_scope_open(&dl, em_now() + 20000000000ll);
    http_start(&s, &sc, &port);
    rd_t *r;

    /* the handler reads: 100 first (only the headers are sent), once, then the answer; kept */
    r = rd_open(port);
    send_str(r->fd, "POST /echo HTTP/1.1\r\nExpect: 100-Continue\r\nContent-Length: 12\r\n\r\n");
    expect_100(r);
    send_str(r->fd, "hello, world");
    expect_resp(r, "HTTP/1.1 200 ", "hello, world", NULL); /* no second 100 in front of it */
    send_str(r->fd, "POST /echo HTTP/1.1\r\nExpect: 100-continue\r\nTransfer-Encoding: chunked\r\n\r\n");
    expect_100(r);
    send_str(r->fd, "3\r\nabc\r\n0\r\n\r\n");
    expect_resp(r, "HTTP/1.1 200 ", "abc", NULL);
    /* a pipelined answer still queued goes out before the 100 */
    send_str(r->fd, "GET /a HTTP/1.1\r\n\r\nPOST /echo HTTP/1.1\r\nExpect: 100-continue\r\nContent-Length: 3\r\n\r\n");
    expect_resp(r, "HTTP/1.1 200 ", "GET /a", NULL);
    expect_100(r);
    send_str(r->fd, "xyz");
    expect_resp(r, "HTTP/1.1 200 ", "xyz", NULL);
    rd_close(r);

    /* the handler does not read: no 100 at all, Connection: close */
    r = rd_open(port);
    send_str(r->fd, "POST /noread HTTP/1.1\r\nExpect: 100-continue\r\nContent-Length: 5\r\n\r\n");
    expect_resp(r, "HTTP/1.1 200 ", "noread", "close");
    expect_eof(r);
    rd_close(r);

    /* another Expect value: 417 and close, handler not run */
    r = rd_open(port);
    send_str(r->fd, "POST /echo HTTP/1.1\r\nExpect: foo\r\nContent-Length: 5\r\n\r\nhello");
    expect_resp(r, "HTTP/1.1 417 ", "", "close");
    expect_eof(r);
    rd_close(r);
    r = rd_open(port);
    send_str(r->fd, "GET / HTTP/1.0\r\nExpect: 100-continue, x\r\n\r\n");
    expect_resp(r, "HTTP/1.0 417 ", "", "close");
    expect_eof(r);
    rd_close(r);

    /* HTTP/1.0: no 100, the body is read as usual */
    r = rd_open(port);
    send_str(r->fd, "POST /echo HTTP/1.0\r\nExpect: 100-continue\r\nContent-Length: 3\r\n\r\nabc");
    expect_resp(r, "HTTP/1.0 200 ", "abc", "close");
    expect_eof(r);
    rd_close(r);

    int r1 = http_stop(&s);
    int r2 = em_scope_close(&dl);
    return r1 ? r1 : r2;
}

int main(void)
{
    assert(em_run(1, main_fn, NULL) == 0);
    puts("http expect: 100 only on read, close when unread, 417, HTTP/1.0 verified");
    return 0;
}
