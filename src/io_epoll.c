#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#include "internal.h"

/* epoll backend (D8). One I/O op = one nonblocking syscall try; only on EAGAIN
 * one wait (park on the fd's read/write list until epoll reports it) and one
 * more try. Nothing sits between the epoll event and the fiber but the run
 * queue. A fiber waiting here has no kernel op in flight, so cancel is a
 * synchronous unlink (I1 holds trivially: the fiber runs again only after its
 * own wait ended).
 *
 * fds are registered once, edge-triggered, for IN|OUT|RDHUP. This only works if
 * the syscall is always retried before parking (no edge is lost: the worker is
 * single-threaded, so an event between EAGAIN and epoll_wait stays in the ready
 * list). Every fd em creates starts from a clean slot (ep_fd_forget), so a number
 * reused after a plain close() is fine. An fd NOT created by em, used with em and
 * then closed with plain close(), leaves its slot stale: the next foreign fd with
 * that number is never added to epoll and its first wait parks forever. Contract
 * (em.h): such fds must be closed with em_close. */

#define EP_EVENTS 64         /* epoll_wait batch; the rest stays in the kernel ready list */
#define EV_WAKE   UINT32_MAX     /* data.u32 of the cancel eventfd (an fd number can't be this) */
#define EV_JOBS   (UINT32_MAX - 1) /* ... of the job-done eventfd (pool threads write it) */

/* ---- timer heap: binary min-heap on deadline; f->heap_idx makes removal O(log n) ---- */

static void heap_set(em_worker *w, size_t i, struct em_timer t)
{
    w->heap[i] = t;
    t.f->heap_idx = (int)i;
}

static void heap_up(em_worker *w, size_t i)
{
    struct em_timer t = w->heap[i];
    while (i > 0 && t.deadline < w->heap[(i - 1) / 2].deadline) {
        heap_set(w, i, w->heap[(i - 1) / 2]);
        i = (i - 1) / 2;
    }
    heap_set(w, i, t);
}

static void heap_down(em_worker *w, size_t i)
{
    struct em_timer t = w->heap[i];
    for (;;) {
        size_t c = 2 * i + 1;
        if (c >= w->nheap) break;
        if (c + 1 < w->nheap && w->heap[c + 1].deadline < w->heap[c].deadline) c++;
        if (t.deadline <= w->heap[c].deadline) break;
        heap_set(w, i, w->heap[c]);
        i = c;
    }
    heap_set(w, i, t);
}

static int heap_push(em_worker *w, em_fiber *f, int64_t deadline)
{
    if (w->nheap == w->capheap) { /* grows with the number of timer waiters; rare, never steady state */
        size_t n = w->capheap ? w->capheap * 2 : 64;
        struct em_timer *h = realloc(w->heap, n * sizeof *h);
        if (!h) return -ENOMEM;
        w->heap = h;
        w->capheap = n;
    }
    w->heap[w->nheap] = (struct em_timer){ deadline, f };
    heap_up(w, w->nheap++);
    return 0;
}

static void heap_remove(em_worker *w, em_fiber *f)
{
    size_t i = (size_t)f->heap_idx;
    f->heap_idx = -1;
    struct em_timer last = w->heap[--w->nheap];
    if (i == w->nheap) return;
    heap_set(w, i, last);
    heap_up(w, i);
    heap_down(w, (size_t)last.f->heap_idx);
}

/* ---- waiting ---- */

static em_fiber **wlist(em_worker *w, int fd, int dir)
{
    return dir == EM_WAIT_RD ? &w->fdt[fd].rd : &w->fdt[fd].wr;
}

/* f stops waiting (it is already off its fd list): hand it res and make it ready. */
static void wake(em_worker *w, em_fiber *f, int res)
{
    if (f->heap_idx >= 0) heap_remove(w, f);
    f->wfd = -1;
    f->wake_res = res;
    w->inflight--;
    em_make_ready(w, f);
}

static void wake_list(em_worker *w, em_fiber **head, int res)
{
    em_fiber *f = *head;
    *head = NULL;
    while (f) {
        em_fiber *nxt = f->wnext;
        wake(w, f, res);
        f = nxt;
    }
}

