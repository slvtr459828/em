/* send_timeout_ns: a client that never reads is dropped (serve returns -ETIMEDOUT), a slow one that
 * keeps taking data is not, the caller's scope stays the caller's. */
#include "http_util.h"

#define MS 1000000ll
#define BODY (16u << 20) /* more than the socket buffers of a loopback pair hold */

static int64_t since(int64_t t0) { return (em_now() - t0) / MS; }

static void wait_done(int n)
{
    while (atomic_load(&http_done) < n) assert(em_sleep_until(em_now() + MS) == 0);
}

/* serve over a socketpair nobody reads: the caller's scope ends the blocked send */
static void caller_scope(int cancel)
{
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    static const char req[] = "GET /huge HTTP/1.1\r\n\r\n";
    assert(em_send_all(sv[1], req, sizeof req - 1) == 0);
    em_scope sc;
    em_scope_open(&sc, cancel ? 0 : em_now() + 100 * MS);
    int64_t t0 = em_now();
    if (cancel) em_scope_cancel(&sc);
    int rc = em_http_serve(sv[0], NULL, &http_cfg, test_handler, NULL); /* send_timeout is 60 s here */
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
    em_scope_open(&dl, em_now() + 30000 * MS);
    http_start(&s, &sc, &port);
    rd_t *r;
    int n;
    int64_t t0;

    /* never reads: 16 MiB cannot fit the socket buffers, so the send stalls and expires */
    http_cfg.send_timeout_ns = 200 * MS;
    r = rd_open_rcvbuf(port, 4096);
    n = atomic_load(&http_done);
    t0 = em_now();
    send_str(r, "GET /huge HTTP/1.1\r\n\r\n");
    wait_done(n + 1);
    assert(atomic_load(&http_rc) == -ETIMEDOUT);
    assert(since(t0) >= 150 && since(t0) < 2000);
    rd_close(r);

    /* slow but steady: 1 MiB every 100 ms over 16 MiB takes over a second, far over send_timeout
     * (300 ms), and the server is blocked in send for most of it, yet no send is stalled that long:
     * the connection lives and serves the next request */
    http_cfg.send_timeout_ns = 300 * MS;
    r = rd_open(port);
    n = atomic_load(&http_done);
    t0 = em_now();
    send_str(r, "GET /huge HTTP/1.1\r\n\r\n");
    {
        static char sink[1 << 20];
        size_t total = 0, hl = 0, since_nap = 0;
        while (!hl || total < hl + BODY) {
            ssize_t k = rd_recv(r, sink, sizeof sink);
            assert(k > 0);
            if (!hl) {
                const char *e = memmem(sink, (size_t)k, "\r\n\r\n", 4);
                assert(e && memmem(sink, (size_t)k, "Content-Length: 16777216\r\n", 26));
                hl = (size_t)(e - sink) + 4;
            }
            total += (size_t)k;
            since_nap += (size_t)k;
            if (since_nap >= sizeof sink) { since_nap = 0; assert(em_sleep_until(em_now() + 100 * MS) == 0); }
        }
        assert(total == hl + BODY);
    }
    assert(since(t0) > 2 * 300);
    send_str(r, "GET /after HTTP/1.1\r\n\r\n");
    expect_resp(r, "HTTP/1.1 200 ", "GET /after", NULL);
    assert(atomic_load(&http_done) == n); /* still being served */
    rd_close(r);

    /* send_timeout -1: no limit of ours: a pause far over the other values is fine */
    http_cfg.send_timeout_ns = -1;
    r = rd_open(port);
    send_str(r, "GET /huge HTTP/1.1\r\n\r\n");
    assert(em_sleep_until(em_now() + 500 * MS) == 0);
    {
        static char sink[1 << 16];
        size_t total = 0, hl = 0;
        while (!hl || total < hl + BODY) {
            ssize_t k = rd_recv(r, sink, sizeof sink);
            assert(k > 0);
            if (!hl) hl = (size_t)((char *)memmem(sink, (size_t)k, "\r\n\r\n", 4) - sink) + 4;
            total += (size_t)k;
        }
    }
    send_str(r, "GET /after HTTP/1.1\r\n\r\n");
    expect_resp(r, "HTTP/1.1 200 ", "GET /after", NULL);
    rd_close(r);

    /* the caller's scope is not ours: cancel -> -ECANCELED, its deadline -> -ETIMEDOUT */
    http_cfg.send_timeout_ns = 0;
    caller_scope(1);
    caller_scope(0);

    int r1 = http_stop(&s);
    int r2 = em_scope_close(&dl);
    return r1 ? r1 : r2;
}

int main(void)
{
    assert(em_run(1, main_fn, NULL) == 0);
    puts("http send timeout: stalled reader dropped, slow steady reader kept, caller scope kept verified");
    return 0;
}
