#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "internal.h"

#ifdef __SANITIZE_ADDRESS__
#include <sanitizer/asan_interface.h>
#include <sanitizer/common_interface_defs.h>
#endif

#define FIBER_SZ ((sizeof(em_fiber) + 15u) & ~(size_t)15u)
#define MAP_RESERVE 1024u /* VMAs left for libs, heap, etc. */
#define RING_ENTRIES 4096u

#define W em_W
__thread em_worker em_W;
size_t em_dbg_mmap_count;
size_t em_dbg_munmap_count;
size_t em_dbg_inflight(void) { return W.inflight; }

extern void em_tramp(void);
void em_fiber_main(void) __attribute__((noreturn));

static char *map_base(em_fiber *f)
{
    return (char *)f + FIBER_SZ - EM_MAP_SIZE;
}

static void rq_push(em_worker *w, em_fiber *f)
{
    f->next = NULL;
    if (w->rq_tail) w->rq_tail->next = f; else w->rq_head = f;
    w->rq_tail = f;
}

static em_fiber *rq_pop(em_worker *w)
{
    em_fiber *f = w->rq_head;
    if (f && !(w->rq_head = f->next)) w->rq_tail = NULL;
    return f;
}

/* Build the first frame so that em_ctx_switch's final ret enters em_tramp
 * with a call-ready (16-aligned) stack. f is 16-byte aligned. */
static void *init_frame(em_fiber *f)
{
#if defined(__x86_64__)
    uint64_t *t = (uint64_t *)f;
    t[-1] = t[-2] = 0;
    t[-3] = (uint64_t)(uintptr_t)em_tramp;      /* ret target */
    for (int i = 4; i <= 9; i++) t[-i] = 0;     /* r15 r14 r13 r12 rbx rbp */
    t[-10] = 0x1F80u | (0x037Full << 32);       /* MXCSR | x87 CW defaults */
    return &t[-10];
#elif defined(__aarch64__)
    char *sp = (char *)f - 176;
    memset(sp, 0, 176);                         /* FPCR = 0 included */
    *(uint64_t *)(sp + 88) = (uint64_t)(uintptr_t)em_tramp; /* x30 */
    return sp;
#else
#error unsupported architecture
#endif
}

static em_fiber *fiber_get(em_worker *w)
{
    em_fiber *f = w->free;
    if (f) {
        w->free = f->next;
    } else {
        if (w->nmapped >= w->cap) return NULL;
        char *base = mmap(NULL, EM_MAP_SIZE, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK, -1, 0);
        if (base == MAP_FAILED) return NULL;
        if (mprotect(base, EM_GUARD_SIZE, PROT_NONE) != 0) {
            munmap(base, EM_MAP_SIZE);
            return NULL;
        }
        em_dbg_mmap_count++;
        w->nmapped++;
        f = (em_fiber *)(base + EM_MAP_SIZE - FIBER_SZ);
    }
#ifdef __SANITIZE_ADDRESS__
    f->bottom = map_base(f) + EM_GUARD_SIZE;
    f->size = (size_t)((char *)f - (char *)f->bottom);
    f->fake = NULL;
    __asan_unpoison_memory_region(f->bottom, f->size); /* stale redzones from a previous life */
#endif
    return f;
}

static void switch_to_fiber(em_worker *w, em_fiber *f)
{
#ifdef __SANITIZE_ADDRESS__
    __sanitizer_start_switch_fiber(&w->sched_fake, f->bottom, f->size);
#endif
    em_ctx_switch(&w->sched_sp, f->sp);
#ifdef __SANITIZE_ADDRESS__
    __sanitizer_finish_switch_fiber(w->sched_fake, NULL, NULL);
#endif
}

void em_switch_to_sched(em_worker *w, em_fiber *f)
{
#ifdef __SANITIZE_ADDRESS__
    __sanitizer_start_switch_fiber(f->state == EM_DONE ? NULL : &f->fake,
                                   w->sched_bottom, w->sched_size);
#endif
    em_ctx_switch(&f->sp, w->sched_sp);
#ifdef __SANITIZE_ADDRESS__
    __sanitizer_finish_switch_fiber(f->fake, NULL, NULL);
#endif
}

void em_fiber_main(void)
{
    em_worker *w = &W;
#ifdef __SANITIZE_ADDRESS__
    __sanitizer_finish_switch_fiber(NULL, &w->sched_bottom, &w->sched_size);
#endif
    em_fiber *f = w->current;
    int r = f->fn(f->arg);
    em_fiber_finished(w, f, r);
    f->state = EM_DONE;
    em_switch_to_sched(w, f);
    __builtin_unreachable();
}

