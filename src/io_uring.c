#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>
#include "internal.h"

#define OP_UNCANCELLABLE 1 /* ignore cancel + deadline (close must always run) */
#define OP_NOLINK        2 /* caller handles the deadline itself (sleep): no entry in the deadline heap */

/* A ring that can't submit/reap leaves ops in flight that point into fiber
 * stacks; unwinding would break I1, so the only safe exit is fail-stop. */
static void fatal(const char *what, int err)
{
    fprintf(stderr, "em: %s: %s\n", what, strerror(-err));
    abort();
}

/* A free SQE. Submitting when the SQ is full is safe at any point: every SQE already queued
 * is fully prepared (op_end sets data before it queues the timer SQE) and nothing links SQEs. */
struct io_uring_sqe *ur_sqe_get(em_worker *w)
{
    while (io_uring_sq_space_left(&w->ring) < 1) {
        int r = io_uring_submit(&w->ring);
        if (r < 0 && r != -EBUSY && r != -EAGAIN && r != -EINTR) fatal("io_uring_submit", r);
    }
    return io_uring_get_sqe(&w->ring);
}

/* Push queued SQEs to the kernel now. Transient errors are left for the next
 * submit (ur_reap / ur_sqe_get / the shutdown drain all submit). */
void ur_submit(em_worker *w)
{
    int r = io_uring_submit(&w->ring);
    if (r < 0 && r != -EBUSY && r != -EAGAIN && r != -EINTR) fatal("io_uring_submit", r);
}

/* Fiber f is EM_IOWAIT, so its op's CQE has not been reaped and its frame is
 * live (I5): cancel by user_data. The CQE of the cancel itself is ignored. */
void ur_interrupt(em_worker *w, em_fiber *f)
{
    if (f->cur_op->uncancellable) return;
    struct io_uring_sqe *sqe = ur_sqe_get(w);
    io_uring_prep_cancel(sqe, f->cur_op, 0);
    io_uring_sqe_set_data64(sqe, 0);
}

/* The worker keeps one READ armed on its done eventfd (pool threads write to it). Its CQE
 * (EM_MSG_JOBS) is the only wake-up the pool needs: ur_reap takes the done list, then re-arms.
 * No wake-up is lost: the counter is read before the list is taken, so a push after that
 * leaves the counter nonzero and the next READ completes at once. */
static void jobs_arm(em_worker *w)
{
    struct io_uring_sqe *sqe = ur_sqe_get(w);
    io_uring_prep_read(sqe, w->rt->slot[w->id].done_fd, &w->jobs_buf, sizeof w->jobs_buf, 0);
    io_uring_sqe_set_data64(sqe, EM_MSG_JOBS);
    w->jobs_rd = EM_JRD_ARMED;
}

void ur_jobs_start(em_worker *w)
{
    jobs_arm(w);
}

/* Shutdown: the READ targets w->jobs_buf and the eventfd, so its CQE must be reaped before
 * the worker's memory and the ring go away. */
void ur_jobs_stop(em_worker *w)
{
    struct io_uring_sqe *sqe = ur_sqe_get(w);
    io_uring_prep_cancel64(sqe, EM_MSG_JOBS, 0);
    io_uring_sqe_set_data64(sqe, 0);
    w->jobs_rd = EM_JRD_CANCELLING;
    while (w->jobs_rd != EM_JRD_STOPPED) ur_reap(w, 1);
}

/* Deadlines: every in-flight op with a scope deadline has an entry (deadline, fiber) in the
 * worker heap, and the worker has at most ONE kernel timeout (EM_MSG_TIMER) armed, at the heap
 * minimum or earlier. A later deadline costs a heap push, no SQE. The timeout is moved earlier
 * with timeout_update (user_data 0: its CQE, e.g. -ENOENT when the timeout just fired, is
 * ignored; the fire's own CQE re-arms for whatever the heap holds). SQ order guarantees an
 * update never hits a re-armed timeout. All SQEs read w->tmo_ts at submit and it always holds
 * the newest target, so they agree. tmo_armed != 0 until the timeout's CQE is reaped. */
