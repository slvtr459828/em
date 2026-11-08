/* em_close wakes every fiber waiting to read that fd with -ECANCELED. */
#include <stdio.h>
#include "util.h"

static int sv[2];
static int res[3] = { 1, 1, 1 };

static int waiter(void *arg)
{
    int i = (int)(intptr_t)arg;
    char b;
    res[i] = (int)em_recv(sv[0], &b, 1);
    return 0;
}

static int closer(void *arg)
{
    (void)arg;
    for (int i = 0; i < 4; i++) em_yield(); /* the three waiters are parked by now */
    return em_close(sv[0]);
}

static int main_fn(void *arg)
{
    (void)arg;
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    em_scope s;
    em_scope_open(&s, 0);
    for (int i = 0; i < 3; i++) assert(em_spawn(&s, waiter, (void *)(intptr_t)i) == 0);
    assert(em_spawn(&s, closer, NULL) == 0);
    assert(em_scope_close(&s) == 0);
    for (int i = 0; i < 3; i++) assert(res[i] == -ECANCELED);
    em_close(sv[1]);
    printf("close wakes (%s): 3 waiters got -ECANCELED\n", em_backend());
    return 0;
}

int main(void)
{
    assert(em_run(1, main_fn, NULL) == 0);
    return 0;
}
