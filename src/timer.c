#include <errno.h>
#include <stdlib.h>
#include "internal.h"

/* Per-worker timer heap, shared by both backends: a binary min-heap on deadline in w->heap.
 * f->heap_idx makes removal O(log n). epoll parks a fiber with its scope deadline here and
 * polls with the earliest one as timeout; io_uring keeps the deadline of every in-flight op
 * here and arms one kernel timeout for the earliest (io_uring.c). */
static void heap_set(em_worker *w, size_t i, struct em_timer t)
{
    w->heap[i] = t;
    t.f->heap_idx = (int)i;
}

static void heap_up(em_worker *w, size_t i)
{
    struct em_timer t = w->heap[i];
    while (i > 0 && t.deadline < w->heap[(i - 1) / 2].deadline) {
        heap_set(w, i, w->heap[(i - 1) / 2]);
        i = (i - 1) / 2;
    }
    heap_set(w, i, t);
}

static void heap_down(em_worker *w, size_t i)
{
    struct em_timer t = w->heap[i];
    for (;;) {
        size_t c = 2 * i + 1;
        if (c >= w->nheap) break;
        if (c + 1 < w->nheap && w->heap[c + 1].deadline < w->heap[c].deadline) c++;
        if (t.deadline <= w->heap[c].deadline) break;
        heap_set(w, i, w->heap[c]);
        i = c;
    }
    heap_set(w, i, t);
}

int em_heap_reserve(em_worker *w)
{
    if (w->nheap < w->capheap) return 0;
    size_t n = w->capheap ? w->capheap * 2 : 64; /* grows with the number of timer waiters; rare, never steady state */
    struct em_timer *h = realloc(w->heap, n * sizeof *h);
    if (!h) return -ENOMEM;
    w->heap = h;
    w->capheap = n;
    return 0;
}

int em_heap_push(em_worker *w, em_fiber *f, int64_t deadline)
{
    int e = em_heap_reserve(w);
    if (e) return e;
    w->heap[w->nheap] = (struct em_timer){ deadline, f };
    heap_up(w, w->nheap++);
    return 0;
}

void em_heap_remove(em_worker *w, em_fiber *f)
{
    size_t i = (size_t)f->heap_idx;
    f->heap_idx = -1;
    struct em_timer last = w->heap[--w->nheap];
    if (i == w->nheap) return;
    heap_set(w, i, last);
    heap_up(w, i);
    heap_down(w, (size_t)last.f->heap_idx);
}

void em_heap_free(em_worker *w)
{
    free(w->heap);
    w->heap = NULL;
    w->nheap = w->capheap = 0;
}
