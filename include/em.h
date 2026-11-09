#ifndef EM_H
#define EM_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>

typedef struct em_fiber em_fiber;
typedef struct em_scope em_scope;

/* Structured-concurrency scope. Lives on the stack of the fiber that opens it;
 * treat the fields as private. */
struct em_scope {
    em_scope *parent;
    int64_t   deadline;    /* absolute CLOCK_MONOTONIC ns, 0 = none; min with parent */
    int       cancelled;
    int       first_err;   /* first nonzero child result */
    em_fiber *children;    /* intrusive list through em_fiber.sib_* */
    size_t    nchildren;
    em_fiber *closer;      /* fiber waiting in em_scope_close */
    em_fiber *owner;       /* fiber that opened it (LIFO check) */
};

/* Fiber context. Everything below except em_run, em_now, em_cancel_all and the
 * em_worker_* / em_backend queries must be called from a fiber. Called anywhere
 * else (before or after em_run on the calling thread, a thread em did not start,
 * e.g. through a socket kept in a thread-local past em_run) the calls fail
 * instead of touching worker state that no longer exists: I/O, em_sleep_until,
 * em_spawn, em_park, em_offload, em_resolve and em_scope_close return -EPERM; em_close just close()s fd
 * (0 or -errno); em_yield does nothing; em_scope_open leaves an inert scope. */

/* Runs main_fn as the first fiber of EVERY worker (one app copy per core).
 * nworkers = 0: the number of CPUs this process may run on (sched_getaffinity).
 * The calling thread is worker 0; nworkers-1 more threads are created. Worker i
 * is pinned to the i-th allowed CPU; with more workers than allowed CPUs the
 * assignment wraps round-robin. The caller's affinity is restored on return.
 * The whole process is one scope: when any worker's root ends with an error,
 * every other root is cancelled.
 * Returns 0, or the first error in time across workers (a worker whose root
 * was cancelled by em_cancel_all and had no other error reports -ECANCELED;
 * a deadlocked worker reports -EDEADLK, even if its fibers unwound cleanly);
 * -EBUSY if called from inside a worker; -EINVAL if nworkers < 0; -ENOMEM if a
 * first fiber cannot be created. If a ring cannot be created the kernel error
 * is returned and nothing runs, no fallback:
 *   -EPERM   kernel.io_uring_disabled sysctl or seccomp policy,
 *   -ENOSYS  kernel or seccomp has no io_uring,
 *   -EINVAL  kernel < 6.1 (does not know the setup flags).
 * Backend: env EM_BACKEND = auto (default) | uring | epoll, read once here; any
 * other value returns -EINVAL. auto uses io_uring, and epoll only if io_uring
 * is unavailable (setup fails with EPERM/ENOSYS/EACCES/ENOMEM, e.g. the default
 * Docker/k8s seccomp profile). The choice is made once, before any worker
 * starts, is the same for every worker and never changes. uring / epoll force
 * that backend; if it cannot start, the error above is returned. */
int       em_run(int nworkers, int (*main_fn)(void *), void *arg);
/* "uring" or "epoll": the backend of the current run. Valid only inside em_run.
 * Meant for logging. Same API and semantics on both; epoll differences are
 * noted at em_read and em_close, and its timers have 1 ms resolution. */
const char *em_backend(void);
/* Valid only inside em_run (0 / 0 outside). */
int       em_worker_id(void);
int       em_worker_count(void);
/* Cancels the root scope of every worker (graceful shutdown). Callable from any
 * fiber; a no-op outside em_run. */
void      em_cancel_all(void);

/* Valid only inside em_run. 0 or -ENOMEM (process-wide map cap reached / mmap failed).
 * The fiber belongs to scope s until it returns; a nonzero return cancels s. */
int       em_spawn(em_scope *s, int (*fn)(void *), void *arg);
void      em_yield(void);
em_fiber *em_self(void);
/* Parks until em_unpark or cancellation. 0, or -ECANCELED if the scope chain
 * was cancelled while parked. It does not check for a cancel that happened
 * before the call. */
int       em_park(void);
/* PARKED -> READY. Must be called from the worker that owns f. Ignored in any
 * other state (a fiber waiting on the kernel or in em_scope_close is not
 * parkable). */
void      em_unpark(em_fiber *f);

