/* Lingering close: a connection ended while the peer may still be sending is shut (FIN after the
 * response) and drained, bounded in time and bytes; normal, timed-out and EOF closes are not. */
#include "http_util.h"

#define MS 1000000ll
#define HDR413 "POST /x HTTP/1.1\r\nContent-Length: 2097152\r\n\r\n" /* over max_body_bytes (1 MiB) */

static int64_t since(int64_t t0) { return (em_now() - t0) / MS; }

static void wait_done(int n)
{
    while (atomic_load(&http_done) < n) assert(em_sleep_until(em_now() + MS) == 0);
}

static _Atomic int stop;

/* keeps sending on r: chunk bytes, then gap_ms of rest; total 0 = until stop or the first error */
struct snd { rd_t *r; size_t total, chunk; int64_t gap_ms; };

static int sender(void *arg)
{
    struct snd *s = arg;
    static char buf[1 << 14];
    memset(buf, 'x', sizeof buf);
    for (size_t sent = 0; !atomic_load(&stop) && (!s->total || sent < s->total); ) {
        size_t n = s->chunk;
        if (s->total && n > s->total - sent) n = s->total - sent;
        if (rd_send(s->r, buf, n) != 0) break;
        sent += n;
        if (s->gap_ms && em_sleep_until(em_now() + s->gap_ms * MS) != 0) break;
    }
    return 0;
}

/* the response is followed by the server's FIN, but it is not done: it is still discarding */
static void still_lingering(rd_t *r, int n)
{
    expect_eof(r);
    assert(em_sleep_until(em_now() + 50 * MS) == 0);
    assert(atomic_load(&http_done) == n);
}

