/* HTTP/2: GET through nghttp2's client over h2c (plain) or h2 via ALPN (-DHTTP_TLS), against the same
 * handler the HTTP/1 tests use, which also answers h1 on the same server. M1: a request with a body is
 * answered 501 and never reaches the handler. */
#include "h2_util.h"

static int main_fn(void *arg)
{
    (void)arg;
    em_scope dl, sc;
    struct hsrv s;
    uint16_t port;
    em_scope_open(&dl, em_now() + 20000000000ll); /* a hang fails the test instead of stalling it */
    http_start(&s, &sc, &port);

    h2cl *c = h2_open(port);
    h2resp *x = h2_req(c, "GET", "/x", NULL, 0);
    h2resp *k4 = h2_req(c, "GET", "/4k", NULL, 0);
    h2resp *big = h2_req(c, "GET", "/big", NULL, 0);   /* 20000 bytes: more than one DATA frame */
    h2resp *hd = h2_req(c, "HEAD", "/4k", NULL, 0);
    h2resp *ver = h2_req(c, "GET", "/ver", NULL, 0);
    h2resp *err = h2_req(c, "GET", "/err", NULL, 0);
    h2resp *nor = h2_req(c, "GET", "/noresp", NULL, 0);
    h2resp *post = h2_req(c, "POST", "/echo", "abc", 3);
    h2_run(c);
    h2_expect(x, 200, "GET /x");
    h2_expect(k4, 200, k4_body);
    h2_expect(big, 200, big_body);
    /* HEAD: the headers of the GET, no body */
    assert(hd->closed && !hd->err && hd->status == 200 && hd->clen == 4096 && hd->blen == 0 && hd->has_date);
    h2_expect(ver, 200, "v=20 host=localhost"); /* version 20, :authority as host */
    h2_expect(err, 500, "");                    /* the handler failed: 500, like h1 */
    h2_expect(nor, 500, "");                    /* no response: 500 */
    assert(post->closed && post->status == 501 && post->clen == 0); /* M1: no request bodies, no Date either */
    assert(atomic_load(&handler_calls) == 7);   /* all but the POST */
    h2_close(c);

    /* the same server, the same handler, over HTTP/1.1 (TLS: no ALPN offered) */
    rd_t *r = rd_open(port);
    send_str(r, "GET /ver HTTP/1.1\r\nHost: h1host\r\n\r\n");
    expect_resp(r, "HTTP/1.1 200 ", "v=11 host=h1host", NULL);
    rd_close(r);

    int r1 = http_stop(&s);
    assert(r1 == 0);
    int r2 = em_scope_close(&dl);
    return r1 ? r1 : r2;
}

int main(void)
{
    assert(em_run(1, main_fn, NULL) == 0);
    puts("http2 get: h1 handler served over h2");
    return 0;
}
