#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../include/em.h"

static int recurse(volatile char *p, int depth);
static int (*volatile rec)(volatile char *, int) = recurse;

static int recurse(volatile char *p, int depth)
{
    volatile char buf[512];
    buf[0] = (char)depth;
    return rec(buf, depth + 1) + buf[0] + (p ? p[0] : 0);
}

static int bad(void *arg) { (void)arg; return recurse(NULL, 0); }
static int main_fn(void *arg)
{
    (void)arg;
    em_scope s;
    em_scope_open(&s, 0);
    assert(em_spawn(&s, bad, NULL) == 0);
    return em_scope_close(&s);
}

int main(void)
{
    fflush(NULL);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        setrlimit(RLIMIT_CORE, &(struct rlimit){0, 0}); /* no core file (qemu writes one into cwd) */
        freopen("/dev/null", "w", stderr); /* mute the expected ASan report */
        em_run(main_fn, NULL);
        _exit(0); /* reached only if overflow went undetected */
    }
    int st;
    assert(waitpid(pid, &st, 0) == pid);
#ifdef __SANITIZE_ADDRESS__
    /* ASan's SEGV handler reports the overflow and exits non-zero */
    assert(WIFEXITED(st) && WEXITSTATUS(st) != 0);
#else
    assert(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV);
#endif
    printf("guard: child died on guard page as expected\n");
    return 0;
}
