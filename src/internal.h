#ifndef EM_INTERNAL_H
#define EM_INTERNAL_H

#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>
#include <pthread.h>
#include <semaphore.h>
#include <liburing.h>
#include "../include/em.h"

#define EM_GUARD_SIZE 4096u
#define EM_STACK_SIZE (256u * 1024u) /* includes the em_fiber at its top */
#define EM_MAP_SIZE   (EM_GUARD_SIZE + EM_STACK_SIZE)

/* io_uring only: user_data of CQEs that carry no em_op. Op pointers are even, so odd values
 * cannot collide. EM_MSG_CANCEL is what the TARGET ring sees (msg_ring's data
 * argument); EM_MSG_SENT is the SENDER's own completion of the msg_ring SQE. */
#define EM_MSG_CANCEL 1u
#define EM_MSG_SENT   3u
#define EM_MSG_JOBS   5u /* user_data of the armed READ on the worker's job-done eventfd */

#define EM_POOL_MAX 64 /* pool threads, process-wide */

/* EM_JOBWAIT: waiting in em_offload; not interruptible (the job uses the fiber's stack). */
enum { EM_READY, EM_RUNNING, EM_PARKED, EM_IOWAIT, EM_SCOPEWAIT, EM_DONE, EM_JOBWAIT };

/* A job for the pool. Every field but `res` and `next` is touched by the owning worker only;
 * the pool thread reads fn/arg, writes res, then pushes the job on the owner's done list. */
enum { EM_JOB_WAIT, EM_JOB_DONE, EM_JOB_ABANDONED };
typedef struct em_job {
    struct em_job *next;     /* pool queue OR done list, never both */
    struct em_slot *home;    /* owner's done list + eventfd */
    intptr_t (*fn)(void *);
    void     *arg;
    intptr_t  res;
    em_fiber *f;             /* waiter; stale once state == EM_JOB_ABANDONED */
    void    (*release)(struct em_job *); /* heap jobs: frees the job (and what hangs off it); NULL = on a fiber stack */
    uint8_t   state;
} em_job;

/* One in-flight kernel op; lives on the stack of the fiber that waits for it. */
typedef struct em_op {
    em_fiber *f;
    int32_t   res;
    uint8_t   uncancellable;
} em_op;

struct em_fiber {
    void     *sp;     /* saved sp while the fiber is not running */
    em_fiber *next;   /* run queue OR free-list, never both */
    int     (*fn)(void *);
    void     *arg;
    uint8_t   state;
    int       wake_res;   /* em_park result: 0 or -ECANCELED */
    em_op    *cur_op;     /* io_uring: non-NULL exactly while state == EM_IOWAIT */
    /* epoll backend, meaningful while state == EM_IOWAIT: */
    em_fiber *wnext;      /* next fiber waiting on the same fd direction */
    int       wfd;        /* fd being waited on, -1 = none (sleep) */
    uint8_t   wdir;       /* EM_WAIT_RD / EM_WAIT_WR */
    int       heap_idx;   /* index in the worker's timer heap, -1 = not in it */
    em_scope *home;       /* scope this fiber was spawned into */
    em_scope *cur_scope;  /* innermost scope open in this fiber (home if none) */
    em_fiber *sib_prev, *sib_next; /* home->children list */
#ifdef __SANITIZE_ADDRESS__
    void     *fake;   /* ASan fake stack handle */
    void     *bottom; /* usable stack range */
    size_t    size;
#endif
};

/* epoll backend, per worker. fdt is indexed by fd; epoll_event.data.u32 = fd. */
struct em_fdw {
    em_fiber *rd, *wr;   /* waiters, singly linked through em_fiber.wnext */
    uint8_t   registered; /* EPOLL_CTL_ADD done (once; the kernel drops it when the fd closes) */
    uint8_t   nb;         /* O_NONBLOCK set by em_read */
    uint8_t   rd_idle;    /* stream drained by a short recv; cleared by the next read event */
    uint8_t   kind;       /* 0 unknown, EM_FD_STREAM, EM_FD_OTHER, EM_FD_FILE (classified once; OTHER after EOF/error) */
};
enum { EM_FD_STREAM = 1, EM_FD_OTHER = 2, EM_FD_FILE = 3 };
struct em_timer { int64_t deadline; em_fiber *f; };
enum { EM_WAIT_RD, EM_WAIT_WR };

