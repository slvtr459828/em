/* HTTP/2: streams of one connection run concurrently (one slow handler does not hold the others),
 * and max_concurrent_streams is advertised and honoured. */
#include "h2_util.h"

static int main_fn(void *arg)
{
    (void)arg;
    em_scope dl, sc;
    struct hsrv s;
    uint16_t port;
    em_scope_open(&dl, em_now() + 20000000000ll);

    /* limit 4: the slow request holds one slot for 300 ms, the other 7 go through the remaining 3 */
    http_cfg.max_concurrent_streams = 4;
    http_start(&s, &sc, &port);
    h2cl *c = h2_open(port);
    h2resp *slow = h2_req(c, "GET", "/slow", NULL, 0);
    h2resp *f[7];
    for (int i = 0; i < 7; i++) {
        char p[32];
        snprintf(p, sizeof p, "/f%d", i);
        f[i] = h2_req(c, "GET", p, NULL, 0);
    }
    h2_run(c);
    assert(nghttp2_session_get_remote_settings(c->ses, NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS) == 4);
    h2_expect(slow, 200, "slow");
    for (int i = 0; i < 7; i++) {
        char b[32];
        snprintf(b, sizeof b, "GET /f%d", i);
        h2_expect(f[i], 200, b);
        assert(f[i]->order < slow->order); /* every fast one finished before the slow one */
    }
    h2_close(c);
    assert(http_stop(&s) == 0);

    /* default limit (100): 79 streams at once, the slow one still last */
    http_cfg.max_concurrent_streams = 0;
    http_start(&s, &sc, &port);
    c = h2_open(port);
    slow = h2_req(c, "GET", "/slow", NULL, 0);
    for (int i = 0; i < 78; i++) h2_req(c, "GET", "/y", NULL, 0);
    h2_run(c);
    assert(nghttp2_session_get_remote_settings(c->ses, NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS) == 100);
    h2_expect(slow, 200, "slow");
    for (int i = 1; i < c->n; i++) {
        h2_expect(&c->resp[i], 200, "GET /y");
        assert(c->resp[i].order < slow->order);
    }
    h2_close(c);
    int r1 = http_stop(&s);
    assert(r1 == 0);
    int r2 = em_scope_close(&dl);
    return r1 ? r1 : r2;
}

int main(void)
{
    assert(em_run(1, main_fn, NULL) == 0);
    puts("http2 streams: concurrent, slow one last, limit advertised");
    return 0;
}