static void timer_set(em_worker *w, int64_t dl)
{
    w->tmo_ts.tv_sec = dl / 1000000000;
    w->tmo_ts.tv_nsec = dl % 1000000000;
    struct io_uring_sqe *sqe = ur_sqe_get(w);
    if (w->tmo_armed) {
        io_uring_prep_timeout_update(sqe, &w->tmo_ts, EM_MSG_TIMER, IORING_TIMEOUT_ABS);
        io_uring_sqe_set_data64(sqe, 0);
    } else {
        io_uring_prep_timeout(sqe, &w->tmo_ts, 0, IORING_TIMEOUT_ABS);
        io_uring_sqe_set_data64(sqe, EM_MSG_TIMER);
    }
    w->tmo_armed = dl;
}

/* The timeout's CQE was reaped (every op CQE of the batch too, so each fiber left in the heap
 * is still IOWAIT with a live op): interrupt the expired ones, then re-arm for the next. A fiber
 * whose op finished meanwhile is gone from the heap, so a fire may find nothing: just re-arms. */
static void timer_fired(em_worker *w)
{
    w->tmo_armed = 0;
    int64_t now = em_now();
    while (w->nheap && w->heap[0].deadline <= now) {
        em_fiber *f = w->heap[0].f;
        em_heap_remove(w, f);
        ur_interrupt(w, f); /* op_end turns the -ECANCELED into -ETIMEDOUT */
    }
    if (w->nheap) timer_set(w, w->heap[0].deadline);
}

/* Shutdown: the timeout's CQE must be reaped before the ring goes away. No fiber is left, so
 * the heap is empty and timer_fired will not re-arm. */
void ur_timer_stop(em_worker *w)
{
    if (!w->tmo_armed) return;
    assert(w->nheap == 0);
    struct io_uring_sqe *sqe = ur_sqe_get(w);
    io_uring_prep_cancel64(sqe, EM_MSG_TIMER, 0);
    io_uring_sqe_set_data64(sqe, 0);
    while (w->tmo_armed) ur_reap(w, 1);
}

void ur_reap(em_worker *w, int block)
{
    if (!block) {
        /* COOP_TASKRUN: completions (msg_ring, eventfd READ, timeouts) only appear when we enter
         * the kernel with GETEVENTS; plain io_uring_submit would skip the enter when SQ is empty. */
        int r = io_uring_submit_and_get_events(&w->ring);
        if (r < 0 && r != -EBUSY && r != -EAGAIN && r != -EINTR) fatal("io_uring_submit_and_get_events", r);
    } else for (;;) {
        int r = io_uring_submit_and_wait(&w->ring, 1);
        if (r >= 0 || r == -EBUSY) break; /* EBUSY: CQ overflow pending, reap below */
        if (r != -EINTR && r != -EAGAIN) fatal("io_uring_submit_and_wait", r);
    }
    struct io_uring_cqe *cqe;
    unsigned head, n = 0;
    int cancel_root = 0, jobs_cqe = 0, timer_cqe = 0;
    io_uring_for_each_cqe(&w->ring, head, cqe) {
        n++;
        if (cqe->user_data & 1) { /* not an op pointer */
            if (cqe->user_data == EM_MSG_CANCEL) {
                cancel_root = 1; /* acted on below, once every CQE of the batch is reaped */
            } else if (cqe->user_data == EM_MSG_JOBS) {
                jobs_cqe = 1;    /* done list is taken below, after the whole batch */
            } else if (cqe->user_data == EM_MSG_TIMER) {
                timer_cqe = 1;   /* after the batch: see timer_fired */
            } else {
                assert(cqe->user_data == EM_MSG_SENT);
                w->msg_inflight--;
                /* Rings outlive all senders, so a failure here means a lost cancel. */
                if (cqe->res < 0) fatal("msg_ring", cqe->res);
            }
            continue;
        }
        em_op *op = io_uring_cqe_get_data(cqe);
        if (!op) continue; /* async cancel / timeout update */
        op->res = cqe->res;
        w->inflight--;
        op->f->cur_op = NULL;
        if (op->f->heap_idx >= 0) em_heap_remove(w, op->f);
        em_make_ready(w, op->f);
    }
    io_uring_cq_advance(&w->ring, n);
    if (timer_cqe) timer_fired(w);
    if (jobs_cqe) {
        em_jobs_reap(w); /* a CQE with a value or -ECANCELED alike: the list is the truth */
        if (w->jobs_rd == EM_JRD_CANCELLING) w->jobs_rd = EM_JRD_STOPPED; else jobs_arm(w);
    }
    w->budget = EM_POLL_BUDGET;
    if (cancel_root && w->root) em_scope_cancel_all(w, w->root);
}