struct em_rt;
/* State of the READ armed on the done eventfd (io_uring). IDLE = its CQE was reaped, re-arm. */
enum { EM_JRD_IDLE, EM_JRD_ARMED, EM_JRD_CANCELLING, EM_JRD_STOPPED };

/* Fairness: a fiber that never parks (immediately-completed ops on epoll, or em_yield loops on
 * both backends) must still let the worker poll: cross-worker cancel, pool completions and
 * timers only arrive through the poll. */
#define EM_POLL_BUDGET 128

typedef struct em_worker {
    struct em_rt *rt;   /* NULL = not inside em_run */
    int       id;
    em_scope *root;     /* this worker's root scope, valid while the loop runs */
    size_t    msg_inflight; /* msg_ring SQEs whose own CQE has not been reaped */
    size_t    jobs;     /* jobs handed to the pool and not yet taken off the done list */
    uint64_t  jobs_buf; /* io_uring: target of the armed READ on the done eventfd */
    uint8_t   jobs_rd;  /* io_uring: EM_JRD_* */
    void     *sched_sp;
    em_fiber *current;
    em_fiber *rq_head, *rq_tail;
    em_fiber *free;
    size_t    live;     /* fibers not yet DONE */
    int       epoll;    /* backend: 0 = io_uring, 1 = epoll (same for every worker) */
    struct io_uring ring;           /* io_uring */
    size_t    inflight; /* io_uring: ops whose CQE has not been reaped; epoll: fibers waiting on an fd or timer */
    int       epfd, evfd;           /* epoll: instance, cross-worker cancel eventfd */
    struct em_fdw *fdt;             /* epoll: fd table, grown by doubling */
    size_t    nfd;
    struct em_timer *heap;          /* epoll: binary min-heap on deadline */
    size_t    nheap, capheap;
    int       budget;               /* ops/yields left before a non-blocking poll (see em_yield); reset by every poll */
#ifdef __SANITIZE_ADDRESS__
    void     *sched_fake;
    const void *sched_bottom;
    size_t    sched_size;
#endif
} em_worker;

/* Process-wide state, allocated once in em_run. The only atomics in em live
 * here and are touched on the mmap/munmap path, first-error publication and
 * startup/shutdown, never on the per-op path (plus the per-worker done lists of
 * the offload pool, one CAS per finished job). Everything else crosses threads
 * as a msg_ring CQE (io_uring) or an eventfd write (epoll). */
struct em_pool {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    pthread_attr_t  attr;   /* threads run on the caller's original CPU set, not on a worker's core */
    em_job *head, *tail;    /* FIFO of jobs not yet taken */
    int     queued, spare;  /* spare = threads not running a job */
    int     nthreads, stop;
    pthread_t th[EM_POOL_MAX];
};

struct em_rt {
    int    n;
    int  (*main_fn)(void *);
    void  *arg;
    _Atomic int    first_err;   /* first error in time, 0 = none yet */
    _Atomic size_t nmapped;     /* mappings owned by all workers (live + free-lists) */
    size_t cap;                 /* max mappings, process-wide */
    int    epoll;               /* backend, decided once in em_run */
    struct em_pool pool;
    sem_t  up;                  /* workers -> worker 0: "ready" then "finished" (phases never overlap) */
    sem_t  go;                  /* worker 0 -> workers: start (or abort) */
    sem_t  release;             /* worker 0 -> workers: all finished, rings may close */
    int    abort;               /* written by worker 0 before "go" */
    struct em_slot {
        struct em_rt *rt;
        int       id;
        int       cpu;
        _Atomic(em_job *) done; /* finished jobs, pushed by pool threads (LIFO), taken whole by the owner */
        int       done_fd;      /* eventfd written after each push; -1 = not created */
        int       wake_fd;      /* io_uring: ring fd (msg_ring target); epoll: eventfd. Written before "ready", read after "go" */
        pthread_t th;
    } slot[];
};

void em_ctx_switch(void **save_sp, void *load_sp);

extern __thread em_worker em_W;

