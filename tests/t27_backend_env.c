/* EM_BACKEND: invalid value -> -EINVAL (nothing runs); valid values run on the
 * backend they name. */
#include <stdio.h>
#include "util.h"

static int ran;
static const char *name;
static int main_fn(void *arg) { (void)arg; ran = 1; name = em_backend(); return 0; }

static void run(const char *env, int rc, const char *want)
{
    ran = 0;
    name = NULL;
    if (env) setenv("EM_BACKEND", env, 1); else unsetenv("EM_BACKEND");
    assert(em_run(1, main_fn, NULL) == rc);
    assert(ran == (rc == 0));
    if (want) assert(name && strcmp(name, want) == 0);
}

int main(void)
{
    run("bogus", -EINVAL, NULL);
    run("", -EINVAL, NULL);
    run("EPOLL", -EINVAL, NULL);
    run("epoll", 0, "epoll");
    run("auto", 0, NULL); /* either, depending on the machine; must work */
    if (strcmp(name, "uring") == 0) run("uring", 0, "uring"); /* else io_uring is blocked here (Docker): auto chose epoll */
    run(NULL, 0, NULL);   /* unset = auto */
    printf("backend env: invalid -> -EINVAL, uring/epoll/auto run\n");
    return 0;
}
