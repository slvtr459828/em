#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <sched.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <unistd.h>
#include "internal.h"

#ifdef __SANITIZE_ADDRESS__
#include <sanitizer/asan_interface.h>
#include <sanitizer/common_interface_defs.h>
#endif

#define FIBER_SZ ((sizeof(em_fiber) + 15u) & ~(size_t)15u)
#define MAP_RESERVE 1024u /* VMAs left for libs, heap, etc. */
/* Ring memory counts against RLIMIT_MEMLOCK on some kernels (seen on 7.0), per
 * UID, and is released asynchronously after close. Default limit is 8 MiB, so
 * a ring must stay small: SQ 256 + CQ 1024 is ~40 KiB, ~200 workers per 8 MiB.
 * SQ only bounds one submit batch; a full CQ overflows into the kernel's
 * NODROP list (slower, still correct). Measured 2026-11-08, 1 worker, 1024
 * conns echo: ~500 CQEs per reap (max 1024), no overflow; SQ/CQ 256/1024 vs
 * 4096/8192 within run-to-run noise. Revisit if a worker holds far more ops. */
#define SQ_ENTRIES 256u
#define CQ_ENTRIES 1024u

#define W em_W
__thread em_worker em_W;
_Atomic size_t em_dbg_mmap_count;
_Atomic size_t em_dbg_munmap_count;
size_t em_dbg_map_cap;
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
        /* Process-wide cap, reserved before mmap so concurrent workers can't
         * overshoot it. Only this path (and fiber_unmap) touches the atomic. */
        struct em_rt *rt = w->rt;
        if (atomic_fetch_add(&rt->nmapped, 1) >= rt->cap) {
            atomic_fetch_sub(&rt->nmapped, 1);
            return NULL;
        }
        char *base = mmap(NULL, EM_MAP_SIZE, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK, -1, 0);
        if (base == MAP_FAILED) {
            atomic_fetch_sub(&rt->nmapped, 1);
            return NULL;
        }
        if (mprotect(base, EM_GUARD_SIZE, PROT_NONE) != 0) {
            munmap(base, EM_MAP_SIZE);
            atomic_fetch_sub(&rt->nmapped, 1);
            return NULL;
        }
        em_dbg_mmap_count++;
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

/* The scheduler itself spawns each worker's root fiber with current == NULL, so the
 * -EPERM guard lives in em_spawn, not here. */