/* fiber.c */
void em_switch_to_sched(em_worker *w, em_fiber *f);
void em_make_ready(em_worker *w, em_fiber *f);
void em_fiber_finished(em_worker *w, em_fiber *f, int r); /* called on f's own stack */
void em_stack_trim(void);   /* running fiber: release the dead stack pages below the caller's frame */
/* scope.c */
void em_scope_unlink(em_fiber *f);          /* remove f from home; wakes closer at 0 children */
int  em_scope_is_cancelled(const em_fiber *f);
void em_scope_interrupt_all(em_worker *w, em_scope *s);   /* wake/cancel every fiber under s */
void em_scope_cancel_all(em_worker *w, em_scope *s);      /* s->cancelled = 1 + interrupt */
/* io.c: public ops, each branching once on w->epoll */
/* pool.c */
int  em_pool_init(struct em_pool *p, size_t masksz, const void *mask);
void em_pool_join(struct em_pool *p);    /* after every worker is done: stops and joins the threads */
void em_pool_destroy(struct em_pool *p);
void em_jobs_reap(em_worker *w);         /* owner: take the done list; wake waiters, free abandoned jobs */
/* io_uring.c */
struct io_uring_sqe *ur_sqe_get(em_worker *w);   /* leaves room for a link timeout */
void ur_interrupt(em_worker *w, em_fiber *f);    /* f is EM_IOWAIT: submit ASYNC_CANCEL */
void ur_submit(em_worker *w);
void ur_reap(em_worker *w, int block);           /* submit, (block: wait for a CQE,) wake fibers */
void ur_jobs_start(em_worker *w);                /* arm the READ on the done eventfd */
void ur_jobs_stop(em_worker *w);                 /* cancel it and wait for its CQE (no CQE may outlive the worker) */
int  ur_job_wait(int64_t deadline_ns);           /* em_resolve: park until woken by ur_job_wake, the deadline (-ETIME) or cancel */
void ur_job_wake(em_worker *w, em_fiber *f);
int     ur_sleep_until(int64_t deadline_ns);
int     ur_accept(int lfd);
int     ur_connect(const struct sockaddr *a, socklen_t n);
ssize_t ur_recv(int fd, void *buf, size_t len);
ssize_t ur_read(int fd, void *buf, size_t len);
ssize_t ur_send(int fd, const void *buf, size_t len);
int     ur_shutdown(int fd, int how);
int     ur_close(int fd);
ssize_t ur_recvfrom(int fd, void *buf, size_t len, struct sockaddr *src, socklen_t *srclen);
ssize_t ur_sendto(int fd, const void *buf, size_t len, const struct sockaddr *dst, socklen_t n);
/* io_epoll.c */
int  ep_init(em_worker *w);                      /* 0 or -errno; w->evfd is the cancel eventfd */
void ep_exit(em_worker *w);
void ep_interrupt(em_worker *w, em_fiber *f);    /* f is EM_IOWAIT: unlink, -ECANCELED, ready */
int  ep_job_wait(void);                          /* em_resolve: 0 = woken by ep_job_wake, else -ETIMEDOUT / -ECANCELED */
void ep_job_wake(em_worker *w, em_fiber *f);
void ep_reap(em_worker *w, int block);           /* epoll_wait (block: until the earliest timer), wake fibers */
int     ep_sleep_until(int64_t deadline_ns);
int     ep_accept(int lfd);
int     ep_connect(const struct sockaddr *a, socklen_t n);
ssize_t ep_recv(int fd, void *buf, size_t len);
ssize_t ep_read(int fd, void *buf, size_t len);
ssize_t ep_send(int fd, const void *buf, size_t len);
int     ep_shutdown(int fd, int how);
int     ep_close(int fd);
void    ep_fd_forget(em_worker *w, int fd); /* wake waiters -ECANCELED, clean slot */
void    ep_fd_new(em_worker *w, int fd, int kind); /* fd em just created: clean slot + kind (EM_FD_*) */
ssize_t ep_recvfrom(int fd, void *buf, size_t len, struct sockaddr *src, socklen_t *srclen);
ssize_t ep_sendto(int fd, const void *buf, size_t len, const struct sockaddr *dst, socklen_t n);

/* Test-only. */
extern _Atomic size_t em_dbg_mmap_count;
extern _Atomic size_t em_dbg_munmap_count;
extern size_t em_dbg_map_cap; /* nonzero: overrides the process-wide mapping cap of the next em_run */
size_t em_dbg_inflight(void);
/* The em_resolve path (heap job, interruptible wait, abandoned on cancel/deadline) with fn(arg)
 * in place of getaddrinfo. Returns fn's result, or -ECANCELED / -ETIMEDOUT / -EPERM / -ENOMEM.
 * arg must outlive the call: an abandoned job still runs fn. */
int em_dbg_heap_job(intptr_t (*fn)(void *), void *arg);

#endif