static void unlink_wait(em_worker *w, em_fiber *f)
{
    if (f->wfd < 0) return;
    em_fiber **p = wlist(w, f->wfd, f->wdir);
    while (*p != f) p = &(*p)->wnext;
    *p = f->wnext;
}

void ep_interrupt(em_worker *w, em_fiber *f)
{
    unlink_wait(w, f);
    wake(w, f, -ECANCELED);
}

/* Make sure fdt has a slot for fd. */
static int fdt_ensure(em_worker *w, int fd)
{
    if (fd < 0) return -EBADF;
    if ((size_t)fd < w->nfd) return 0;
    size_t n = w->nfd ? w->nfd : 64;
    while (n <= (size_t)fd) n *= 2;
    struct em_fdw *t = realloc(w->fdt, n * sizeof *t);
    if (!t) return -ENOMEM;
    memset(t + w->nfd, 0, (n - w->nfd) * sizeof *t);
    w->fdt = t;
    w->nfd = n;
    return 0;
}

static int fd_register(em_worker *w, int fd)
{
    int e = fdt_ensure(w, fd);
    if (e) return e;
    struct em_fdw *s = &w->fdt[fd];
    if (!s->registered) {
        struct epoll_event ev = { .events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET, .data.u32 = (uint32_t)fd };
        if (epoll_ctl(w->epfd, EPOLL_CTL_ADD, fd, &ev) < 0) return -errno;
        s->registered = 1;
    }
    return 0;
}

static void park(em_worker *w, em_fiber *f)
{
    w->inflight++;
    f->state = EM_IOWAIT;
    em_switch_to_sched(w, f);
}

/* Park on fd's read or write list (plus the scope deadline in the heap) until
 * an event, cancel, em_close or the deadline. 0 = event: try the syscall again. */
static int wait_fd(int fd, int dir)
{
    em_worker *w = &em_W;
    em_fiber *f = w->current;
    int e = fd_register(w, fd);
    if (e) return e;
    int64_t dl = f->cur_scope->deadline;
    if (dl && (e = heap_push(w, f, dl))) return e;
    em_fiber **head = wlist(w, fd, dir);
    f->wnext = *head;
    *head = f;
    f->wfd = fd;
    f->wdir = (uint8_t)dir;
    park(w, f);
    return f->wake_res;
}

/* ---- the poll: one epoll_wait for the whole worker ---- */

static void fatal(const char *what)
{
    fprintf(stderr, "em: %s: %s\n", what, strerror(errno));
    abort();
}

/* block = wait for an event or the earliest timer (rounded UP to ms, so a timer
 * never fires early); else just collect what is ready. */
static void ep_poll(em_worker *w, int block)
{
    struct epoll_event evs[EP_EVENTS];
    int timeout = 0;
    if (block) {
        timeout = -1;
        if (w->nheap) {
            int64_t d = w->heap[0].deadline - em_now();
            timeout = d <= 0 ? 0 : d > (int64_t)INT_MAX * 1000000 ? INT_MAX : (int)((d + 999999) / 1000000);
        }
    }
    int n = epoll_wait(w->epfd, evs, EP_EVENTS, timeout);
    if (n < 0) {
        if (errno != EINTR) fatal("epoll_wait");
        n = 0;
    }
    int cancel_root = 0;
    for (int i = 0; i < n; i++) {
        uint32_t fd = evs[i].data.u32, m = evs[i].events;
        if (fd == EV_WAKE) {
            uint64_t v;
            ssize_t r = read(w->evfd, &v, sizeof v); /* drain; EAGAIN = nothing left */
            (void)r;
            cancel_root = 1; /* acted on below, after every event of the batch */
            continue;
        }
        if (fd == EV_JOBS) {
            uint64_t v;
            ssize_t r = read(w->rt->slot[w->id].done_fd, &v, sizeof v); /* counter first, then the list: see ur_reap */
            (void)r;
            em_jobs_reap(w);
            continue;
        }
        if (fd >= w->nfd) continue; /* stale event of an fd em_close already forgot */
        struct em_fdw *s = &w->fdt[fd];
        if (m & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
            s->rd_idle = 0; /* also with no waiter: the next recv must try */
            /* EOF or error pending: a short recv no longer means drained, and no
             * further edge will come to clear the mark (t7: FIN in the same edge
             * as the last bytes). The next recv returns 0 or the error at once. */
            if (m & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) s->kind = EM_FD_OTHER;
            wake_list(w, &s->rd, 0);
        }
        if (m & (EPOLLOUT | EPOLLHUP | EPOLLERR)) wake_list(w, &s->wr, 0);
    }
    if (w->nheap) {
        int64_t now = em_now();
        while (w->nheap && w->heap[0].deadline <= now) {
            em_fiber *f = w->heap[0].f;
            unlink_wait(w, f);
            wake(w, f, -ETIMEDOUT);
        }
    }
    w->budget = EM_POLL_BUDGET;
    if (cancel_root && w->root) em_scope_cancel_all(w, w->root);
}