/* Effective deadline of the calling fiber, 0 = none. */
static int64_t cur_deadline(void)
{
    return em_W.current->cur_scope->deadline;
}

/* Phase 1 of an op: cancel/deadline check, then a fresh SQE for the caller to
 * prepare. Returns 0 or the error to hand back (nothing was submitted). */
static int op_begin(unsigned flags, struct io_uring_sqe **sqe)
{
    if (flags & OP_UNCANCELLABLE) {
        *sqe = ur_sqe_get(&em_W);
        return 0;
    }
    if (em_scope_is_cancelled(em_W.current)) return -ECANCELED;
    int64_t dl = cur_deadline();
    if (dl && em_now() >= dl) return -ETIMEDOUT;
    /* op_end's heap push must not fail once the SQE is prepared. */
    if (dl) {
        int e = em_heap_reserve(&em_W);
        if (e) return e;
    }
    *sqe = ur_sqe_get(&em_W);
    return 0;
}

/* Phase 2: register the deadline, queue, park until THIS op's CQE (I1). The op lives in this
 * frame until then. */
static int op_end(struct io_uring_sqe *sqe, unsigned flags)
{
    em_worker *w = &em_W;
    em_fiber *f = w->current;
    em_op op = { .f = f, .uncancellable = !!(flags & OP_UNCANCELLABLE) };
    int64_t dl = (flags & (OP_UNCANCELLABLE | OP_NOLINK)) ? 0 : cur_deadline();

    io_uring_sqe_set_data(sqe, &op);
    if (dl) {
        int e = em_heap_push(w, f, dl);
        assert(e == 0); /* capacity reserved in op_begin */
        (void)e;
        if (!w->tmo_armed || dl < w->tmo_armed) timer_set(w, dl);
    }
    f->cur_op = &op;
    w->inflight++;
    f->state = EM_IOWAIT;
    em_switch_to_sched(w, f);
    assert(f->cur_op == NULL);

    /* A cancel that hits an op running in io-wq completes it with -EINTR (seen
     * for recvmsg). io_uring ops never see user signals, so -EINTR here always
     * means "cancelled": one result code for cancel on both backends. */
    if (op.res == -EINTR) op.res = -ECANCELED;
    /* -ECANCELED with the deadline passed and no scope cancel: our deadline
     * expired (timer_fired). Otherwise (e.g. ur_close from another fiber) it stays -ECANCELED. */
    if (op.res == -ECANCELED && dl && !em_scope_is_cancelled(f) && em_now() >= dl) return -ETIMEDOUT;
    return op.res;
}

int ur_sleep_until(int64_t deadline_ns)
{
    struct io_uring_sqe *sqe;
    int e = op_begin(0, &sqe);
    if (e) return e;
    /* A timeout op can't carry a link timeout, so fold the scope deadline in. */
    int64_t dl = cur_deadline();
    int64_t target = (dl && dl < deadline_ns) ? dl : deadline_ns;
    struct __kernel_timespec ts = { target / 1000000000, target % 1000000000 };
    io_uring_prep_timeout(sqe, &ts, 0, IORING_TIMEOUT_ABS);
    int r = op_end(sqe, OP_NOLINK); /* ts is read at submit, but stays live anyway */
    if (r != -ETIME) return r;
    return target == deadline_ns ? 0 : -ETIMEDOUT;
}

/* em_resolve's wait: a timeout op at the scope deadline (none = never fires) that the done
 * list cancels (ur_job_wake) like any other op, so the fiber leaves its frame only after
 * this op's CQE (I1). Returns -ETIME when the deadline passed, -ECANCELED when woken. */
int ur_job_wait(int64_t deadline_ns)
{
    struct io_uring_sqe *sqe = ur_sqe_get(&em_W);
    int64_t t = deadline_ns ? deadline_ns : INT64_MAX;
    struct __kernel_timespec ts = { t / 1000000000, t % 1000000000 };
    io_uring_prep_timeout(sqe, &ts, 0, IORING_TIMEOUT_ABS);
    return op_end(sqe, OP_NOLINK);
}

