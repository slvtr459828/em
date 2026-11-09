/* A fiber that only loops on em_yield() still lets its worker poll: a cross-worker
 * em_cancel_all and a timer on the same worker both get through (both backends). */
#include <stdio.h>
#include <unistd.h>
#include "util.h"

static int64_t timer_lat; /* how late the 10 ms sleeper woke, ns; written by worker 0 only */

static volatile int seen; /* the accept waiter got -ECANCELED (worker 0 only) */

/* Never parks: the run queue is never empty, so only the budgeted poll in em_yield can deliver
 * the cancel to the accept waiter (a parking cancel point here would empty the queue). */
static int spinner(void *arg)
{
    (void)arg;
    while (!seen) em_yield();
    return 0;
}

static int waiter(void *arg)
{
    uint16_t port;
    int lfd = listen_local(&port);
    int r = em_accept(lfd);
    seen = 1;
    em_close(lfd);
    (void)arg;
    return r;
}

static int sleeper(void *arg)
{
    (void)arg;
    int64_t t0 = em_now();
    assert(em_sleep_until(t0 + 10000000) == 0);
    timer_lat = em_now() - t0 - 10000000;
    return 0;
}

static int canceller(void *arg)
{
    (void)arg;
    em_sleep_until(em_now() + 100000000);
    em_cancel_all();
    return 0;
}

static int main_fn(void *arg)
{
    (void)arg;
    em_scope s;
    em_scope_open(&s, 0);
    if (em_worker_id() == 0) {
        assert(em_spawn(&s, waiter, NULL) == 0);
        assert(em_spawn(&s, spinner, NULL) == 0);
        assert(em_spawn(&s, sleeper, NULL) == 0);
    } else {
        assert(em_spawn(&s, canceller, NULL) == 0);
    }
    return em_scope_close(&s);
}

int main(void)
{
    alarm(10); /* a broken poll hangs em_run: fail instead */
    int64_t t0 = em_now();
    assert(em_run(2, main_fn, NULL) == -ECANCELED);
    int64_t total = em_now() - t0;
    assert(total < 2000000000LL);
    assert(timer_lat >= 0 && timer_lat < 50000000LL); /* woke while the spinner was running */
    printf("yield poll (%s): cancel_all reached a yield-looping worker in %lld ms, 10 ms timer %lld ms late\n",
           em_backend(), (long long)(total / 1000000), (long long)(timer_lat / 1000000));
    return 0;
}
