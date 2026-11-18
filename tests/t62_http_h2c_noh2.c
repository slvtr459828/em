/* This binary does not link libem_http2: the h2c client preface (prior knowledge) is answered like
 * any other request that is not HTTP/1: 400, then close. */
#include "http_util.h"

static int main_fn(void *arg)
{
    (void)arg;
    em_scope dl, sc;
    struct hsrv s;
    uint16_t port;
    em_scope_open(&dl, em_now() + 20000000000ll);
    http_start(&s, &sc, &port);

    rd_t *r = rd_open(port);
    send_str(r, "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n");
    expect_resp(r, "HTTP/1.1 400 ", "", "close");
    expect_eof(r);
    rd_close(r);

    r = rd_open(port); /* the server is still fine */
    send_str(r, "GET /a HTTP/1.1\r\n\r\n");
    expect_resp(r, "HTTP/1.1 200 ", "GET /a", NULL);
    rd_close(r);

    int r1 = http_stop(&s);
    int r2 = em_scope_close(&dl);
    return r1 ? r1 : r2;
}

int main(void)
{
    assert(em_run(1, main_fn, NULL) == 0);
    puts("http h2c without libem_http2: 400");
    return 0;
}
