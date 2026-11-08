#include <stdio.h>
#include <stdlib.h>
#include "util.h"

#define N 1000

static int64_t dl[N];
static int order[N], nwoke;

static int sleeper(void *arg)
{
    int i = (int)(intptr_t)arg;
    int r = em_sleep_until(dl[i]);
    if (r) return r;
    assert(em_now() >= dl[i]);
    order[nwoke++] = i;
    return 0;
}

static int sleeper_ts(void *arg)
{
    (void)arg;
    em_scope s;
    em_scope_open(&s, em_now() + 20 * 1000000LL);
    int r = em_sleep_until(em_now() + 10000 * 1000000LL); /* scope deadline wins */
    em_scope_close(&s);
    assert(r == -ETIMEDOUT);
    return 0;
}

static int main_fn(void *arg)
{
    (void)arg;
    em_scope s;
    em_scope_open(&s, 0);
    uint64_t rng = 88172645463325252ull;
    int perm[N];
    for (int i = 0; i < N; i++) perm[i] = i;
    for (int i = N - 1; i > 0; i--) { int j = (int)(xs_next(&rng) % (uint64_t)(i + 1)); int t = perm[i]; perm[i] = perm[j]; perm[j] = t; }
    int64_t base = em_now() + 50 * 1000000LL;
    for (int i = 0; i < N; i++) dl[perm[i]] = base + (int64_t)perm[i] * 1000000LL; /* distinct, 1 ms apart */
    for (int i = 0; i < N; i++) assert(em_spawn(&s, sleeper, (void *)(intptr_t)perm[i]) == 0);
    assert(em_spawn(&s, sleeper_ts, NULL) == 0);
    assert(em_scope_close(&s) == 0);
    assert(nwoke == N);
    for (int i = 0; i < N; i++) assert(order[i] == i);
    printf("sleep: %d fibers woke in deadline order\n", N);
    return 0;
}

int main(void)
{
    assert(em_run(1, main_fn, NULL) == 0);
    return 0;
}
