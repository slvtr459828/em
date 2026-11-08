/* Graceful shutdown in user code: signalfd(SIGTERM) -> em_read -> em_cancel_all. */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/signalfd.h>
#include <unistd.h>
#include "util.h"
#include "internal.h"

static int N;

static int sig_waiter(void *arg)
{
    (void)arg;
    sigset_t m;
    sigemptyset(&m);
    sigaddset(&m, SIGTERM);
    int sfd = signalfd(-1, &m, SFD_CLOEXEC);
    if (sfd < 0) return -errno;
    struct signalfd_siginfo si;
    ssize_t n = em_read(sfd, &si, sizeof si);
    em_close(sfd);
    if (n != (ssize_t)sizeof si || si.ssi_signo != SIGTERM) return -EIO;
    em_cancel_all();
    return 0;
}

static int killer(void *arg)
{
    (void)arg;
    em_sleep_until(em_now() + 20000000); /* the waiter is blocked in em_read by now */
    return kill(getpid(), SIGTERM) == 0 ? 0 : -errno;
}

static int blocker(void *arg)
{
    (void)arg;
    uint16_t port;
    int lfd = listen_local(&port);
    int r = em_accept(lfd);
    em_close(lfd);
    return r;
}

static int main_fn(void *arg)
{
    (void)arg;
    int id = em_worker_id();
    em_scope s;
    em_scope_open(&s, 0);
    assert(em_spawn(&s, blocker, NULL) == 0);
    if (id == 0) assert(em_spawn(&s, sig_waiter, NULL) == 0);
    if (id == N - 1) assert(em_spawn(&s, killer, NULL) == 0);
    return em_scope_close(&s);
}

int main(void)
{
    N = test_workers();
    sigset_t m;
    sigemptyset(&m);
    sigaddset(&m, SIGTERM);
    /* Before em_run creates threads, so every worker inherits the mask and the
     * signal can only be consumed through the signalfd. */
    assert(pthread_sigmask(SIG_BLOCK, &m, NULL) == 0);
    assert(em_run(N, main_fn, NULL) == -ECANCELED);
    assert(em_dbg_mmap_count == em_dbg_munmap_count);
    printf("sigterm: clean exit, %zu mmaps == %zu munmaps\n", (size_t)em_dbg_mmap_count,
           (size_t)em_dbg_munmap_count);
    return 0;
}
