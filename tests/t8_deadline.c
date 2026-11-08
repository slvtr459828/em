#include <stdio.h>
#include "util.h"

static int main_fn(void *arg)
{
    (void)arg;
    uint16_t port;
    int lfd = listen_local(&port);
    int c = connect_local(port); /* connect completes via the backlog; never accepted */
    assert(c >= 0);
    char b;

    em_scope s;
    int64_t t0 = em_now();
    em_scope_open(&s, t0 + 50 * 1000000LL);
    ssize_t r = em_recv(c, &b, 1);
    int64_t ms = (em_now() - t0) / 1000000;
    assert(em_scope_close(&s) == 0); /* the timeout is the op's result, not a child error */
    printf("deadline: recv -> %zd after %lld ms\n", r, (long long)ms);
    assert(r == -ETIMEDOUT);
    assert(ms >= 50 && ms < 150);

    /* already-expired deadline: no op is submitted */
    em_scope_open(&s, em_now() - 1);
    assert(em_recv(c, &b, 1) == -ETIMEDOUT);
    em_scope_close(&s);

    /* an inner deadline later than the outer one does not extend it */
    em_scope o, i;
    t0 = em_now();
    em_scope_open(&o, t0 + 30 * 1000000LL);
    em_scope_open(&i, t0 + 10000 * 1000000LL);
    assert(em_recv(c, &b, 1) == -ETIMEDOUT);
    ms = (em_now() - t0) / 1000000;
    assert(ms >= 30 && ms < 130);
    em_scope_close(&i);
    em_scope_close(&o);

    em_close(c);
    em_close(lfd);
    return 0;
}

int main(void)
{
    assert(em_run(1, main_fn, NULL) == 0);
    return 0;
}
