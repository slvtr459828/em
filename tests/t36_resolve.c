/* em_resolve: localhost, numeric hosts without the pool, a name that does not exist. */
#include <dirent.h>
#include <stdio.h>
#include <unistd.h>
#include "util.h"

static int nthreads(void)
{
    DIR *d = opendir("/proc/self/task");
    assert(d);
    int n = 0;
    for (struct dirent *e; (e = readdir(d));)
        if (e->d_name[0] != '.') n++;
    closedir(d);
    return n;
}

static int main_fn(void *arg)
{
    (void)arg;
    struct sockaddr_storage out[8];

    /* numeric: the pool is not touched (the only thread is this worker) */
    int n = em_resolve("127.0.0.1", 8080, AF_UNSPEC, out, 8);
    assert(n == 1 && out[0].ss_family == AF_INET);
    struct sockaddr_in *a4 = (struct sockaddr_in *)&out[0];
    assert(a4->sin_addr.s_addr == htonl(INADDR_LOOPBACK) && a4->sin_port == htons(8080));
    n = em_resolve("::1", 9, AF_INET6, out, 8);
    assert(n == 1 && out[0].ss_family == AF_INET6);
    assert(((struct sockaddr_in6 *)&out[0])->sin6_port == htons(9));
    assert(nthreads() == 1);

    /* localhost goes through the pool */
    n = em_resolve("localhost", 8080, AF_UNSPEC, out, 8);
    assert(n >= 1 && n <= 8);
    for (int i = 0; i < n; i++) {
        if (out[i].ss_family == AF_INET) {
            struct sockaddr_in *a = (struct sockaddr_in *)&out[i];
            assert(a->sin_addr.s_addr == htonl(INADDR_LOOPBACK) && a->sin_port == htons(8080));
        } else {
            assert(out[i].ss_family == AF_INET6);
            struct sockaddr_in6 *a = (struct sockaddr_in6 *)&out[i];
            assert(memcmp(&a->sin6_addr, &in6addr_loopback, 16) == 0 && a->sin6_port == htons(8080));
        }
    }
    n = em_resolve("localhost", 80, AF_INET, out, 8);
    assert(n >= 1);
    for (int i = 0; i < n; i++) assert(out[i].ss_family == AF_INET);
    assert(em_resolve("localhost", 80, AF_UNSPEC, out, 1) == 1); /* max truncates */

    /* a name that cannot exist (RFC 6761). A sandbox with no resolver says "try again". */
    int r = em_resolve("nonexistent.invalid", 80, AF_UNSPEC, out, 8);
    assert(r == -ENOENT || r == -EAGAIN);

    assert(em_resolve("localhost", 80, 12345, out, 8) == -EINVAL);
    assert(em_resolve("localhost", 80, AF_UNSPEC, out, 0) == -EINVAL);
    char longname[300];
    memset(longname, 'a', sizeof longname - 1);
    longname[sizeof longname - 1] = 0;
    assert(em_resolve(longname, 80, AF_UNSPEC, out, 8) == -EINVAL);

    /* deadline already passed: no lookup */
    em_scope s;
    em_scope_open(&s, em_now() - 1);
    assert(em_resolve("localhost", 80, AF_UNSPEC, out, 8) == -ETIMEDOUT);
    em_scope_close(&s);

    printf("resolve (%s): numeric, localhost, ENOENT (%d), arguments\n", em_backend(), r);
    return 0;
}

int main(void)
{
    struct sockaddr_storage out[1];
    assert(em_resolve("localhost", 80, AF_UNSPEC, out, 1) == -EPERM);
    assert(em_run(1, main_fn, NULL) == 0);
    return 0;
}
