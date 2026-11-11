/*
 * eh.c - guest C++ exceptions: the host frames that can take a caught throw.
 *
 * Derived from pcrecomp's runtime/native32/eh32.c (MIT, Copyright (c) 2026
 * sp00nz), ported to based memory and POSIX threads. Each lifted function with
 * an EH landing pad (bw2_lift.h) pushes a frame on entry and pops it on return;
 * the dispatch (plan 3 Task 7) longjmps into the frame that owns the catching
 * guest frame. A longjmp past frames leaves them on the list; the next pop
 * below them drops them.
 */
#include <setjmp.h>
#include <stdio.h>
#include "recomp_types.h"
#include "win32hle.h"
#include "bw2.h"

static _Thread_local recomp_ehframe_t *t_frames;     /* newest first */

void recomp_eh_push(recomp_ehframe_t *f) { f->prev = t_frames; t_frames = f; }

void recomp_eh_pop(recomp_ehframe_t *f) {
    for (recomp_ehframe_t *p = t_frames; p; p = p->prev)
        if (p == f) { t_frames = f->prev; return; }
}

/* The newest frame entered above guest stack address esp. */
recomp_ehframe_t *recomp_eh_owner(uint32_t esp) {
    for (recomp_ehframe_t *f = t_frames; f; f = f->prev)
        if (f->entry_esp > esp) return f;
    return NULL;
}

void recomp_eh_resume(recomp_ehframe_t *f, uint32_t resume) {
    t_frames = f;
    f->resume = resume;
    g_cur_func = f->func;
    _longjmp(f->jb, 1);
}

recomp_ehframe_t *bw2_eh_frames(void) { return t_frames; }

int bw2_eh_depth(void) {
    int n = 0;
    for (recomp_ehframe_t *f = t_frames; f; f = f->prev) n++;
    return n;
}

void bw2_eh_bad_resume(uint32_t func, uint32_t resume) {
    hle_fatal("sub_%08X: a caught exception resumed at 0x%08X, which is not one of its catch continuations",
              func, resume);
    for (;;) {}
}
