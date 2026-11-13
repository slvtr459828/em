/* Requests M1 refuses, and a handler that does not answer: a response, then close. */
#include "http_util.h"

static void rejected(uint16_t port, const char *req, const char *status)
{
    rd_t *r = rd_open(port);
    send_str(r->fd, req);
    expect_resp(r, status, "", "close");
    expect_eof(r);
    rd_close(r);
}

static int main_fn(void *arg)
{
    (void)arg;
    em_scope dl, sc;
    struct hsrv s;
    uint16_t port;
    em_scope_open(&dl, em_now() + 20000000000ll);
    http_start(&s, &sc, &port);

    /* a body over max_body_bytes is refused before the handler (the bytes ride along in the same send) */
    http_cfg.max_body_bytes = 4;
    rejected(port, "POST /x HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello", "HTTP/1.1 413 ");
    rejected(port, "POST /x HTTP/1.0\r\nContent-Length: 5\r\n\r\nhello", "HTTP/1.0 413 ");
    rejected(port, "POST /x HTTP/1.1\r\nContent-Length: 99999999999999999999\r\n\r\n", "HTTP/1.1 413 ");
    http_cfg.max_body_bytes = 0;
    rejected(port, "POST /x HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\n0\r\n\r\n", "HTTP/1.1 501 ");
    rejected(port, "POST /x HTTP/1.1\r\nTransfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n", "HTTP/1.1 501 ");
    rejected(port, "POST /x HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", "HTTP/1.0 400 ");
    /* smuggling shapes */
    rejected(port, "POST /x HTTP/1.1\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", "HTTP/1.1 400 ");
    rejected(port, "POST /x HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello!", "HTTP/1.1 400 ");
    rejected(port, "POST /x HTTP/1.1\r\nContent-Length: abc\r\n\r\n", "HTTP/1.1 400 ");
    rejected(port, "POST /x HTTP/1.1\r\nContent-Length: +5\r\n\r\n", "HTTP/1.1 400 ");
    rejected(port, "GET / HTTP/1.1\r\nX: a\r\n b\r\n\r\n", "HTTP/1.1 400 "); /* obs-fold */

    /* header block over the read buffer: exactly 8192 bytes, no end yet -> 431 */
    {
        char big[8192];
        memset(big, 'a', sizeof big);
        const char *pre = "GET / HTTP/1.1\r\nX: ";
        memcpy(big, pre, strlen(pre));
        rd_t *r = rd_open(port);
        assert(em_send_all(r->fd, big, sizeof big) == 0);
        expect_resp(r, "HTTP/1.1 431 ", "", "close");
        expect_eof(r);
        rd_close(r);
    }
    /* a complete head of exactly 8192 bytes still passes */
    {
        char big[8192 + 1];
        const char *pre = "GET / HTTP/1.1\r\nX: ", *post = "\r\n\r\n";
        size_t pl = strlen(pre), ql = strlen(post);
        memset(big, 'a', 8192);
        memcpy(big, pre, pl);
        memcpy(big + 8192 - ql, post, ql);
        rd_t *r = rd_open(port);
        assert(em_send_all(r->fd, big, 8192) == 0);
        expect_resp(r, "HTTP/1.1 200 ", "GET /", NULL);
        rd_close(r);
    }

    /* garbage */
    rejected(port, "\x01\x02 garbage\r\n\r\n", "HTTP/1.1 400 ");
    rejected(port, "GET / HTTP/2.0\r\n\r\n", "HTTP/1.1 400 ");
    rejected(port, "GET /\x01 HTTP/1.1\r\n\r\n", "HTTP/1.1 400 ");

    /* handler that does not respond, or fails: 500 + close; earlier pipelined answers survive */
    rejected(port, "GET /noresp HTTP/1.1\r\n\r\n", "HTTP/1.1 500 ");
    rejected(port, "GET /err HTTP/1.1\r\n\r\n", "HTTP/1.1 500 ");
    {
        rd_t *r = rd_open(port);
        send_str(r->fd, "GET /ok HTTP/1.1\r\n\r\nGET /noresp HTTP/1.1\r\n\r\nGET /late HTTP/1.1\r\n\r\n");
        expect_resp(r, "HTTP/1.1 200 ", "GET /ok", NULL);
        expect_resp(r, "HTTP/1.1 500 ", "", "close");
        expect_eof(r);
        rd_close(r);
    }

    /* the handler answered, then failed: its answer goes out (keep-alive header and all), no 500 after it */
    {
        rd_t *r = rd_open(port);
        send_str(r->fd, "GET /okerr HTTP/1.1\r\n\r\nGET /late HTTP/1.1\r\n\r\n");
        expect_resp(r, "HTTP/1.1 200 ", "okerr", NULL);
        expect_eof(r);
        rd_close(r);
    }

    /* the peer leaves in the middle of a request: serve ends, nothing else breaks */
    {
        rd_t *r = rd_open(port);
        send_str(r->fd, "GET /half HTTP/1.1\r\nHo");
        rd_close(r);
        r = rd_open(port);
        send_str(r->fd, "GET /after HTTP/1.1\r\n\r\n");
        expect_resp(r, "HTTP/1.1 200 ", "GET /after", NULL);
        rd_close(r);
    }

    /* tls is not supported in M1 */
    assert(em_http_serve(-1, (em_tls *)&s, NULL, test_handler, NULL) == -ENOTSUP);

    int r1 = http_stop(&s);
    int r2 = em_scope_close(&dl);
    return r1 ? r1 : r2;
}

int main(void)
{
    assert(em_run(1, main_fn, NULL) == 0);
    puts("http reject: 413/501/400/431/500 and close verified");
    return 0;
}