int em_spawn(em_scope *s, int (*fn)(void *), void *arg)
{
    em_worker *w = &W;
    em_fiber *f = fiber_get(w);
    if (!f) return -ENOMEM;
    f->fn = fn;
    f->arg = arg;
    f->state = EM_READY;
    f->wake_res = 0;
    f->cur_op = NULL;
    f->home = f->cur_scope = s;
    f->sib_prev = NULL;
    f->sib_next = s->children;
    if (s->children) s->children->sib_prev = f;
    s->children = f;
    s->nchildren++;
    f->sp = init_frame(f);
    w->live++;
    rq_push(w, f);
    return 0;
}

/* A fiber's function returned r (runs on f's stack). */
void em_fiber_finished(em_worker *w, em_fiber *f, int r)
{
    assert(f->cur_scope == f->home); /* every scope it opened is closed */
    if (r) {
        if (!f->home->first_err) f->home->first_err = r;
        em_scope_cancel_all(w, f->home);
    }
    em_scope_unlink(f);
}

void em_make_ready(em_worker *w, em_fiber *f)
{
    f->state = EM_READY;
    rq_push(w, f);
}

void em_yield(void)
{
    em_worker *w = &W;
    em_fiber *f = w->current;
    f->state = EM_READY;
    rq_push(w, f);
    em_switch_to_sched(w, f);
}

em_fiber *em_self(void) { return W.current; }

int em_park(void)
{
    em_worker *w = &W;
    em_fiber *f = w->current;
    if (em_scope_is_cancelled(f)) return -ECANCELED; /* a cancel that landed while we were READY */
    f->wake_res = 0;
    f->state = EM_PARKED;
    em_switch_to_sched(w, f);
    return f->wake_res;
}

void em_unpark(em_fiber *f)
{
    if (f->state != EM_PARKED) return;
    f->state = EM_READY;
    rq_push(&W, f);
}

static size_t read_max_map_count(void)
{
    size_t v = 65530; /* kernel default if /proc is unavailable */
    FILE *fp = fopen("/proc/sys/vm/max_map_count", "r");
    if (fp) {
        unsigned long x;
        if (fscanf(fp, "%lu", &x) == 1) v = x;
        fclose(fp);
    }
    return v;
}

static void fiber_unmap(em_fiber *f)
{
#ifdef __SANITIZE_ADDRESS__
    __asan_unpoison_memory_region(f->bottom, f->size); /* don't leave stale redzones for the next mapping */
#endif
    munmap(map_base(f), EM_MAP_SIZE);
    em_dbg_munmap_count++;
}

int em_run(int (*main_fn)(void *), void *arg)
{
    em_worker *w = &W;
    if (w->cap) return -EBUSY; /* nested em_run from inside a fiber */
    memset(w, 0, sizeof *w);

    struct io_uring_params p;
    memset(&p, 0, sizeof p);
    p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN |
              IORING_SETUP_SUBMIT_ALL | IORING_SETUP_CQSIZE;
    p.cq_entries = 2 * RING_ENTRIES;
    int rc = io_uring_queue_init_params(RING_ENTRIES, &w->ring, &p);
    if (rc < 0) return rc;

    size_t m = read_max_map_count();
    w->cap = m > MAP_RESERVE ? (m - MAP_RESERVE) / 2 : 0; /* 2 VMAs per fiber */

    em_scope root = {0};
    int deadlocked = 0;
    rc = em_spawn(&root, main_fn, arg);
    while (rc == 0) {
        em_fiber *f;
        while ((f = rq_pop(w))) {
            w->current = f;
            f->state = EM_RUNNING;
            switch_to_fiber(w, f);
            w->current = NULL;
            if (f->state == EM_DONE) {
                w->live--;
                f->next = w->free;
                w->free = f;
            }
        }
        if (w->live == 0) break;
        if (w->inflight == 0) {
            /* Nobody can make progress: cancel everything so parked fibers
             * unwind (host-language Drop runs). After a root cancel every wait
             * returns at once (park, ops) or completes (close), so a second
             * deadlock means an em bug: fail-stop. */
            if (deadlocked) abort();
            deadlocked = 1;
            em_scope_cancel_all(w, &root);
            continue;
        }
        em_io_reap(w);
    }
    while (w->free) {
        em_fiber *f = w->free;
        w->free = f->next;
        fiber_unmap(f);
    }
    io_uring_queue_exit(&w->ring);
    w->cap = 0;
    return deadlocked ? -EDEADLK : rc ? rc : root.first_err;
}