void ep_reap(em_worker *w, int block)
{
    ep_poll(w, block);
}

/* An op that completed without waiting never went through the scheduler. A fiber
 * that always finds data would starve the others and never let a timer fire, so
 * after EM_POLL_BUDGET such ops the worker yields, and em_yield polls (the budget is spent).
 * io_uring needs this only for em_yield loops: every op goes through the scheduler. */
static void fair(void)
{
    em_worker *w = &em_W;
    if (--w->budget > 0) return;
    em_yield();
}

/* Cancel/deadline check before an op. 0 or the error to hand back. */
static int begin(void)
{
    em_fiber *f = em_W.current;
    if (em_scope_is_cancelled(f)) return -ECANCELED;
    int64_t dl = f->cur_scope->deadline;
    if (dl && em_now() >= dl) return -ETIMEDOUT;
    return 0;
}

/* One I/O op: res = call's result, or -errno, or the wait's error. call must be
 * a nonblocking syscall expression (-1 + errno on failure). */
#define EP_IO(res, fd, dir, call)                                              \
    do {                                                                       \
        for (;;) {                                                             \
            int e_ = begin();                                                  \
            if (e_) { (res) = e_; break; }                                     \
            ssize_t r_ = (call);                                               \
            if (r_ < 0 && errno == EINTR) continue;                            \
            if (r_ >= 0 || errno != EAGAIN) {                                  \
                (res) = r_ >= 0 ? r_ : -errno;                                 \
                fair();                                                        \
                break;                                                         \
            }                                                                  \
            e_ = wait_fd((fd), (dir));                                         \
            if (e_) { (res) = e_; break; }                                     \
        }                                                                      \
    } while (0)

/* ---- ops ---- */

int ep_sleep_until(int64_t deadline_ns)
{
    em_worker *w = &em_W;
    em_fiber *f = w->current;
    int e = begin();
    if (e) return e;
    int64_t dl = f->cur_scope->deadline;
    int64_t target = (dl && dl < deadline_ns) ? dl : deadline_ns;
    if (em_now() < target) {
        if ((e = heap_push(w, f, target))) return e;
        f->wfd = -1;
        park(w, f);
        if (f->wake_res != -ETIMEDOUT) return f->wake_res; /* -ECANCELED */
    } else {
        fair();
    }
    return target == deadline_ns ? 0 : -ETIMEDOUT;
}

/* em_resolve's wait: parked like a sleeper (deadline in the heap, no fd) until
 * ep_job_wake, the deadline or cancel. 0 = woken by the done list. */
int ep_job_wait(void)
{
    em_worker *w = &em_W;
    em_fiber *f = w->current;
    int64_t dl = f->cur_scope->deadline;
    int e;
    if (dl && (e = heap_push(w, f, dl))) return e;
    f->wfd = -1;
    park(w, f);
    return f->wake_res;
}

void ep_job_wake(em_worker *w, em_fiber *f)
{
    wake(w, f, 0);
}

int ep_accept(int lfd)
{
    ssize_t r;
    EP_IO(r, lfd, EM_WAIT_RD, accept4(lfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK));
    if (r >= 0) ep_fd_new(&em_W, (int)r, EM_FD_STREAM);
    return (int)r;
}