/* deadline_ns: absolute CLOCK_MONOTONIC (see em_now), 0 = none. */
void      em_scope_open(em_scope *s, int64_t deadline_ns);
void      em_scope_cancel(em_scope *s);
/* Waits for all children; returns the first error (0 if clean). Must be called
 * LIFO by the fiber that opened s. */
int       em_scope_close(em_scope *s);

int64_t   em_now(void);
/* 0, -ECANCELED, or -ETIMEDOUT if the scope deadline came first. */
int       em_sleep_until(int64_t deadline_ns);

/* All I/O calls return the result or -errno; -ECANCELED when the scope chain
 * was cancelled, -ETIMEDOUT when the scope deadline passed. */
int       em_tcp_listen(const struct sockaddr *a, socklen_t n, int backlog);
int       em_accept(int lfd);
int       em_connect(const struct sockaddr *a, socklen_t n);
ssize_t   em_recv(int fd, void *buf, size_t len);        /* 0 = EOF */
/* Any fd (signalfd, eventfd, pipe). On epoll the first call on an fd sets
 * O_NONBLOCK on it (the flag lives on the open file description, so dups see
 * it). A regular file is read on the offload pool (as em_offload, so not
 * interruptible once started); on io_uring it is an ordinary read. */
ssize_t   em_read(int fd, void *buf, size_t len);
ssize_t   em_send(int fd, const void *buf, size_t len);  /* may send less */
int       em_send_all(int fd, const void *buf, size_t len);
int       em_shutdown(int fd, int how);
/* UDP socket bound to a. SO_REUSEPORT is set unless the port is 0 (client
 * socket: an ephemeral port must stay unique, or replies would be spread over
 * sockets that share it). */
int       em_udp_bind(const struct sockaddr *a, socklen_t n);
/* src/srclen may be NULL. *srclen is the capacity on entry, the address length
 * on return. A datagram longer than len is truncated. */
ssize_t   em_recvfrom(int fd, void *buf, size_t len, struct sockaddr *src, socklen_t *srclen);
ssize_t   em_sendto(int fd, const void *buf, size_t len, const struct sockaddr *dst, socklen_t n);
/* Runs fn(arg) on a pool thread; the calling fiber parks until fn returns and the
 * worker keeps running other fibers meanwhile. Returns 0 when fn ran (its result is
 * stored in *res; res may be NULL), -EPERM outside a fiber, or -EAGAIN if no pool
 * thread could be created (fn did not run): the result of fn is never mixed with
 * these codes.
 * Not interruptible: fn may use the caller's stack (buffers, arg), so em_offload
 * always waits for fn to finish, even if the scope is cancelled or its deadline
 * passes meanwhile; the cancellation is reported by the next cancel point.
 * fn runs outside em: em calls made from it return -EPERM. The pool is shared by
 * the whole process, starts at em_run and ends with it; threads are created on
 * demand, at most 64, and extra jobs queue. A job that never returns stalls its
 * thread, and em_run, forever. */
int       em_offload(intptr_t (*fn)(void *), void *arg, intptr_t *res);
/* getaddrinfo(host) on the offload pool. Writes up to max (>= 1) addresses (port
 * set to `port`; family AF_INET, AF_INET6 or AF_UNSPEC) into out, in resolver
 * order, and returns the count (>= 1). A numeric host is parsed in place, without
 * the pool. Errors: -ENOENT name does not exist (EAI_NONAME, EAI_NODATA), -EAGAIN
 * temporary resolver failure, -ENOMEM, -errno for EAI_SYSTEM, -EIO any other
 * resolver error, -EINVAL bad argument or host longer than 255 bytes, -EPERM
 * outside a fiber, -ECANCELED / -ETIMEDOUT. Unlike em_offload it returns at once
 * on cancel or deadline: the lookup keeps running on the pool and its result is
 * dropped; em_run waits for such a lookup before it returns (as long as the
 * resolver takes). */
int       em_resolve(const char *host, uint16_t port, int family,
                     struct sockaddr_storage *out, int max);

/* Cancels other fibers' ops on fd (they get -ECANCELED), then closes. Not
 * cancellable itself: always closes fd. fds em created may be closed any way.
 * An fd created outside em and used with em must be closed with this, on the
 * worker that uses it: on epoll a plain close() leaves per-fd state that the
 * next outside fd with the same number would inherit (its first wait would
 * never wake). A dup of fd keeps the file (and its epoll registration) alive
 * after em_close. */
int       em_close(int fd);

#endif
