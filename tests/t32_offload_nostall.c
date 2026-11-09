/* em_offload does not stall the worker: while one fiber is inside a 200 ms blocking call,
 * a ping-pong pair on the same (only) worker keeps running. */
#include <stdio.h>
#include <unistd.h>
#include "util.h"

static int sv[2];
static volatile int done;
static long rounds;

static intptr_t slow(void *arg)
{
    (void)arg;
    usleep(200 * 1000);
    return 99;
}

static int off(void *arg)
{
    (void)arg;
    { intptr_t r_; assert(em_offload(slow, NULL, &r_) == 0 && r_ == 99); }
    done = 1;
    return 0;
}

static int pinger(void *arg)
{
    (void)arg;
    char c = 'p';
    while (!done) {
        assert(em_send(sv[0], &c, 1) == 1);
        assert(em_recv(sv[0], &c, 1) == 1);
        rounds++;
    }
    assert(em_shutdown(sv[0], SHUT_WR) == 0);
    return 0;
}

static int ponger(void *arg)
{
    (void)arg;
    char c;
    for (;;) {
        ssize_t n = em_recv(sv[1], &c, 1);
        if (n == 0) return 0;
        assert(n == 1);
        assert(em_send(sv[1], &c, 1) == 1);
    }
}

static int main_fn(void *arg)
{
    (void)arg;
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    em_scope s;
    em_scope_open(&s, 0);
    assert(em_spawn(&s, off, NULL) == 0);
    assert(em_spawn(&s, pinger, NULL) == 0);
    assert(em_spawn(&s, ponger, NULL) == 0);
    assert(em_scope_close(&s) == 0);
    em_close(sv[0]);
    em_close(sv[1]);
    assert(rounds >= 1000);
    printf("offload no stall (%s): %ld ping-pong rounds during a 200 ms blocking call\n", em_backend(), rounds);
    return 0;
}

int main(void)
{
    assert(em_run(1, main_fn, NULL) == 0);
    return 0;
}