/* Forget fd: wake its waiters with -ECANCELED and clear the slot. The kernel
 * drops the epoll registration when the last reference to the file closes.
 * Also called on every fd em creates: a number reused after a plain close()
 * must not inherit the old "registered" slot, or its first wait parks forever. */
void ep_fd_forget(em_worker *w, int fd)
{
    if (fd < 0 || (size_t)fd >= w->nfd) return;
    struct em_fdw *s = &w->fdt[fd];
    wake_list(w, &s->rd, -ECANCELED);
    wake_list(w, &s->wr, -ECANCELED);
    *s = (struct em_fdw){0};
}

/* A fd em just created: clean slot, kind known (no lazy fstat + getsockopt later). */
void ep_fd_new(em_worker *w, int fd, int kind)
{
    ep_fd_forget(w, fd);
    if (!fdt_ensure(w, fd)) w->fdt[fd].kind = (uint8_t)kind; /* ENOMEM: stays unknown, classified lazily */
}

int ep_connect(const struct sockaddr *a, socklen_t n)
{
    em_worker *w = &em_W;
    int fd = socket(a->sa_family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -errno;
    ep_fd_new(w, fd, EM_FD_STREAM);
    int e = begin();
    if (!e && connect(fd, a, n) < 0) {
        if (errno == EINPROGRESS || errno == EINTR) {
            e = wait_fd(fd, EM_WAIT_WR); /* writable = connect finished, well or badly */
            if (!e) {
                int err = 0;
                socklen_t l = sizeof err;
                e = getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l) < 0 ? -errno : -err;
            }
        } else {
            e = -errno;
        }
    }
    if (e < 0) {
        ep_fd_forget(w, fd);
        close(fd);
        return e;
    }
    return fd;
}

/* Classify fd once (cached in kind; fds em creates get it at creation, see ep_fd_new): regular file, stream socket, or other. A stream recv
 * that returns less than asked has drained the socket: the next recv would only see EAGAIN
 * (in echo-like loops, one wasted syscall per round-trip). Remember it and wait first; any
 * data arriving later raises a new edge, which clears rd_idle. Not for datagram sockets (a
 * short recv says nothing about the queue) nor once EOF/error was seen (ep_poll sets
 * EM_FD_OTHER). A regular file is never ready-or-not: em_read sends it to the pool. */
static int fd_kind(em_worker *w, int fd)
{
    struct em_fdw *s = &w->fdt[fd];
    if (!s->kind) {
        struct stat st;
        int t = 0;
        socklen_t l = sizeof t;
        if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) s->kind = EM_FD_FILE;
        else s->kind = getsockopt(fd, SOL_SOCKET, SO_TYPE, &t, &l) == 0 && t == SOCK_STREAM ? EM_FD_STREAM : EM_FD_OTHER;
    }
    return s->kind;
}

static int stream_fd(em_worker *w, int fd)
{
    return fd_kind(w, fd) == EM_FD_STREAM;
}

/* The mark must be set right after the syscall, before EP_IO's fair() may poll or
 * yield: an edge consumed in between would otherwise be lost and the next recv
 * would park forever. errno is only read when r < 0, where nothing is marked. */
static ssize_t recv_mark(em_worker *w, int fd, void *buf, size_t len)
{
    ssize_t r = recv(fd, buf, len, MSG_DONTWAIT);
    if (r > 0 && (size_t)r < len && !fdt_ensure(w, fd) && stream_fd(w, fd)) w->fdt[fd].rd_idle = 1;
    return r;
}

ssize_t ep_recv(int fd, void *buf, size_t len)
{
    em_worker *w = &em_W;
    ssize_t r;
    if ((size_t)fd < w->nfd && w->fdt[fd].rd_idle && w->fdt[fd].registered) {
        int e = begin();
        if (!e) e = wait_fd(fd, EM_WAIT_RD);
        if (e) return e;
    }
    EP_IO(r, fd, EM_WAIT_RD, recv_mark(w, fd, buf, len));
    return r;
}