static int spawn_fiber(em_scope *s, int (*fn)(void *), void *arg)
{
    em_worker *w = &W;
    em_fiber *f = fiber_get(w);
    if (!f) return -ENOMEM;
    f->fn = fn;
    f->arg = arg;
    f->state = EM_READY;
    f->wake_res = 0;
    f->cur_op = NULL;
    f->wfd = -1;
    f->heap_idx = -1;
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

int em_spawn(em_scope *s, int (*fn)(void *), void *arg)
{
    if (!W.current) return -EPERM;
    return spawn_fiber(s, fn, arg);
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
    if (!f) return;
    f->state = EM_READY;
    rq_push(w, f);
    em_switch_to_sched(w, f);
}

em_fiber *em_self(void) { return W.current; }

int em_park(void)
{
    em_worker *w = &W;
    em_fiber *f = w->current;
    if (!f) return -EPERM;
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

static void fiber_unmap(em_worker *w, em_fiber *f)
{
#ifdef __SANITIZE_ADDRESS__
    __asan_unpoison_memory_region(f->bottom, f->size); /* don't leave stale redzones for the next mapping */
#endif
    munmap(map_base(f), EM_MAP_SIZE);
    atomic_fetch_sub(&w->rt->nmapped, 1);
    em_dbg_munmap_count++;
}

int em_worker_id(void) { return W.id; }
int em_worker_count(void) { return W.rt ? W.rt->n : 0; }

/* ---- cross-worker cancel -------------------------------------------------
 * io_uring: a msg_ring SQE posts a CQE (user_data EM_MSG_CANCEL) straight into
 * the target worker's ring; the target cancels its root when it reaps it
 * (io_uring.c). epoll: a write to the target's eventfd, which its epoll reports
 * (io_epoll.c). Either way the target's ring/eventfd stays open until EVERY
 * worker has finished (see worker_main), so wake_fd of slot k is always valid
 * here: a worker that already finished its root just never looks at the
 * message, and teardown discards it. */
static void cancel_others(em_worker *w)
{
    struct em_rt *rt = w->rt;
    for (int k = 0; k < rt->n; k++) {
        if (k == w->id) continue;
        if (w->epoll) {
            uint64_t one = 1;
            /* EAGAIN = counter saturated = a cancel is already pending */
            if (write(rt->slot[k].wake_fd, &one, sizeof one) < 0 && errno != EAGAIN) abort();
            continue;
        }
        struct io_uring_sqe *sqe = ur_sqe_get(w);
        io_uring_prep_msg_ring(sqe, rt->slot[k].wake_fd, 0, EM_MSG_CANCEL, 0);
        io_uring_sqe_set_data64(sqe, EM_MSG_SENT);
        w->msg_inflight++;
    }
    if (!w->epoll) ur_submit(w); /* now, not at the next reap: the caller may run for a long time or exit */
}

void em_cancel_all(void)
{
    em_worker *w = &W;
    if (!w->rt) return;
    cancel_others(w);
    em_scope_cancel_all(w, w->root);
}

/* ---- worker lifecycle ---------------------------------------------------- */

static int worker_init(em_worker *w, struct em_rt *rt, int id)
{
    memset(w, 0, sizeof *w);
    w->rt = rt;
    w->id = id;
    w->epoll = rt->epoll;

    cpu_set_t one;
    CPU_ZERO(&one);
    CPU_SET(rt->slot[id].cpu, &one);
    if (sched_setaffinity(0, sizeof one, &one) != 0) return -errno;

    /* Pool threads wake this worker through done_fd (closed in worker_exit, after the pool is joined). */
    rt->slot[id].done_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (rt->slot[id].done_fd < 0) {
        int e = -errno;
        rt->slot[id].done_fd = -1;
        return e;
    }

    if (w->epoll) {
        int e = ep_init(w);
        if (e) return e;
        rt->slot[id].wake_fd = w->evfd;
        return 0;
    }

    struct io_uring_params p;
    memset(&p, 0, sizeof p);
    p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN |
              IORING_SETUP_SUBMIT_ALL | IORING_SETUP_CQSIZE;
    p.cq_entries = CQ_ENTRIES;
    int rc = io_uring_queue_init_params(SQ_ENTRIES, &w->ring, &p);
    if (rc < 0) return rc;
    rt->slot[id].wake_fd = w->ring.ring_fd;
    return 0;
}

static void worker_exit(em_worker *w, int init_ok)
{
    while (w->free) {
        em_fiber *f = w->free;
        w->free = f->next;
        fiber_unmap(w, f);
    }
    if (init_ok) {
        if (w->epoll) ep_exit(w); else io_uring_queue_exit(&w->ring);
    }
    if (w->rt->slot[w->id].done_fd >= 0) close(w->rt->slot[w->id].done_fd);
    w->rt = NULL;
}

/* The first error in time wins; whoever wins tells everyone else to stop. */
static void publish_result(em_worker *w, int r)
{
    int none = 0;
    if (r && atomic_compare_exchange_strong(&w->rt->first_err, &none, r)) cancel_others(w);
}

/* Scheduler loop of one worker, then the shutdown handshake. */
static void worker_main(em_worker *w)
{
    struct em_rt *rt = w->rt;
    em_scope root = {0};
    w->root = &root;
    int deadlocked = 0;
    int rc = spawn_fiber(&root, rt->main_fn, rt->arg);
    if (!w->epoll) ur_jobs_start(w);
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
        if (w->epoll) ep_reap(w); else ur_reap(w);
    }
    publish_result(w, deadlocked ? -EDEADLK : rc ? rc : root.first_err ? root.first_err :
                      root.cancelled ? -ECANCELED : 0);
    /* Abandoned em_resolve jobs still run on the pool and will land on our done list; wait
     * for them (em.h), then no CQE of ours may remain that targets this worker's memory. */
    while (w->jobs) { if (w->epoll) ep_reap(w); else ur_reap(w); }
    if (!w->epoll) ur_jobs_stop(w);
    /* Our msg_ring SQEs must complete before the ring can go away, and no ring
     * may go away before every sender is past this point (below). */
    while (w->msg_inflight) ur_reap(w); /* io_uring only: epoll sends none */
    w->root = NULL;
}

static void sem_wait_n(sem_t *s, int n)
{
    while (n > 0) {
        if (sem_wait(s) == 0) n--;
        else assert(errno == EINTR);
    }
}

static void sem_post_n(sem_t *s, int n)
{
    while (n-- > 0) {
        int r = sem_post(s);
        assert(r == 0);
        (void)r;
    }
}

static void note_error(struct em_rt *rt, int e)
{
    int none = 0;
    atomic_compare_exchange_strong(&rt->first_err, &none, e);
}

static void *worker_thread(void *p)
{
    struct em_slot *s = p;
    struct em_rt *rt = s->rt;
    em_worker *w = &W;
    int e = worker_init(w, rt, s->id);
    if (e) note_error(rt, e);
    sem_post_n(&rt->up, 1);   /* ready */
    sem_wait_n(&rt->go, 1); /* go (or abort) */
    if (!rt->abort) {
        worker_main(w);
        sem_post_n(&rt->up, 1);   /* finished: my ring may now be closed */
        sem_wait_n(&rt->release, 1); /* every worker has finished */
    }
    worker_exit(w, e == 0);
    return NULL;
}

/* k-th (wrapping) CPU of the allowed set */
static int nth_cpu(const cpu_set_t *set, int k)
{
    k %= CPU_COUNT(set);
    for (int c = 0;; c++)
        if (CPU_ISSET(c, set) && k-- == 0) return c;
}

/* EM_BACKEND=auto|uring|epoll (default auto), read once per em_run. Returns 0
 * (io_uring), 1 (epoll) or -EINVAL. auto probes with a throwaway ring and
 * switches to epoll only when io_uring is unavailable (blocked or absent); the
 * answer holds for the whole run. A forced backend is never second-guessed:
 * if it cannot start, worker_init reports why. */
static int pick_backend(void)
{
    const char *e = getenv("EM_BACKEND");
    if (e && strcmp(e, "auto") != 0) {
        if (strcmp(e, "uring") == 0) return 0;
        if (strcmp(e, "epoll") == 0) return 1;
        return -EINVAL;
    }
    struct io_uring probe;
    struct io_uring_params p;
    memset(&p, 0, sizeof p);
    p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
    int rc = io_uring_queue_init_params(2, &probe, &p);
    if (rc == 0) {
        io_uring_queue_exit(&probe);
        return 0;
    }
    return rc == -EPERM || rc == -ENOSYS || rc == -EACCES || rc == -ENOMEM;
}

const char *em_backend(void) { return W.epoll ? "epoll" : "uring"; }

int em_run(int nworkers, int (*main_fn)(void *), void *arg)
{
    em_worker *w = &W;
    if (w->rt) return -EBUSY; /* nested em_run from inside a worker */
    if (nworkers < 0) return -EINVAL;
    int backend = pick_backend();
    if (backend < 0) return backend;

    cpu_set_t orig;
    if (sched_getaffinity(0, sizeof orig, &orig) != 0) return -errno;
    int n = nworkers ? nworkers : CPU_COUNT(&orig);

    struct em_rt *rt = calloc(1, sizeof *rt + (size_t)n * sizeof rt->slot[0]);
    if (!rt) return -ENOMEM;
    if (sem_init(&rt->up, 0, 0) != 0 || sem_init(&rt->go, 0, 0) != 0 ||
        sem_init(&rt->release, 0, 0) != 0) {
        int e = -errno;
        free(rt);
        return e;
    }
    rt->n = n;
    rt->epoll = backend;
    rt->main_fn = main_fn;
    rt->arg = arg;
    size_t m = read_max_map_count();
    rt->cap = em_dbg_map_cap ? em_dbg_map_cap
                             : m > MAP_RESERVE ? (m - MAP_RESERVE) / 2 : 0; /* 2 VMAs per fiber */
    for (int i = 0; i < n; i++) {
        rt->slot[i] = (struct em_slot){ .rt = rt, .id = i, .cpu = nth_cpu(&orig, i), .done_fd = -1 };
    }
    int pe = em_pool_init(&rt->pool, sizeof orig, &orig);
    if (pe) {
        sem_destroy(&rt->up);
        sem_destroy(&rt->go);
        sem_destroy(&rt->release);
        free(rt);
        return pe;
    }

    /* Threads first, so they inherit the caller's mask and signal mask. */
    int started = 0;
    for (int i = 1; i < n; i++) {
        int e = pthread_create(&rt->slot[i].th, NULL, worker_thread, &rt->slot[i]);
        if (e) { note_error(rt, -e); break; }
        started++;
    }

    int e0 = worker_init(w, rt, 0);
    if (e0) note_error(rt, e0);
    sem_wait_n(&rt->up, started); /* every ring exists and is published */
    rt->abort = atomic_load(&rt->first_err) != 0;
    sem_post_n(&rt->go, started);
    if (!rt->abort) {
        worker_main(w);
        sem_wait_n(&rt->up, started); /* everyone finished, nobody will send any more */
        em_pool_join(&rt->pool);      /* every job is back (workers waited), so no pool thread touches a done_fd later */
        sem_post_n(&rt->release, started);
    }
    worker_exit(w, e0 == 0);
    for (int i = 1; i <= started; i++) pthread_join(rt->slot[i].th, NULL);

    if (sched_setaffinity(0, sizeof orig, &orig) != 0) note_error(rt, -errno);
    int r = atomic_load(&rt->first_err);
    em_pool_destroy(&rt->pool);
    sem_destroy(&rt->up);
    sem_destroy(&rt->go);
    sem_destroy(&rt->release);
    free(rt);
    return r;
}
