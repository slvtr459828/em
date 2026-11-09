#include <assert.h>
#include <errno.h>
#include "internal.h"

static int64_t min_deadline(int64_t a, int64_t b)
{
    if (!a) return b;
    if (!b) return a;
    return a < b ? a : b;
}

void em_scope_open(em_scope *s, int64_t deadline_ns)
{
    em_fiber *f = em_W.current;
    if (!f) { /* outside a fiber: leave an inert scope; em_scope_close reports -EPERM */
        *s = (em_scope){0};
        return;
    }
    s->parent = f->cur_scope;
    s->deadline = min_deadline(deadline_ns, s->parent->deadline);
    s->cancelled = 0;
    s->first_err = 0;
    s->children = NULL;
    s->nchildren = 0;
    s->closer = NULL;
    s->owner = f;
    f->cur_scope = s;
}

/* Cancelled = some scope on the chain from f's innermost scope up is cancelled. */
int em_scope_is_cancelled(const em_fiber *f)
{
    for (const em_scope *s = f->cur_scope; s; s = s->parent)
        if (s->cancelled) return 1;
    return 0;
}

static void interrupt(em_worker *w, em_fiber *f)
{
    switch (f->state) {
    case EM_IOWAIT:
        if (w->epoll) ep_interrupt(w, f); else ur_interrupt(w, f);
        break;
    case EM_PARKED:
        f->wake_res = -ECANCELED;
        em_make_ready(w, f);
        break;
    default: /* READY/RUNNING see the flag at their next op; SCOPEWAIT's children get hit below */
        break;
    }
}

/* Every fiber under s: its children plus, recursively, the children of scopes
 * those fibers have open (chain cur_scope -> home). */
void em_scope_interrupt_all(em_worker *w, em_scope *s)
{
    for (em_fiber *c = s->children; c; c = c->sib_next) {
        interrupt(w, c);
        for (em_scope *n = c->cur_scope; n != c->home; n = n->parent)
            em_scope_interrupt_all(w, n);
    }
}

void em_scope_cancel_all(em_worker *w, em_scope *s)
{
    s->cancelled = 1;
    em_scope_interrupt_all(w, s);
}

void em_scope_cancel(em_scope *s)
{
    em_scope_cancel_all(&em_W, s);
}

void em_scope_unlink(em_fiber *f)
{
    em_scope *s = f->home;
    if (f->sib_prev) f->sib_prev->sib_next = f->sib_next; else s->children = f->sib_next;
    if (f->sib_next) f->sib_next->sib_prev = f->sib_prev;
    if (--s->nchildren == 0 && s->closer) {
        em_make_ready(&em_W, s->closer);
        s->closer = NULL;
    }
}

int em_scope_close(em_scope *s)
{
    em_worker *w = &em_W;
    em_fiber *f = w->current;
    if (!f) return -EPERM;
    assert(s->owner == f && f->cur_scope == s); /* LIFO on the opener */
    if (s->nchildren) {
        s->closer = f;
        f->state = EM_SCOPEWAIT;
        em_switch_to_sched(w, f);
    }
    f->cur_scope = s->parent;
    if (!s->first_err && s->cancelled) return -ECANCELED;
    return s->first_err;
}