/* Reads signalfd/eventfd/pipe: the fd must be nonblocking or the first read would stop the
 * whole worker, so set O_NONBLOCK once per fd (shared by dups). A regular file cannot be
 * waited on (always "ready", the read itself blocks on the disk): it is read on the pool,
 * like em_offload, so it is not interruptible once started (the kernel writes into buf). */
struct file_read { int fd; void *buf; size_t len; };

static intptr_t file_read_fn(void *p)
{
    struct file_read *a = p;
    ssize_t r;
    do r = read(a->fd, a->buf, a->len); while (r < 0 && errno == EINTR);
    return r < 0 ? -errno : r;
}

ssize_t ep_read(int fd, void *buf, size_t len)
{
    em_worker *w = &em_W;
    int e = fdt_ensure(w, fd);
    if (e) return e;
    if (fd_kind(w, fd) == EM_FD_FILE) {
        if ((e = begin())) return e;
        struct file_read a = { fd, buf, len };
        intptr_t r;
        e = em_offload(file_read_fn, &a, &r);
        return e ? e : r;
    }
    if (!w->fdt[fd].nb) {
        int fl = fcntl(fd, F_GETFL);
        if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) return -errno;
        w->fdt[fd].nb = 1;
    }
    ssize_t r;
    EP_IO(r, fd, EM_WAIT_RD, read(fd, buf, len));
    return r;
}

ssize_t ep_send(int fd, const void *buf, size_t len)
{
    ssize_t r;
    EP_IO(r, fd, EM_WAIT_WR, send(fd, buf, len, MSG_DONTWAIT | MSG_NOSIGNAL));
    return r;
}

int ep_shutdown(int fd, int how)
{
    int e = begin();
    if (e) return e;
    return shutdown(fd, how) < 0 ? -errno : 0;
}

int ep_close(int fd)
{
    ep_fd_forget(&em_W, fd);
    return close(fd) < 0 ? -errno : 0;
}

ssize_t ep_recvfrom(int fd, void *buf, size_t len, struct sockaddr *src, socklen_t *srclen)
{
    struct iovec iov = { buf, len };
    struct msghdr mh = { .msg_name = src, .msg_namelen = (src && srclen) ? *srclen : 0,
                         .msg_iov = &iov, .msg_iovlen = 1 };
    ssize_t r;
    EP_IO(r, fd, EM_WAIT_RD, recvmsg(fd, &mh, MSG_DONTWAIT));
    if (r >= 0 && src && srclen) *srclen = mh.msg_namelen;
    return r;
}

ssize_t ep_sendto(int fd, const void *buf, size_t len, const struct sockaddr *dst, socklen_t n)
{
    struct iovec iov = { (void *)(uintptr_t)buf, len }; /* iovec is not const-aware; the kernel only reads */
    struct msghdr mh = { .msg_name = (void *)(uintptr_t)dst, .msg_namelen = n,
                         .msg_iov = &iov, .msg_iovlen = 1 };
    ssize_t r;
    EP_IO(r, fd, EM_WAIT_WR, sendmsg(fd, &mh, MSG_DONTWAIT | MSG_NOSIGNAL));
    return r;
}

/* ---- lifecycle ---- */

int ep_init(em_worker *w)
{
    w->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (w->epfd < 0) return -errno;
    w->evfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    struct epoll_event ev = { .events = EPOLLIN | EPOLLET, .data.u32 = EV_WAKE };
    struct epoll_event jev = { .events = EPOLLIN | EPOLLET, .data.u32 = EV_JOBS };
    if (w->evfd < 0 || epoll_ctl(w->epfd, EPOLL_CTL_ADD, w->evfd, &ev) < 0 ||
        epoll_ctl(w->epfd, EPOLL_CTL_ADD, w->rt->slot[w->id].done_fd, &jev) < 0) {
        int e = -errno;
        if (w->evfd >= 0) close(w->evfd);
        close(w->epfd);
        return e;
    }
    w->budget = EM_POLL_BUDGET;
    return 0;
}

void ep_exit(em_worker *w)
{
    close(w->evfd);
    close(w->epfd);
    free(w->fdt);
    free(w->heap);
}
