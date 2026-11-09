#define _GNU_SOURCE
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "internal.h"

/* Offload pool (D6): em_offload and em_resolve run blocking code on a process-wide
 * set of threads, created on demand up to EM_POOL_MAX, mutex + condvar queue (not the
 * network hot path, rule 3 does not apply).
 *
 * A job belongs to the worker that submitted it. The pool thread only runs fn, stores
 * res, pushes the job on the owner's done list and writes the owner's eventfd; every
 * state change of the job and of its waiter happens on the owner (em_jobs_reap), so
 * nothing races on job state. The backends watch the eventfd (io_uring: an armed READ,
 * epoll: a registered fd) and call em_jobs_reap.
 *
 * em_offload: the job lives on the fiber's stack and the fiber is in EM_JOBWAIT, which
 * cancel ignores: the call waits for fn, always.
 * em_resolve: heap job, interruptible wait; on cancel/deadline the job becomes
 * ABANDONED and em_jobs_reap frees it when it comes back. */

/* ---- pool ---------------------------------------------------------------- */

/* Pool thread: pop, run, push on the owner's list, repeat; exits when stopped and drained. */
static void *pool_thread(void *p)
{
    struct em_pool *pool = p;
    pthread_mutex_lock(&pool->mu);
    for (;;) {
        while (!pool->head && !pool->stop) pthread_cond_wait(&pool->cv, &pool->mu);
        em_job *j = pool->head;
        if (!j) break; /* stop, nothing left */
        if (!(pool->head = j->next)) pool->tail = NULL;
        pool->queued--;
        pool->spare--;
        pthread_mutex_unlock(&pool->mu);

        j->res = j->fn(j->arg);
        /* After the push the owner may free j: only `home` is used from here on. */
        struct em_slot *home = j->home;
        em_job *old = atomic_load(&home->done);
        do j->next = old; while (!atomic_compare_exchange_weak(&home->done, &old, j));
        uint64_t one = 1;
        /* EAGAIN = counter saturated = the owner has a wake-up pending anyway */
        if (write(home->done_fd, &one, sizeof one) < 0 && errno != EAGAIN) abort();

        pthread_mutex_lock(&pool->mu);
        pool->spare++;
    }
    pthread_mutex_unlock(&pool->mu);
    return NULL;
}

int em_pool_init(struct em_pool *p, size_t masksz, const void *mask)
{
    memset(p, 0, sizeof *p);
    int e = pthread_mutex_init(&p->mu, NULL);
    if (e) return -e;
    if ((e = pthread_cond_init(&p->cv, NULL))) goto cv;
    if ((e = pthread_attr_init(&p->attr))) goto attr;
    if ((e = pthread_attr_setaffinity_np(&p->attr, masksz, mask))) {
        pthread_attr_destroy(&p->attr);
        goto attr;
    }
    return 0;
attr:
    pthread_cond_destroy(&p->cv);
cv:
    pthread_mutex_destroy(&p->mu);
    return -e;
}

void em_pool_join(struct em_pool *p)
{
    pthread_mutex_lock(&p->mu);
    p->stop = 1;
    pthread_cond_broadcast(&p->cv);
    int n = p->nthreads;
    pthread_mutex_unlock(&p->mu);
    for (int i = 0; i < n; i++) pthread_join(p->th[i], NULL);
}

void em_pool_destroy(struct em_pool *p)
{
    pthread_attr_destroy(&p->attr);
    pthread_cond_destroy(&p->cv);
    pthread_mutex_destroy(&p->mu);
}

/* Queue j; start a thread if every existing one is busy and the cap allows.
 * 0, or -EAGAIN if no thread exists and none can be created (j was not queued). */
