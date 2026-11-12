/* Pipelining (responses in order, one flush) and requests that arrive in pieces. */
#include "http_util.h"

#define DEPTH 16

static int main_fn(void *arg)
{
    (void)arg;
    em_scope dl, sc;
    struct hsrv s;
    uint16_t port;
    em_scope_open(&dl, em_now() + 20000000000ll);
    http_start(&s, &sc, &port);
    rd_t *r;

    /* 16 requests in one send -> 16 responses, in order, repeated on the same connection */
    r = rd_open(port);
    for (int round = 0; round < 3; round++) {
        char req[DEPTH * 48], body[DEPTH][32];
        size_t n = 0;
        for (int i = 0; i < DEPTH; i++) {
            n += (size_t)snprintf(req + n, sizeof req - n, "GET /r%d/%d HTTP/1.1\r\nHost: x\r\n\r\n", round, i);
            snprintf(body[i], sizeof body[i], "GET /r%d/%d", round, i);
        }
        assert(em_send_all(r->fd, req, n) == 0);
        for (int i = 0; i < DEPTH; i++) expect_resp(r, "HTTP/1.1 200 ", body[i], NULL);
    }
    rd_close(r);

    /* big bodies mixed in: write buffer fills, flushes, and a 20 KB body goes out from its own
     * pointer; order must hold */
    r = rd_open(port);
    send_str(r->fd, "GET /p1 HTTP/1.1\r\n\r\nGET /big HTTP/1.1\r\n\r\nGET /p2 HTTP/1.1\r\n\r\n"
                    "GET /4k HTTP/1.1\r\n\r\nGET /4k HTTP/1.1\r\n\r\nGET /4k HTTP/1.1\r\n\r\n"
                    "GET /p3 HTTP/1.1\r\n\r\n");
    expect_resp(r, "HTTP/1.1 200 ", "GET /p1", NULL);
    expect_resp(r, "HTTP/1.1 200 ", big_body, NULL);
    expect_resp(r, "HTTP/1.1 200 ", "GET /p2", NULL);
    for (int i = 0; i < 3; i++) expect_resp(r, "HTTP/1.1 200 ", k4_body, NULL);
    expect_resp(r, "HTTP/1.1 200 ", "GET /p3", NULL);
    rd_close(r);

    /* a close in the middle ends the pipeline: later requests are not answered */
    r = rd_open(port);
    send_str(r->fd, "GET /a HTTP/1.1\r\n\r\nGET /b HTTP/1.1\r\nConnection: close\r\n\r\nGET /c HTTP/1.1\r\n\r\n");
    expect_resp(r, "HTTP/1.1 200 ", "GET /a", NULL);
    expect_resp(r, "HTTP/1.1 200 ", "GET /b", "close");
    expect_eof(r);
    rd_close(r);

    /* short reads: byte by byte, two requests back to back */
    r = rd_open(port);
    const char *two = "GET /s1 HTTP/1.1\r\nHost: x\r\n\r\nGET /s2 HTTP/1.1\r\nHost: x\r\n\r\n";
    size_t tl = strlen(two);
    for (size_t i = 0; i < tl; i++) {
        assert(em_send_all(r->fd, two + i, 1) == 0);
        em_yield(); /* let the server fiber see this byte alone */
    }
    expect_resp(r, "HTTP/1.1 200 ", "GET /s1", NULL);
    expect_resp(r, "HTTP/1.1 200 ", "GET /s2", NULL);

    /* a whole request plus half of the next (the leftover is moved to the buffer start) */
    send_str(r->fd, "GET /h1 HTTP/1.1\r\n\r\nGET /h2 HTT");
    expect_resp(r, "HTTP/1.1 200 ", "GET /h1", NULL); /* flushed before the server waits */
    send_str(r->fd, "P/1.1\r\n\r\n");
    expect_resp(r, "HTTP/1.1 200 ", "GET /h2", NULL);
    rd_close(r);

    int r1 = http_stop(&s);
    int r2 = em_scope_close(&dl);
    return r1 ? r1 : r2;
}

int main(void)
{
    assert(em_run(1, main_fn, NULL) == 0);
    puts("http pipeline: order, big bodies, close mid-pipeline, short reads verified");
    return 0;
}
