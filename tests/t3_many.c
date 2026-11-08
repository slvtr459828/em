#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../include/em.h"

#define N 100000

static em_fiber *fibers[N];
static int order[N];
static int nrun, nparked;

static int worker(void *arg)
{
    int i = (int)(long)arg;
    fibers[i] = em_self();
    nparked++;
    em_park();
    order[nrun++] = i;
    return 0;
}

static int main_fn(void *arg)
{
    (void)arg;
    em_scope s;
    em_scope_open(&s, 0);
    for (long i = 0; i < N; i++) assert(em_spawn(&s, worker, (void *)i) == 0);
    while (nparked < N) em_yield();
    for (int i = 0; i < N; i++) em_unpark(fibers[i]);
    while (nrun < N) em_yield();
    return em_scope_close(&s);
}

int main(void)
{
    assert(em_run(1, main_fn, NULL) == 0);
    assert(nrun == N);
    for (int i = 0; i < N; i++) assert(order[i] == i);
    printf("many: %d fibers parked/unparked, FIFO order ok\n", N);
    return 0;
}