static int pool_submit(struct em_pool *p, em_job *j)
{
    int e = 0;
    j->next = NULL;
    pthread_mutex_lock(&p->mu);
    if (p->queued >= p->spare && p->nthreads < EM_POOL_MAX) {
        if (pthread_create(&p->th[p->nthreads], &p->attr, pool_thread, p) == 0) {
            p->nthreads++;
            p->spare++;
        } else if (p->nthreads == 0) {
            e = -EAGAIN;
        } /* else: an existing thread will get to it */
    }
    if (!e) {
        if (p->tail) p->tail->next = j; else p->head = j;
        p->tail = j;
        p->queued++;
        pthread_cond_signal(&p->cv);
    }
    pthread_mutex_unlock(&p->mu);
    return e;
}

/* ---- owner side ---------------------------------------------------------- */

/* Hand j to the pool on behalf of the current fiber's worker. */
static int job_submit(em_job *j)
{
    em_worker *w = &em_W;
    j->home = &w->rt->slot[w->id];
    j->f = w->current;
    j->state = EM_JOB_WAIT;
    int e = pool_submit(&w->rt->pool, j);
    if (!e) w->jobs++;
    return e;
}

/* Take everything the pool has finished. A WAIT job becomes DONE and its waiter, if it is
 * still waiting, is woken; an ABANDONED job is freed. A waiter that is not waiting any more
 * (EM_READY: woken by cancel/deadline, not yet run) simply finds the job DONE when it runs. */
void em_jobs_reap(em_worker *w)
{
    em_job *j = atomic_exchange(&w->rt->slot[w->id].done, NULL);
    while (j) {
        em_job *nx = j->next; /* j may be freed or reused below */
        w->jobs--;
        if (j->state == EM_JOB_ABANDONED) {
            j->release(j);
        } else {
            assert(j->state == EM_JOB_WAIT);
            j->state = EM_JOB_DONE;
            em_fiber *f = j->f;
            if (f->state == EM_JOBWAIT) {          /* em_offload */
                w->inflight--;
                em_make_ready(w, f);
            } else if (f->state == EM_IOWAIT) {    /* em_resolve */
                if (w->epoll) ep_job_wake(w, f); else ur_job_wake(w, f);
            }
        }
        j = nx;
    }
}

intptr_t em_offload(intptr_t (*fn)(void *), void *arg)
{
    em_worker *w = &em_W;
    em_fiber *f = w->current;
    if (!f) return -EPERM;
    em_job j = { .fn = fn, .arg = arg, .res = 0 };
    int e = job_submit(&j);
    if (e) return e;
    w->inflight++; /* counts as pending work for the deadlock check */
    f->state = EM_JOBWAIT;
    em_switch_to_sched(w, f);
    assert(j.state == EM_JOB_DONE); /* j is on this frame: never leave before the pool is done with it */
    return j.res;
}

/* Interruptible wait for a heap job. 0 = DONE (caller reads j->res and releases it); else
 * the error, and j is ABANDONED: it frees itself when the pool returns it. */
static int heap_job_wait(em_job *j)
{
    em_worker *w = &em_W;
    em_fiber *f = w->current;
    int e = job_submit(j);
    if (e) {
        j->release(j);
        return e;
    }
    int64_t dl = f->cur_scope->deadline;
    int r = w->epoll ? ep_job_wait() : ur_job_wait(dl);
    if (j->state == EM_JOB_DONE) return 0; /* finished first (even if a cancel or the deadline raced it) */
    j->state = EM_JOB_ABANDONED;
    return r == -ETIME ? -ETIMEDOUT : r;
}

/* Cancel/deadline check before starting: 0 or the error to hand back. */
static int scope_check(em_fiber *f)
{
    if (em_scope_is_cancelled(f)) return -ECANCELED;
    int64_t dl = f->cur_scope->deadline;
    if (dl && em_now() >= dl) return -ETIMEDOUT;
    return 0;
}

/* ---- em_resolve ---------------------------------------------------------- */

