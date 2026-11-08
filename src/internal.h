#ifndef EM_INTERNAL_H
#define EM_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <liburing.h>
#include "../include/em.h"

#define EM_GUARD_SIZE 4096u
#define EM_STACK_SIZE (256u * 1024u) /* includes the em_fiber at its top */
#define EM_MAP_SIZE   (EM_GUARD_SIZE + EM_STACK_SIZE)

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
    em_op    *cur_op;     /* non-NULL exactly while state == EM_IOWAIT */
    em_scope *home;       /* scope this fiber was spawned into */
    em_scope *cur_scope;  /* innermost scope open in this fiber (home if none) */
    em_fiber *sib_prev, *sib_next; /* home->children list */
#ifdef __SANITIZE_ADDRESS__
    void     *fake;   /* ASan fake stack handle */
    void     *bottom; /* usable stack range */
    size_t    size;
#endif
};

typedef struct em_worker {
    void     *sched_sp;
    em_fiber *current;
    em_fiber *rq_head, *rq_tail;
    em_fiber *free;
    size_t    live;     /* fibers not yet DONE */
    size_t    nmapped;  /* mappings owned (live + free-list) */
    size_t    cap;      /* max mappings, from vm.max_map_count */
    struct io_uring ring;
    size_t    inflight; /* ops submitted whose CQE has not been reaped */
#ifdef __SANITIZE_ADDRESS__
    void     *sched_fake;
    const void *sched_bottom;
    size_t    sched_size;
#endif
} em_worker;

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
/* io.c */
struct io_uring_sqe *em_sqe_get(em_worker *w);   /* leaves room for a link timeout */
void em_io_interrupt(em_worker *w, em_fiber *f); /* f is EM_IOWAIT: submit ASYNC_CANCEL */
void em_io_reap(em_worker *w);

/* Test-only. Single worker, so plain globals. */
extern size_t em_dbg_mmap_count;
extern size_t em_dbg_munmap_count;
size_t em_dbg_inflight(void);

#endif
