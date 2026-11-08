/* A cancel arriving while the target worker is finishing / has finished its
 * root must not crash, hang or leak (rings stay open until all workers are
 * done). 1000 em_run calls; workers end at random times while worker 0 calls
 * em_cancel_all at a random time (or not at all). */
#include <dirent.h>
#include <stdio.h>
#include <unistd.h>
#include "util.h"
#include "internal.h"

static int N;
static uint64_t run_seed;
static int cancels; /* this run: does worker 0 call em_cancel_all? */

static int count_fds(void)
{
    int n = 0;
    DIR *d = opendir("/proc/self/fd");
    assert(d);
    while (readdir(d)) n++;
    closedir(d);
    return n;
}

static int main_fn(void *arg)
{
    (void)arg;
    int id = em_worker_id();
    uint64_t s = run_seed * 0x9E3779B97F4A7C15ull + (uint64_t)id * 0xD1B54A32D192ED03ull + 1;
    xs_next(&s);
    int64_t delay = (int64_t)(xs_next(&s) % 400) * 1000; /* 0..400 us, some are 0 */
    if (delay > 100000 && delay % 3 == 0 && !(id == 0 && cancels)) return 0; /* some finish at once */
    int r = delay ? em_sleep_until(em_now() + delay) : 0;
    if (id == 0 && cancels) em_cancel_all();
    if (r && r != -ECANCELED) fprintf(stderr, "worker %d: sleep %lld ns -> %d\n", id, (long long)delay, r);
    return r;
}

int main(void)
{
    N = test_workers();
    int fds0 = count_fds();
    int cancelled = 0;
    for (int run = 0; run < 1000; run++) {
        run_seed = (uint64_t)run + 1;
        cancels = run % 4 != 3;
        int r = em_run(N, main_fn, NULL);
        if (cancels ? r != -ECANCELED : r != 0 && r != -ECANCELED) { /* nobody fails */
            fprintf(stderr, "run %d (cancels=%d): em_run = %d\n", run, cancels, r);
            return 1;
        }
        cancelled += r == -ECANCELED;
    }
    assert(em_dbg_mmap_count == em_dbg_munmap_count);
    assert(count_fds() == fds0);
    printf("cancel/exit race: 1000 runs ok (%d cancelled), mmaps == munmaps, no fd leak\n", cancelled);
    return 0;
}
