/* One worker's root fails: every other worker, blocked in accept, gets
 * -ECANCELED and em_run returns the failing worker's error. */
#include <stdio.h>
#include "util.h"

#define MAXW 64
static int N, failer;
static int result[MAXW];

static int main_fn(void *arg)
{
    (void)arg;
    int id = em_worker_id();
    uint16_t port;
    int lfd = listen_local(&port);
    int r;
    if (id == failer) {
        em_sleep_until(em_now() + 20000000); /* let the others block in accept */
        r = -EIO;
    } else {
        r = em_accept(lfd);
    }
    em_close(lfd);
    result[id] = r;
    return r;
}

int main(void)
{
    N = test_workers();
    assert(N >= 2 && N <= MAXW);
    failer = N > 2 ? 2 : N - 1;
    assert(em_run(N, main_fn, NULL) == -EIO);
    for (int i = 0; i < N; i++) assert(result[i] == (i == failer ? -EIO : -ECANCELED));
    printf("cross failfast: worker %d -EIO cancelled %d others\n", failer, N - 1);
    return 0;
}
