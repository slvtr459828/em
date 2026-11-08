/* A fiber whose I/O always completes immediately must not starve the others:
 * a 10 ms sleeper has to wake within 50 ms (on epoll the op never reaches the
 * scheduler, so this is what the op budget is for). */
#include <stdio.h>
#include "util.h"

static int sv[2];
static int stop;
static int64_t t0;

/* echo on a socket that always has data: recv finds the byte we sent the
 * previous round */
static int echoer(void *arg)
{
    (void)arg;
    char c = 'x';
    assert(em_send_all(sv[1], &c, 1) == 0);
    while (!stop && em_now() - t0 < 2000000000LL) {
        if (em_recv(sv[0], &c, 1) != 1 || em_send_all(sv[1], &c, 1) != 0) return -EIO;
    }
    return 0;
}

static int sleeper(void *arg)
{
    (void)arg;
    assert(em_sleep_until(em_now() + 10000000LL) == 0);
    int64_t ms = (em_now() - t0) / 1000000;
    stop = 1;
    printf("fairness (%s): 10 ms sleeper woke after %lld ms\n", em_backend(), (long long)ms);
    assert(ms < 50);
    return 0;
}

static int main_fn(void *arg)
{
    (void)arg;
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    t0 = em_now();
    em_scope s;
    em_scope_open(&s, 0);
    assert(em_spawn(&s, echoer, NULL) == 0);
    assert(em_spawn(&s, sleeper, NULL) == 0);
    assert(em_scope_close(&s) == 0);
    em_close(sv[0]);
    em_close(sv[1]);
    return 0;
}

int main(void)
{
    assert(em_run(1, main_fn, NULL) == 0);
    return 0;
}
