/* fn runs on a pool thread, not on the worker: another tid, not pinned to the worker's core,
 * em calls from it return -EPERM, and it may use the caller's stack. */
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "util.h"

struct args { pid_t worker_tid; int ncpu; int buf[8]; };

static intptr_t fn(void *p)
{
    struct args *a = p;
    assert(syscall(SYS_gettid) != a->worker_tid);
    assert(em_sleep_until(em_now() + 1000000) == -EPERM);
    assert(em_offload(fn, NULL, NULL) == -EPERM);
    cpu_set_t set;
    assert(sched_getaffinity(0, sizeof set, &set) == 0);
    assert(CPU_COUNT(&set) == a->ncpu); /* the caller's original set, not the worker's single core */
    for (int i = 0; i < 8; i++) a->buf[i] = i * i; /* the caller's stack */
    return 41;
}

static int main_fn(void *arg)
{
    int ncpu = (int)(intptr_t)arg;
    struct args a = { .worker_tid = (pid_t)syscall(SYS_gettid), .ncpu = ncpu };
    { intptr_t r_; assert(em_offload(fn, &a, &r_) == 0 && r_ == 41); }
    for (int i = 0; i < 8; i++) assert(a.buf[i] == i * i);
    { intptr_t r_; assert(em_offload(fn, &a, &r_) == 0 && r_ == 41); } /* reuses the pool thread */
    printf("offload thread (%s): runs off the worker, em calls from fn are -EPERM\n", em_backend());
    return 0;
}

int main(void)
{
    cpu_set_t set;
    assert(sched_getaffinity(0, sizeof set, &set) == 0);
    assert(em_offload(fn, NULL, NULL) == -EPERM); /* outside em_run */
    assert(em_run(1, main_fn, (void *)(intptr_t)CPU_COUNT(&set)) == 0);
    return 0;
}