static int main_fn(void *arg)
{
    (void)arg;
    em_scope dl, sc, ss;
    struct hsrv s;
    uint16_t port;
    em_scope_open(&dl, em_now() + 30000 * MS);
    http_start(&s, &sc, &port);
    rd_t *r;
    int n;
    int64_t t0;

    /* 1. the client is not reading (small window) when the server decides: a 20 KB answer is still
     * queued behind it, and then the 413. Closing with the unread body would reset the connection and
     * destroy both; with the linger the client gets everything, then the FIN. */
    r = rd_open_rcvbuf(port, 4096);
    n = atomic_load(&http_done);
    {
        static char req[sizeof "GET /big HTTP/1.1\r\n\r\n" HDR413 + (100 << 10)];
        size_t k = (size_t)snprintf(req, sizeof req, "GET /big HTTP/1.1\r\n\r\n" HDR413);
        memset(req + k, 'x', 100 << 10);
        assert(rd_send(r, req, k + (100 << 10)) == 0);
    }
    assert(em_sleep_until(em_now() + 100 * MS) == 0); /* the server has decided and closed (or lingers) */
    expect_resp(r, "HTTP/1.1 200 ", big_body, NULL);
    expect_resp(r, "HTTP/1.1 413 ", "", "close");
    still_lingering(r, n);
    rd_close(r);
    wait_done(n + 1);

    /* 2. the client keeps sending the body from one fiber and reads from another: the whole 413 */
    r = rd_open(port);
    n = atomic_load(&http_done);
    send_str(r, HDR413);
    {
        struct snd sd = { r, 2u << 20, 1 << 14, 0 };
        em_scope_open(&ss, 0);
        assert(em_spawn(&ss, sender, &sd) == 0);
        expect_resp(r, "HTTP/1.1 413 ", "", "close");
        expect_eof(r);
        assert(em_scope_close(&ss) == 0); /* the server stops reading at 1 MiB: the sender fails or is done */
    }
    wait_done(n + 1);
    rd_close(r);

    /* 3. byte cap: a peer that never stops sending is cut off by the 1 MiB cap, long before 2 s */
    r = rd_open(port);
    n = atomic_load(&http_done);
    send_str(r, HDR413);
    {
        struct snd sd = { r, 0, 1 << 14, 0 };
        atomic_store(&stop, 0);
        em_scope_open(&ss, 0);
        assert(em_spawn(&ss, sender, &sd) == 0);
        t0 = em_now();
        expect_resp(r, "HTTP/1.1 413 ", "", "close");
        wait_done(n + 1);
        assert(since(t0) < 1500);
        atomic_store(&stop, 1);
        assert(em_scope_close(&ss) == 0);
    }
    rd_close(r);

    /* 4. time cap: a trickle (1 byte per 100 ms) never reaches the byte cap; 2 s ends it */
    r = rd_open(port);
    n = atomic_load(&http_done);
    send_str(r, HDR413);
    {
        struct snd sd = { r, 0, 1, 100 };
        atomic_store(&stop, 0);
        em_scope_open(&ss, 0);
        assert(em_spawn(&ss, sender, &sd) == 0);
        expect_resp(r, "HTTP/1.1 413 ", "", "close");
        t0 = em_now();
        wait_done(n + 1);
        assert(since(t0) >= 1800 && since(t0) < 4000);
        atomic_store(&stop, 1);
        assert(em_scope_close(&ss) == 0);
    }
    rd_close(r);

    /* 5. the other cases of the list: 500 over an unread body, Connection: close over a body
     * left unread above the drain limit, bad chunked framing */
    {
        static char big[100000];
        memset(big, 'y', sizeof big);
        r = rd_open(port);
        n = atomic_load(&http_done);
        send_str(r, "POST /err HTTP/1.1\r\nContent-Length: 100000\r\n\r\n");
        assert(rd_send(r, big, 50000) == 0);
        expect_resp(r, "HTTP/1.1 500 ", "", "close");
        still_lingering(r, n);
        rd_close(r);
        wait_done(n + 1);

        r = rd_open(port);
        n = atomic_load(&http_done);
        send_str(r, "POST /noread HTTP/1.1\r\nContent-Length: 100000\r\n\r\n");
        assert(rd_send(r, big, 50000) == 0);
        expect_resp(r, "HTTP/1.1 200 ", "noread", "close");
        still_lingering(r, n);
        rd_close(r);
        wait_done(n + 1);

        r = rd_open(port);
        n = atomic_load(&http_done);
        send_str(r, "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n");
        assert(rd_send(r, big, 20000) == 0);
        expect_resp(r, "HTTP/1.1 400 ", "", "close");
        still_lingering(r, n);
        rd_close(r);
        wait_done(n + 1);
    }

    /* 6. no linger: a fully read request with Connection: close, and a header timeout. The client
     * sees the close and serve has already returned (a lingering server would be waiting for us). */
    r = rd_open(port);
    n = atomic_load(&http_done);
    send_str(r, "POST /echo HTTP/1.1\r\nConnection: close\r\nContent-Length: 5\r\n\r\nhello");
    expect_resp(r, "HTTP/1.1 200 ", "hello", "close");
    expect_eof(r);
    assert(atomic_load(&http_done) == n + 1);
    rd_close(r);
    http_cfg.header_timeout_ns = 100 * MS;
    r = rd_open(port);
    n = atomic_load(&http_done);
    send_str(r, "GET /slow HTTP/1.1\r\nHo");
    expect_resp(r, "HTTP/1.1 408 ", "", "close");
    expect_eof(r);
    assert(atomic_load(&http_done) == n + 1);
    rd_close(r);
    http_cfg.header_timeout_ns = 0;

    /* 7. max_requests_per_conn ends a connection the client is still pipelining on: the requests
     * behind the last one are never read, so this close lingers too (the 20 KB answer is still
     * queued behind the small window when the server decides). */
    http_cfg.max_requests_per_conn = 1;
    r = rd_open_rcvbuf(port, 4096);
    n = atomic_load(&http_done);
    {
        static char req[sizeof "GET /big HTTP/1.1\r\n\r\n" + 4000 * (sizeof "GET / HTTP/1.1\r\n\r\n" - 1)];
        size_t k = (size_t)snprintf(req, sizeof req, "GET /big HTTP/1.1\r\n\r\n");
        for (int i = 0; i < 4000; i++) k += (size_t)snprintf(req + k, sizeof req - k, "GET / HTTP/1.1\r\n\r\n");
        assert(rd_send(r, req, k) == 0);
    }
    assert(em_sleep_until(em_now() + 100 * MS) == 0);
    expect_resp(r, "HTTP/1.1 200 ", big_body, "close");
    still_lingering(r, n);
    rd_close(r);
    wait_done(n + 1);
    http_cfg.max_requests_per_conn = 0;

    int r1 = http_stop(&s);
    int r2 = em_scope_close(&dl);
    return r1 ? r1 : r2;
}

int main(void)
{
    assert(em_run(1, main_fn, NULL) == 0);
    puts("http linger: 413/500/400/close drained and shut, byte and time caps, no linger on normal and timeout closes verified");
    return 0;
}
