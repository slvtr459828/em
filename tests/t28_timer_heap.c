/* Many fibers wait on an fd with a scope deadline; about half get data first.
 * Those return the byte (their timer leaves the heap from the middle), the rest
 * -ETIMEDOUT, and none before its deadline. */
#include <stdio.h>
#include "util.h"
#include "../src/internal.h"

#define N 500

static int sv[N][2];
static int64_t dl[N], woke[N];
static ssize_t res[N];

static int waiter(void *arg)
{
    int i = (int)(intptr_t)arg;
    em_scope s;
    em_scope_open(&s, dl[i]);
    char b;
    res[i] = em_recv(sv[i][0], &b, 1);
    woke[i] = em_now();
    em_scope_close(&s);
    return 0;
}

static int main_fn(void *arg)
{
    (void)arg;
    uint64_t rng = 88172645463325252ull;
    em_scope s;
    em_scope_open(&s, 0);
    for (int i = 0; i < N; i++) {
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv[i]) == 0);
        assert(em_spawn(&s, waiter, (void *)(intptr_t)i) == 0);
    }
    /* After the setup loop (waiters read dl[i] only when they run, at the yield below), so a
     * slow host cannot eat the margin before anyone waits. */
    int64_t base = em_now();
    for (int i = 0; i < N; i++) dl[i] = base + 60000000LL + (int64_t)(xs_next(&rng) % 100) * 1000000LL;
    em_yield(); /* everyone is waiting */
    for (int i = 0; i < N; i++)
        if (xs_next(&rng) & 1) { assert(em_send(sv[i][1], "x", 1) == 1); }
    assert(em_scope_close(&s) == 0);
    int nd = 0, nt = 0;
    for (int i = 0; i < N; i++) {
        if (res[i] == 1) nd++;
        else { assert(res[i] == -ETIMEDOUT); assert(woke[i] >= dl[i]); nt++; }
        em_close(sv[i][0]);
        em_close(sv[i][1]);
    }
    assert(nd > 0 && nt > 0 && em_dbg_inflight() == 0);
    printf("timer heap (%s): %d got data, %d timed out\n", em_backend(), nd, nt);
    return 0;
}

int main(void)
{
    assert(em_run(1, main_fn, NULL) == 0);
    return 0;
}