void ur_job_wake(em_worker *w, em_fiber *f)
{
    ur_interrupt(w, f);
}

int ur_accept(int lfd)
{
    struct io_uring_sqe *sqe;
    int e = op_begin(0, &sqe);
    if (e) return e;
    io_uring_prep_accept(sqe, lfd, NULL, NULL, SOCK_CLOEXEC);
    return op_end(sqe, 0);
}

int ur_connect(const struct sockaddr *a, socklen_t n)
{
    int fd = socket(a->sa_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -errno;
    struct io_uring_sqe *sqe;
    int e = op_begin(0, &sqe);
    if (!e) {
        io_uring_prep_connect(sqe, fd, a, n);
        e = op_end(sqe, 0);
    }
    if (e < 0) {
        close(fd); /* no op references fd any more: its CQE is in */
        return e;
    }
    return fd;
}

ssize_t ur_recv(int fd, void *buf, size_t len)
{
    struct io_uring_sqe *sqe;
    int e = op_begin(0, &sqe);
    if (e) return e;
    io_uring_prep_recv(sqe, fd, buf, len, 0);
    return op_end(sqe, 0);
}

ssize_t ur_read(int fd, void *buf, size_t len)
{
    struct io_uring_sqe *sqe;
    int e = op_begin(0, &sqe);
    if (e) return e;
    io_uring_prep_read(sqe, fd, buf, len, (uint64_t)-1); /* -1: current position (pipes, signalfd, ...) */
    return op_end(sqe, 0);
}

ssize_t ur_send(int fd, const void *buf, size_t len)
{
    struct io_uring_sqe *sqe;
    int e = op_begin(0, &sqe);
    if (e) return e;
    io_uring_prep_send(sqe, fd, buf, len, MSG_NOSIGNAL);
    return op_end(sqe, 0);
}

int ur_shutdown(int fd, int how)
{
    struct io_uring_sqe *sqe;
    int e = op_begin(0, &sqe);
    if (e) return e;
    io_uring_prep_shutdown(sqe, fd, how);
    return op_end(sqe, 0);
}

int ur_close(int fd)
{
    struct io_uring_sqe *sqe;
    op_begin(OP_UNCANCELLABLE, &sqe);
    io_uring_prep_cancel_fd(sqe, fd, IORING_ASYNC_CANCEL_ALL);
    op_end(sqe, OP_UNCANCELLABLE); /* result: #cancelled or -ENOENT, either is fine */
    op_begin(OP_UNCANCELLABLE, &sqe);
    io_uring_prep_close(sqe, fd);
    return op_end(sqe, OP_UNCANCELLABLE);
}

/* msghdr and iovec live in this frame until the CQE (I1); the payload goes
 * straight between the kernel and the caller's buffer. */
ssize_t ur_recvfrom(int fd, void *buf, size_t len, struct sockaddr *src, socklen_t *srclen)
{
    struct io_uring_sqe *sqe;
    int e = op_begin(0, &sqe);
    if (e) return e;
    struct iovec iov = { buf, len };
    struct msghdr mh = { .msg_name = src, .msg_namelen = (src && srclen) ? *srclen : 0,
                         .msg_iov = &iov, .msg_iovlen = 1 };
    io_uring_prep_recvmsg(sqe, fd, &mh, 0);
    int r = op_end(sqe, 0);
    if (r >= 0 && src && srclen) *srclen = mh.msg_namelen;
    return r;
}

ssize_t ur_sendto(int fd, const void *buf, size_t len, const struct sockaddr *dst, socklen_t n)
{
    struct io_uring_sqe *sqe;
    int e = op_begin(0, &sqe);
    if (e) return e;
    struct iovec iov = { (void *)(uintptr_t)buf, len }; /* iovec is not const-aware; the kernel only reads */
    struct msghdr mh = { .msg_name = (void *)(uintptr_t)dst, .msg_namelen = n,
                         .msg_iov = &iov, .msg_iovlen = 1 };
    io_uring_prep_sendmsg(sqe, fd, &mh, MSG_NOSIGNAL);
    return op_end(sqe, 0);
}
