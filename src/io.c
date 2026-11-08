#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "internal.h"

#define OP_UNCANCELLABLE 1 /* ignore cancel + deadline (close must always run) */
#define OP_NOLINK        2 /* caller handles the deadline itself (sleep) */

int64_t em_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* A ring that can't submit/reap leaves ops in flight that point into fiber
 * stacks; unwinding would break I1, so the only safe exit is fail-stop. */
static void fatal(const char *what, int err)
{
    fprintf(stderr, "em: %s: %s\n", what, strerror(-err));
    abort();
}

/* Make room for the SQE plus a possible link timeout in one go: a link must not
 * straddle two submits. */
struct io_uring_sqe *em_sqe_get(em_worker *w)
{
    while (io_uring_sq_space_left(&w->ring) < 2) {
        int r = io_uring_submit(&w->ring);
        if (r < 0 && r != -EBUSY && r != -EAGAIN && r != -EINTR) fatal("io_uring_submit", r);
    }
    return io_uring_get_sqe(&w->ring);
}

/* Fiber f is EM_IOWAIT, so its op's CQE has not been reaped and its frame is
 * live (I5): cancel by user_data. The CQE of the cancel itself is ignored. */
void em_io_interrupt(em_worker *w, em_fiber *f)
{
    if (f->cur_op->uncancellable) return;
    struct io_uring_sqe *sqe = em_sqe_get(w);
    io_uring_prep_cancel(sqe, f->cur_op, 0);
    io_uring_sqe_set_data64(sqe, 0);
}

void em_io_reap(em_worker *w)
{
    for (;;) {
        int r = io_uring_submit_and_wait(&w->ring, 1);
        if (r >= 0 || r == -EBUSY) break; /* EBUSY: CQ overflow pending, reap below */
        if (r != -EINTR && r != -EAGAIN) fatal("io_uring_submit_and_wait", r);
    }
    struct io_uring_cqe *cqe;
    unsigned head, n = 0;
    io_uring_for_each_cqe(&w->ring, head, cqe) {
        n++;
        em_op *op = io_uring_cqe_get_data(cqe);
        if (!op) continue; /* link timeout / async cancel */
        op->res = cqe->res;
        w->inflight--;
        op->f->cur_op = NULL;
        em_make_ready(w, op->f);
    }
    io_uring_cq_advance(&w->ring, n);
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
        *sqe = em_sqe_get(&em_W);
        return 0;
    }
    if (em_scope_is_cancelled(em_W.current)) return -ECANCELED;
    int64_t dl = cur_deadline();
    if (dl && em_now() >= dl) return -ETIMEDOUT;
    *sqe = em_sqe_get(&em_W);
    return 0;
}

/* Phase 2: attach the deadline, submit, park until THIS op's CQE (I1). The op
 * and the timespec live in this frame until then. */
static int op_end(struct io_uring_sqe *sqe, unsigned flags)
{
    em_worker *w = &em_W;
    em_fiber *f = w->current;
    em_op op = { .f = f, .uncancellable = !!(flags & OP_UNCANCELLABLE) };
    struct __kernel_timespec ts;
    int64_t dl = (flags & (OP_UNCANCELLABLE | OP_NOLINK)) ? 0 : cur_deadline();

    io_uring_sqe_set_data(sqe, &op);
    if (dl) {
        ts.tv_sec = dl / 1000000000;
        ts.tv_nsec = dl % 1000000000;
        sqe->flags |= IOSQE_IO_LINK;
        struct io_uring_sqe *t = io_uring_get_sqe(&w->ring); /* room guaranteed by em_sqe_get */
        assert(t);
        io_uring_prep_link_timeout(t, &ts, IORING_TIMEOUT_ABS);
        io_uring_sqe_set_data64(t, 0);
    }
    f->cur_op = &op;
    w->inflight++;
    f->state = EM_IOWAIT;
    em_switch_to_sched(w, f);
    assert(f->cur_op == NULL);

    /* -ECANCELED with the deadline passed and no scope cancel: our link timeout
     * fired. Otherwise (e.g. em_close from another fiber) it stays -ECANCELED. */
    if (op.res == -ECANCELED && dl && !em_scope_is_cancelled(f) && em_now() >= dl) return -ETIMEDOUT;
    return op.res;
}

int em_sleep_until(int64_t deadline_ns)
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

int em_tcp_listen(const struct sockaddr *a, socklen_t n, int backlog)
{
    int fd = socket(a->sa_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -errno;
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0 ||
        bind(fd, a, n) < 0 || listen(fd, backlog) < 0) {
        int e = errno;
        close(fd);
        return -e;
    }
    return fd;
}

int em_accept(int lfd)
{
    struct io_uring_sqe *sqe;
    int e = op_begin(0, &sqe);
    if (e) return e;
    io_uring_prep_accept(sqe, lfd, NULL, NULL, SOCK_CLOEXEC);
    return op_end(sqe, 0);
}

int em_connect(const struct sockaddr *a, socklen_t n)
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

ssize_t em_recv(int fd, void *buf, size_t len)
{
    struct io_uring_sqe *sqe;
    int e = op_begin(0, &sqe);
    if (e) return e;
    io_uring_prep_recv(sqe, fd, buf, len, 0);
    return op_end(sqe, 0);
}

ssize_t em_send(int fd, const void *buf, size_t len)
{
    struct io_uring_sqe *sqe;
    int e = op_begin(0, &sqe);
    if (e) return e;
    io_uring_prep_send(sqe, fd, buf, len, MSG_NOSIGNAL);
    return op_end(sqe, 0);
}

int em_send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        ssize_t r = em_send(fd, p, len);
        if (r < 0) return (int)r;
        p += r;
        len -= (size_t)r;
    }
    return 0;
}

int em_shutdown(int fd, int how)
{
    struct io_uring_sqe *sqe;
    int e = op_begin(0, &sqe);
    if (e) return e;
    io_uring_prep_shutdown(sqe, fd, how);
    return op_end(sqe, 0);
}

int em_close(int fd)
{
    struct io_uring_sqe *sqe;
    op_begin(OP_UNCANCELLABLE, &sqe);
    io_uring_prep_cancel_fd(sqe, fd, IORING_ASYNC_CANCEL_ALL);
    op_end(sqe, OP_UNCANCELLABLE); /* result: #cancelled or -ENOENT, either is fine */
    op_begin(OP_UNCANCELLABLE, &sqe);
    io_uring_prep_close(sqe, fd);
    return op_end(sqe, OP_UNCANCELLABLE);
}