struct rjob {
    em_job j;                 /* first: arg == &j */
    struct addrinfo *ai;      /* set by the pool thread */
    int      err;             /* errno of an EAI_SYSTEM failure */
    int      family;
    char     host[256];
};

static void rjob_release(em_job *j)
{
    struct rjob *r = (struct rjob *)j;
    if (r->ai) freeaddrinfo(r->ai);
    free(r);
}

static intptr_t resolve_fn(void *arg)
{
    struct rjob *r = arg;
    struct addrinfo hints = { .ai_family = r->family, .ai_socktype = SOCK_STREAM };
    int e = getaddrinfo(r->host, NULL, &hints, &r->ai);
    if (e == EAI_SYSTEM) r->err = errno;
    return e;
}

static int gai_to_errno(int e, int err)
{
    switch (e) {
    case EAI_NONAME:
#ifdef EAI_NODATA
    case EAI_NODATA:
#endif
        return -ENOENT;
    case EAI_AGAIN:  return -EAGAIN;
    case EAI_MEMORY: return -ENOMEM;
    case EAI_SYSTEM: return err ? -err : -EIO;
    default:         return -EIO;
    }
}

/* Copy up to max AF_INET/AF_INET6 results of ai into out; returns the count. */
static int fill(const struct addrinfo *ai, uint16_t port, struct sockaddr_storage *out, int max)
{
    int n = 0;
    for (; ai && n < max; ai = ai->ai_next) {
        if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6) continue;
        memset(&out[n], 0, sizeof out[n]);
        memcpy(&out[n], ai->ai_addr, ai->ai_addrlen);
        if (ai->ai_family == AF_INET) ((struct sockaddr_in *)&out[n])->sin_port = htons(port);
        else ((struct sockaddr_in6 *)&out[n])->sin6_port = htons(port);
        n++;
    }
    return n;
}

int em_resolve(const char *host, uint16_t port, int family, struct sockaddr_storage *out, int max)
{
    em_fiber *f = em_W.current;
    if (!f) return -EPERM;
    if (!host || !out || max < 1 || strlen(host) >= sizeof((struct rjob *)0)->host ||
        (family != AF_UNSPEC && family != AF_INET && family != AF_INET6))
        return -EINVAL;
    int e = scope_check(f);
    if (e) return e;

    /* A numeric host needs no lookup: parse it here, never touching the pool. */
    struct addrinfo hints = { .ai_family = family, .ai_socktype = SOCK_STREAM, .ai_flags = AI_NUMERICHOST };
    struct addrinfo *ai;
    if (getaddrinfo(host, NULL, &hints, &ai) == 0) {
        int n = fill(ai, port, out, max);
        freeaddrinfo(ai);
        if (n) return n;
    }

    struct rjob *r = calloc(1, sizeof *r);
    if (!r) return -ENOMEM;
    r->j.fn = resolve_fn;
    r->j.arg = r;
    r->j.release = rjob_release;
    r->family = family;
    strcpy(r->host, host);
    if ((e = heap_job_wait(&r->j))) return e; /* r is the pool's (or already freed) */
    int n = (int)r->j.res;
    if (n) {
        n = gai_to_errno(n, r->err);
    } else if (!(n = fill(r->ai, port, out, max))) {
        n = -ENOENT;
    }
    rjob_release(&r->j);
    return n;
}

/* ---- test hook ----------------------------------------------------------- */

struct djob {
    em_job j;
};

static void djob_release(em_job *j) { free(j); }

int em_dbg_heap_job(intptr_t (*fn)(void *), void *arg)
{
    if (!em_W.current) return -EPERM;
    int e = scope_check(em_W.current);
    if (e) return e;
    struct djob *d = calloc(1, sizeof *d);
    if (!d) return -ENOMEM;
    d->j.fn = fn;
    d->j.arg = arg;
    d->j.release = djob_release;
    if ((e = heap_job_wait(&d->j))) return e;
    int r = (int)d->j.res;
    djob_release(&d->j);
    return r;
}
