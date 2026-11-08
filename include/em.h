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

/* Runs main_fn as the first fiber, in the root scope, on the calling thread.
 * Returns the root scope's result (0, or the first nonzero fiber result);
 * -EDEADLK if a deadlock was detected (even if every fiber unwound cleanly);
 * -EBUSY if called from inside a fiber; -ENOMEM if the first fiber cannot be
 * created. If the io_uring ring cannot be created the kernel error is returned
 * and nothing runs, no fallback:
 *   -EPERM   kernel.io_uring_disabled sysctl or seccomp policy,
 *   -ENOSYS  kernel or seccomp has no io_uring,
 *   -EINVAL  kernel < 6.1 (does not know the setup flags). */
int       em_run(int (*main_fn)(void *), void *arg);

/* Valid only inside em_run. 0 or -ENOMEM (map cap reached / mmap failed).
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
ssize_t   em_send(int fd, const void *buf, size_t len);  /* may send less */
int       em_send_all(int fd, const void *buf, size_t len);
int       em_shutdown(int fd, int how);
/* Cancels other fibers' ops on fd (they get -ECANCELED), then closes. Not
 * cancellable itself: always closes fd. */
int       em_close(int fd);

#endif
