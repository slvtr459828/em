#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "util.h"
#include "../src/internal.h"

#define ROUNDS 10000

static uint64_t rng;
static unsigned rnd(unsigned n) { return (unsigned)(xs_next(&rng) >> 33) % n; }

struct round {
    em_scope *s;
    int rfd, wfd;
    unsigned a_yields, b_yields1, b_yields2;
    int send;
    ssize_t res;
    char buf[4];
};

static int fiber_a(void *arg)
{
    struct round *r = arg;
    for (unsigned i = 0; i < r->a_yields; i++) em_yield();
    r->res = em_recv(r->rfd, r->buf, sizeof r->buf);
    return 0; /* a nonzero return would itself cancel the scope; keep the race pure */
}

static int fiber_b(void *arg)
{
    struct round *r = arg;
    for (unsigned i = 0; i < r->b_yields1; i++) em_yield();
    if (r->send) em_send(r->wfd, "data", 4);
    for (unsigned i = 0; i < r->b_yields2; i++) em_yield();
    em_scope_cancel(r->s);
    return 0;
}

static int main_fn(void *arg)
{
    (void)arg;
    unsigned nd = 0, nc = 0;
    for (int i = 0; i < ROUNDS; i++) {
        int sv[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);
        em_scope s;
        em_scope_open(&s, 0);
        struct round r = { .s = &s, .rfd = sv[0], .wfd = sv[1], .a_yields = rnd(4),
                           .b_yields1 = rnd(4), .b_yields2 = rnd(4), .send = (int)rnd(4) != 0,
                           .res = 12345 };
        if (rnd(2)) { em_spawn(&s, fiber_a, &r); em_spawn(&s, fiber_b, &r); }
        else        { em_spawn(&s, fiber_b, &r); em_spawn(&s, fiber_a, &r); }
        assert(em_scope_close(&s) == -ECANCELED); /* b always cancels the scope */
        if (r.res == 4) { assert(memcmp(r.buf, "data", 4) == 0); nd++; }
        else { assert(r.res == -ECANCELED); nc++; }
        assert(em_dbg_inflight() == 0);
        em_close(sv[0]);
        em_close(sv[1]);
    }
    printf("cancel race: %u data, %u -ECANCELED\n", nd, nc);
    assert(nd > 0 && nc > 0);
    return 0;
}

int main(void)
{
    const char *e = getenv("SEED");
    uint64_t seed = e ? strtoull(e, NULL, 0) : (uint64_t)time(NULL) * 2654435761u ^ (uint64_t)getpid();
    if (!seed) seed = 1;
    rng = seed;
    printf("cancel race: seed=%llu (SEED=%llu to replay)\n", (unsigned long long)seed, (unsigned long long)seed);
    fflush(stdout);
    assert(em_run(main_fn, NULL) == 0);
    return 0;
}
