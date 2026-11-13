/* Request bodies: Content-Length and chunked through em_http_read_body (1..7 byte reads), what is
 * pipelined behind them, drain or close when the handler leaves the body unread, limits, bad
 * framing, a peer that leaves mid-body, HEAD. */
#include "http_util.h"

static void pause_ms(int ms) { assert(em_sleep_until(em_now() + ms * 1000000ll) == 0); }

/* the handler echoes: "POST /echo" with this request must answer 200 + `body` */
static void echo(rd_t *r, const char *req, const char *body)
{
    send_str(r->fd, req);
    expect_resp(r, "HTTP/1.1 200 ", body, NULL);
}

static void closes(uint16_t port, const char *req, const char *status)
{
    rd_t *r = rd_open(port);
    send_str(r->fd, req);
    expect_resp(r, status, "", "close");
    expect_eof(r);
    rd_close(r);
}

/* everything the server sends until it closes */
static size_t slurp(rd_t *r, char *out, size_t cap)
{
    size_t n = 0;
    for (;;) {
        ssize_t k = em_recv(r->fd, out + n, cap - 1 - n);
        assert(k >= 0);
        if (!k) break;
        n += (size_t)k;
    }
    out[n] = 0;
    return n;
}

static int main_fn(void *arg)
{
    (void)arg;
    em_scope dl, sc;
    struct hsrv s;
    uint16_t port;
    em_scope_open(&dl, em_now() + 30000000000ll);
    http_start(&s, &sc, &port);
    rd_t *r;

    /* Content-Length: all in the first send; split; none; GET */
    r = rd_open(port);
    echo(r, "POST /echo HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello", "hello");
    send_str(r->fd, "POST /echo HTTP/1.1\r\nContent-Length: 10\r\n\r\nabc");
    pause_ms(20);
    send_str(r->fd, "defghij");
    expect_resp(r, "HTTP/1.1 200 ", "abcdefghij", NULL);
    echo(r, "POST /echo HTTP/1.1\r\nContent-Length: 0\r\n\r\n", "");
    echo(r, "GET /echo HTTP/1.1\r\n\r\n", "");
    /* pipelined right behind a Content-Length body, then behind a chunked one */
    send_str(r->fd, "POST /echo HTTP/1.1\r\nContent-Length: 5\r\n\r\nhelloGET /next HTTP/1.1\r\n\r\n");
    expect_resp(r, "HTTP/1.1 200 ", "hello", NULL);
    expect_resp(r, "HTTP/1.1 200 ", "GET /next", NULL);
    send_str(r->fd, "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\nGET /n2 HTTP/1.1\r\n\r\n");
    expect_resp(r, "HTTP/1.1 200 ", "abc", NULL);
    expect_resp(r, "HTTP/1.1 200 ", "GET /n2", NULL);
    rd_close(r);

    /* chunked: extensions, trailer, upper-case hex, uneven pieces, one byte at a time */
    r = rd_open(port);
    echo(r, "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
            "4;ext=1;b=\"x\"\r\nWiki\r\n5\r\npedia\r\nB\r\n in chunks.\r\n0\r\nX-Trailer: v\r\nY: w\r\n\r\n",
         "Wikipedia in chunks.");
    echo(r, "POST /echo HTTP/1.1\r\ntransfer-encoding: Chunked\r\n\r\n0\r\n\r\n", "");
    {
        const char *req = "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n3;e\r\nabc\r\n2\r\nde\r\n0\r\nT: 1\r\n\r\n";
        for (const char *p = req; *p; p++) {
            assert(em_send_all(r->fd, p, 1) == 0);
            em_yield();
        }
        expect_resp(r, "HTTP/1.1 200 ", "abcde", NULL);
    }
    {   /* 30000 bytes in 1000-byte chunks: the window refills, the answer outgrows the write buffer */
        static char req[40000], want[30001];
        size_t n = (size_t)snprintf(req, sizeof req, "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n");
        for (int i = 0; i < 30; i++) {
            n += (size_t)snprintf(req + n, sizeof req - n, "3E8\r\n");
            memset(req + n, 'a' + i % 26, 1000);
            memset(want + i * 1000, 'a' + i % 26, 1000);
            n += 1000;
            memcpy(req + n, "\r\n", 2); n += 2;
        }
        memcpy(req + n, "0\r\n\r\n", 6);
        echo(r, req, want);
    }
    {   /* Content-Length 60000 (client sends it in pieces) */
        static char req[60200], want[60001];
        memset(want, 'z', 60000);
        size_t n = (size_t)snprintf(req, sizeof req, "POST /echo HTTP/1.1\r\nContent-Length: 60000\r\n\r\n");
        memcpy(req + n, want, 60001); /* with the NUL */
        for (size_t off = 0; off < n + 60000; off += 7000) {
            size_t k = n + 60000 - off < 7000 ? n + 60000 - off : 7000;
            assert(em_send_all(r->fd, req + off, k) == 0);
        }
        expect_resp(r, "HTTP/1.1 200 ", want, NULL);
    }
    rd_close(r);

    /* unread body, Content-Length <= 64 KiB: drained, connection kept, next request served */
    r = rd_open(port);
    echo(r, "POST /noread HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello", "noread");
    send_str(r->fd, "POST /noread HTTP/1.1\r\nContent-Length: 4\r\n\r\nabcdGET /after HTTP/1.1\r\n\r\n");
    expect_resp(r, "HTTP/1.1 200 ", "noread", NULL);
    expect_resp(r, "HTTP/1.1 200 ", "GET /after", NULL);
    {   /* exactly 64 KiB, arriving after the response */
        static char b[65536];
        memset(b, 'q', sizeof b);
        send_str(r->fd, "POST /noread HTTP/1.1\r\nContent-Length: 65536\r\n\r\n");
        expect_resp(r, "HTTP/1.1 200 ", "noread", NULL);
        assert(em_send_all(r->fd, b, sizeof b) == 0);
        echo(r, "GET /again HTTP/1.1\r\n\r\n", "GET /again");
    }
    rd_close(r);

    /* unread Content-Length remainder over 64 KiB, unfinished chunked: Connection: close */
    r = rd_open(port);
    send_str(r->fd, "POST /noread HTTP/1.1\r\nContent-Length: 65537\r\n\r\nabc");
    expect_resp(r, "HTTP/1.1 200 ", "noread", "close");
    expect_eof(r);
    rd_close(r);
    r = rd_open(port);
    send_str(r->fd, "POST /noread HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n");
    expect_resp(r, "HTTP/1.1 200 ", "noread", "close");
    expect_eof(r);
    rd_close(r);
    r = rd_open(port); /* even a complete chunked body that nobody read */
    send_str(r->fd, "POST /noread HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
    expect_resp(r, "HTTP/1.1 200 ", "noread", "close");
    expect_eof(r);
    rd_close(r);

    /* limits: Content-Length over max_body 413 before the handler; chunked over it: read_body fails, 413 */
    http_cfg.max_body_bytes = 100;
    closes(port, "POST /echo HTTP/1.1\r\nContent-Length: 101\r\n\r\n", "HTTP/1.1 413 ");
    {
        static char req[400], x[101];
        memset(x, 'x', 100);
        r = rd_open(port);
        snprintf(req, sizeof req, "POST /echo HTTP/1.1\r\nContent-Length: 100\r\n\r\n%s", x);
        echo(r, req, x);                      /* exactly the limit passes */
        rd_close(r);
        snprintf(req, sizeof req, "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n65\r\n%sx\r\n0\r\n\r\n", x);
        closes(port, req, "HTTP/1.1 413 ");   /* 101 decoded bytes */
        snprintf(req, sizeof req, "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n32\r\n%.50s\r\n32\r\n%.50s\r\n1\r\nx\r\n0\r\n\r\n", x, x);
        closes(port, req, "HTTP/1.1 413 ");   /* over the limit across chunks */
    }
    http_cfg.max_body_bytes = 0;

    /* bad chunk framing -> 400 + close */
    closes(port, "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n", "HTTP/1.1 400 ");
    closes(port, "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nabXX0\r\n\r\n", "HTTP/1.1 400 ");

    /* the peer leaves in the middle of a body: the server is fine afterwards */
    r = rd_open(port);
    send_str(r->fd, "POST /echo HTTP/1.1\r\nContent-Length: 10\r\n\r\nabc");
    rd_close(r);
    r = rd_open(port);
    send_str(r->fd, "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nab");
    rd_close(r);
    r = rd_open(port);
    send_str(r->fd, "POST /noread HTTP/1.1\r\nContent-Length: 10\r\n\r\nabc");
    expect_resp(r, "HTTP/1.1 200 ", "noread", NULL);
    rd_close(r); /* leaves while the body is being drained */
    r = rd_open(port);
    echo(r, "GET /alive HTTP/1.1\r\n\r\n", "GET /alive");
    rd_close(r);

    /* HEAD: same headers (Content-Length = len), no body, inline and direct-send paths */
    {
        static char out[4096];
        r = rd_open(port);
        send_str(r->fd, "HEAD /big HTTP/1.1\r\n\r\nHEAD /x HTTP/1.1\r\n\r\nGET /after HTTP/1.1\r\nConnection: close\r\n\r\n");
        size_t n = slurp(r, out, sizeof out);
        assert(strstr(out, "Content-Length: 20000\r\n"));
        assert(strstr(out, "Content-Length: 7\r\n"));
        assert(!strstr(out, "bbbb") && !strstr(out, "HEAD /x\r") );
        assert(n < 800 && strstr(out, "GET /after"));
        assert(strstr(out, "\r\n\r\nGET /after"));
        rd_close(r);
    }

    int r1 = http_stop(&s);
    int r2 = em_scope_close(&dl);
    return r1 ? r1 : r2;
}

int main(void)
{
    assert(em_run(1, main_fn, NULL) == 0);
    puts("http body: Content-Length, chunked, pipelining, drain/close, limits, framing, HEAD verified");
    return 0;
}
