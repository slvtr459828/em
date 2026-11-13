/* Deadlines through the per-worker timer (io_uring: one kernel timeout per worker, moved by
 * timeout_update; epoll: the heap alone). Many deadlines in every order, a deadline that never
 * fires because the fiber finished (spurious fire), scope cancel, expired-before-op, nesting,
 * and shutdown with the worker timeout still armed. */
#include <stdio.h>
#include "util.h"
#include "../src/internal.h"

#define N 30
#define MS 1000000LL
#define SLACK (80 * MS)

static int sv[N + 8][2];
static int64_t dl[N + 8], woke[N + 8];
static ssize_t res[N + 8];

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

static void pair(int i) { assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv[i]) == 0); }

static void expect_timeout(int i)
{
    assert(res[i] == -ETIMEDOUT);
    assert(woke[i] >= dl[i] && woke[i] < dl[i] + SLACK);
}

static int main_fn(void *arg)
{
    (void)arg;
    em_scope s;
    em_scope_open(&s, 0);
    char b;

    /* a: each new deadline is earlier than the armed one, so the timeout is moved N times */
    int64_t base = em_now() + 400 * MS;
    for (int i = 0; i < N; i++) {
        pair(i);
        dl[i] = base - i * 5 * MS;
        assert(em_spawn(&s, waiter, (void *)(intptr_t)i) == 0);
    }
    assert(em_scope_close(&s) == 0);
    for (int i = 0; i < N; i++) expect_timeout(i);

    /* b: a later deadline arrives while an earlier one is armed: heap push only */
    em_scope_open(&s, 0);
    int a = N, c = N + 1;
    pair(a); pair(c);
    dl[a] = em_now() + 40 * MS;
    dl[c] = em_now() + 150 * MS;
    assert(em_spawn(&s, waiter, (void *)(intptr_t)a) == 0);
    assert(em_spawn(&s, waiter, (void *)(intptr_t)c) == 0);
    assert(em_scope_close(&s) == 0);
    expect_timeout(a);
    expect_timeout(c);

    /* c: the fiber with the armed (earliest) deadline finishes first; the timeout fires with
     * nothing expired and must re-arm for the later one, which still times out on time */
    em_scope_open(&s, 0);
    int x = N + 2, y = N + 3;
    pair(x); pair(y);
    dl[x] = em_now() + 30 * MS;
    dl[y] = em_now() + 150 * MS;
    assert(em_spawn(&s, waiter, (void *)(intptr_t)x) == 0);
    assert(em_spawn(&s, waiter, (void *)(intptr_t)y) == 0);
    em_yield();
    assert(em_send(sv[x][1], "x", 1) == 1);
    assert(em_sleep_until(em_now() + 60 * MS) == 0); /* past x's deadline: the spurious fire */
    assert(em_scope_close(&s) == 0);
    assert(res[x] == 1 && woke[x] < dl[x]);
    expect_timeout(y);

    /* d: scope cancel while waiting under a far deadline; the armed timeout stays far away */
    int far = N + 4, near = N + 5;
    pair(far); pair(near);
    em_scope fs;
    em_scope_open(&fs, em_now() + 10000 * MS);
    em_spawn(&fs, waiter, (void *)(intptr_t)far);
    dl[far] = 0; /* waiter's own scope adds no deadline; fs's 10 s applies */
    em_yield();
    em_scope_cancel(&fs);
    assert(em_scope_close(&fs) == -ECANCELED);
    assert(res[far] == -ECANCELED);
    /* a short deadline afterwards moves the (10 s) armed timeout earlier */
    dl[near] = em_now() + 30 * MS;
    em_scope_open(&s, 0);
    assert(em_spawn(&s, waiter, (void *)(intptr_t)near) == 0);
    assert(em_scope_close(&s) == 0);
    expect_timeout(near);

    /* e: deadline already passed: no op */
    em_scope_open(&s, em_now() - 1);
    assert(em_recv(sv[0][0], &b, 1) == -ETIMEDOUT);
    em_scope_close(&s);

    /* f: nested: effective deadline is the min with the parent, either way round */
    em_scope o, i;
    int64_t t0 = em_now();
    em_scope_open(&o, t0 + 30 * MS);
    em_scope_open(&i, t0 + 10000 * MS);
    assert(em_recv(sv[0][0], &b, 1) == -ETIMEDOUT);
    assert(em_now() - t0 >= 30 * MS && em_now() - t0 < 30 * MS + SLACK);
    em_scope_close(&i);
    em_scope_close(&o);
    t0 = em_now();
    em_scope_open(&o, t0 + 10000 * MS);
    em_scope_open(&i, t0 + 30 * MS);
    assert(em_recv(sv[0][0], &b, 1) == -ETIMEDOUT);
    assert(em_now() - t0 >= 30 * MS && em_now() - t0 < 30 * MS + SLACK);
    em_scope_close(&i);
    em_scope_close(&o);

    /* g: leave with a 5 s deadline armed (the fiber below ends by cancel); shutdown must reap it */
    em_scope_open(&fs, em_now() + 5000 * MS);
    em_spawn(&fs, waiter, (void *)(intptr_t)far);
    em_yield();
    em_scope_cancel(&fs);
    assert(em_scope_close(&fs) == -ECANCELED);

    for (int k = 0; k < N + 6; k++) { em_close(sv[k][0]); em_close(sv[k][1]); }
    assert(em_dbg_inflight() == 0);
    printf("deadline timer (%s): ok\n", em_backend());
    return 0;
}

int main(void)
{
    int64_t t0 = em_now();
    assert(em_run(1, main_fn, NULL) == 0);
    assert(em_now() - t0 < 4000 * MS); /* the armed 5 s timeout did not hold up shutdown */
    return 0;
}
