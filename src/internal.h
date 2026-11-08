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

enum { EM_READY, EM_RUNNING, EM_PARKED, EM_IOWAIT, EM_SCOPEWAIT, EM_DONE };

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
};
struct em_timer { int64_t deadline; em_fiber *f; };
enum { EM_WAIT_RD, EM_WAIT_WR };

struct em_rt;

typedef struct em_worker {
    struct em_rt *rt;   /* NULL = not inside em_run */
    int       id;
    em_scope *root;     /* this worker's root scope, valid while the loop runs */
    size_t    msg_inflight; /* msg_ring SQEs whose own CQE has not been reaped */
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
    int       budget;               /* epoll: immediately-completed ops left before a forced yield */
#ifdef __SANITIZE_ADDRESS__
    void     *sched_fake;
    const void *sched_bottom;
    size_t    sched_size;
#endif
} em_worker;

/* Process-wide state, allocated once in em_run. The only atomics in em live
 * here and are touched on the mmap/munmap path, first-error publication and
 * startup/shutdown, never on the per-op path. Everything else crosses threads
 * as a msg_ring CQE (io_uring) or an eventfd write (epoll). */
struct em_rt {
    int    n;
    int  (*main_fn)(void *);
    void  *arg;
    _Atomic int    first_err;   /* first error in time, 0 = none yet */
    _Atomic size_t nmapped;     /* mappings owned by all workers (live + free-lists) */
    size_t cap;                 /* max mappings, process-wide */
    int    epoll;               /* backend, decided once in em_run */
    sem_t  up;                  /* workers -> worker 0: "ready" then "finished" (phases never overlap) */
    sem_t  go;                  /* worker 0 -> workers: start (or abort) */
    sem_t  release;             /* worker 0 -> workers: all finished, rings may close */
    int    abort;               /* written by worker 0 before "go" */
    struct em_slot {
        struct em_rt *rt;
        int       id;
        int       cpu;
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
/* scope.c */
void em_scope_unlink(em_fiber *f);          /* remove f from home; wakes closer at 0 children */
int  em_scope_is_cancelled(const em_fiber *f);
void em_scope_interrupt_all(em_worker *w, em_scope *s);   /* wake/cancel every fiber under s */
void em_scope_cancel_all(em_worker *w, em_scope *s);      /* s->cancelled = 1 + interrupt */
/* io.c: public ops, each branching once on w->epoll */
/* io_uring.c */
struct io_uring_sqe *ur_sqe_get(em_worker *w);   /* leaves room for a link timeout */
void ur_interrupt(em_worker *w, em_fiber *f);    /* f is EM_IOWAIT: submit ASYNC_CANCEL */
void ur_submit(em_worker *w);
void ur_reap(em_worker *w);                      /* submit, wait for CQEs, wake fibers */
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
void ep_reap(em_worker *w);                      /* epoll_wait (until the earliest timer), wake fibers */
int     ep_sleep_until(int64_t deadline_ns);
int     ep_accept(int lfd);
int     ep_connect(const struct sockaddr *a, socklen_t n);
ssize_t ep_recv(int fd, void *buf, size_t len);
ssize_t ep_read(int fd, void *buf, size_t len);
ssize_t ep_send(int fd, const void *buf, size_t len);
int     ep_shutdown(int fd, int how);
int     ep_close(int fd);
void    ep_fd_forget(em_worker *w, int fd); /* wake waiters -ECANCELED, clean slot */
ssize_t ep_recvfrom(int fd, void *buf, size_t len, struct sockaddr *src, socklen_t *srclen);
ssize_t ep_sendto(int fd, const void *buf, size_t len, const struct sockaddr *dst, socklen_t n);

/* Test-only. */
extern _Atomic size_t em_dbg_mmap_count;
extern _Atomic size_t em_dbg_munmap_count;
extern size_t em_dbg_map_cap; /* nonzero: overrides the process-wide mapping cap of the next em_run */
size_t em_dbg_inflight(void);

#endif
