#include <assert.h>
#include <stdio.h>
#include <time.h>
#include "../include/em.h"

#define YIELDS 10000000L /* total across both fibers */

static int pp(void *arg)
{
    (void)arg;
    for (long i = 0; i < YIELDS / 2; i++) em_yield();
    return 0;
}

static int main_fn(void *arg)
{
    (void)arg;
    em_scope s;
    em_scope_open(&s, 0);
    assert(em_spawn(&s, pp, NULL) == 0);
    assert(em_spawn(&s, pp, NULL) == 0);
    return em_scope_close(&s);
}

int main(void)
{
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    assert(em_run(main_fn, NULL) == 0);
    clock_gettime(CLOCK_MONOTONIC, &b);
    double ns = (b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec);
    /* each yield = fiber->sched + sched->fiber = 2 raw context switches */
    printf("ping-pong: %ld yields, %.1f ns/yield, %.1f ns/switch\n",
           YIELDS, ns / YIELDS, ns / YIELDS / 2);
    return 0;
}
