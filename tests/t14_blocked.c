#include <errno.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <linux/audit.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__x86_64__)
#define AUDIT_ARCH_EM AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define AUDIT_ARCH_EM AUDIT_ARCH_AARCH64
#endif

#include <stdlib.h>
#include <string.h>
#include "util.h"

static int ran;
static int main_fn(void *arg) { (void)arg; ran = 1; return 0; }

static int client(void *arg)
{
    uint16_t port = (uint16_t)(uintptr_t)arg;
    int fd = connect_local(port);
    char b[4];
    if (fd < 0 || em_send_all(fd, "ping", 4) != 0 || em_recv(fd, b, 4) != 4 || memcmp(b, "pong", 4) != 0)
        return -EIO;
    return em_close(fd);
}

/* echo over loopback: listen/accept/connect/recv/send on whatever backend runs */
static int echo_fn(void *arg)
{
    (void)arg;
    assert(strcmp(em_backend(), "epoll") == 0);
    uint16_t port;
    int lfd = listen_local(&port);
    em_scope s;
    em_scope_open(&s, 0);
    assert(em_spawn(&s, client, (void *)(uintptr_t)port) == 0);
    int fd = em_accept(lfd);
    char b[4];
    assert(fd >= 0 && em_recv(fd, b, 4) == 4 && memcmp(b, "ping", 4) == 0);
    assert(em_send_all(fd, "pong", 4) == 0);
    em_close(fd);
    em_close(lfd);
    assert(em_scope_close(&s) == 0);
    ran = 1;
    return 0;
}

static void install_filter(void)
{
    struct sock_filter f[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_EM, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_io_uring_setup, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog p = { sizeof f / sizeof f[0], f };
    assert(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0);
    assert(prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &p) == 0);
}

/* Runs in a child with io_uring_setup blocked (the default Docker/k8s seccomp
 * profile does the same). 0 = child exited 0. */
static int run_blocked(const char *backend, int (*fn)(void *), int expect_rc)
{
    fflush(NULL);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        install_filter();
        setenv("EM_BACKEND", backend, 1);
        int rc = em_run(1, fn, NULL);
        _exit(rc == expect_rc && (expect_rc == 0) == ran ? 0 : 1);
    }
    int st;
    assert(waitpid(pid, &st, 0) == pid);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

int main(void)
{
    assert(run_blocked("uring", main_fn, -EPERM)); /* forced io_uring: error, no fallback */
    printf("blocked: EM_BACKEND=uring -> -EPERM under seccomp\n");
    assert(run_blocked("auto", echo_fn, 0));       /* auto: falls back to epoll, echo works */
    printf("blocked: EM_BACKEND=auto -> epoll, echo ok\n");
    return 0;
}
