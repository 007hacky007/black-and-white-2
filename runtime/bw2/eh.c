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

/* ---- the dispatch ------------------------------------------------------------
 *
 * A guest `throw` reaches BW2's own _CxxThrowException (static CRT, white.exe
 * 0xAD9B13), which calls RaiseException(0xE06D7363, ..., {magic, object,
 * ThrowInfo}). We answer that import and dispatch the throw from the guest's
 * own data, as __CxxFrameHandler would (eh32.c's scheme):
 *
 *   1. walk fs:[0]'s chain for a C++ frame whose try block covers its state
 *      and has a catch matching a type in the ThrowInfo (by decorated name);
 *   2. run the unwind funclets of every frame above it, and its own down to
 *      the try block;
 *   3. store the object in the catch's variable and run the catch funclet as
 *      guest code with ebp = the frame's; it returns the continuation;
 *   4. destroy the exception object (ThrowInfo.pmfnUnwind), as MSVC's runtime
 *      does after a catch completes, then longjmp into the lifted body that
 *      owns the frame (bw2_lift.h), with esp as its prologue saved it.
 *
 * Unlike eh32: a throw nothing catches stops the run naming its type; a catch
 * above an SEH (__try) frame stops too, since that frame's __finally/__except
 * would not run; any other RaiseException code stops, naming it.
 */
#include <string.h>

#define EH_MAGIC_VC6   0x19930520u
#define EH_MAGIC_VC8   0x19930522u
#define CXX_EXCEPTION  0xE06D7363u
#define MAX_IMAGES     16

int bw2_eh_trace;
static _Thread_local uint32_t t_cur_obj, t_cur_info;   /* the exception a running catch holds */
static struct { uint32_t lo, hi; } g_images[MAX_IMAGES];
static int g_nimages;

void bw2_eh_add_image(uint32_t base, uint32_t span) {
    if (g_nimages == MAX_IMAGES) hle_fatal("more than %d images for EH", MAX_IMAGES);
    g_images[g_nimages].lo = base, g_images[g_nimages++].hi = base + span;
}

/* [va, va+n) inside a mapped image: where EH tables and handler stubs live. */
static int in_image(uint32_t va, uint32_t n) {
    for (int i = 0; i < g_nimages; i++)
        if (va >= g_images[i].lo && va + n <= g_images[i].hi && va + n >= va) return 1;
    return 0;
}

/* The FuncInfo a C++ frame's handler stub (mov eax, FuncInfo; jmp) names, or 0. */
static uint32_t funcinfo(uint32_t handler) {
    if (!in_image(handler, 11)) return 0;
    const uint8_t *h = (const uint8_t *)hle_ptr(handler);
    if (h[0] != 0xB8 || (h[5] != 0xE9 && !(h[5] == 0xFF && h[6] == 0x25))) return 0;
    uint32_t fi = MEM32(handler + 1);
    if (!in_image(fi, 20)) return 0;
    uint32_t magic = MEM32(fi);
    return magic >= EH_MAGIC_VC6 && magic <= EH_MAGIC_VC8 ? fi : 0;
}

/* Guest code at va, as MSVC's _CallSettingFrame runs it: ebp = frame_ebp, on the
 * current guest stack. Returns eax; every other register is kept. */
static uint32_t call_funclet(uint32_t va, uint32_t frame_ebp) {
    recomp_func_t fn = recomp_lookup(va);
    if (!fn) hle_fatal("[eh] funclet 0x%08X is not lifted (add it to the catalog)", va);
    uint32_t esp = g_esp, ebp = g_ebp, ebx = g_ebx, esi = g_esi, edi = g_edi, cur = g_cur_func;
    g_ebp = frame_ebp;
    g_esp -= 4;
    MEM32(g_esp) = 0;                   /* the funclet's ret pops it */
    fn();
    uint32_t eax = g_eax;
    g_esp = esp, g_ebp = ebp, g_ebx = ebx, g_esi = esi, g_edi = edi, g_cur_func = cur;
    return eax;
}

/* A thiscall with one stack argument (a copy constructor) or none (a destructor). */
static void call_this(uint32_t va, uint32_t this_, int with_arg, uint32_t arg) {
    uint32_t ecx = g_ecx, esp = g_esp;
    g_ecx = this_;
    if (with_arg) { g_esp -= 4; MEM32(g_esp) = arg; }
    call_funclet(va, g_ebp);
    g_ecx = ecx, g_esp = esp;
}

/* Destroy a C++ frame's locals from its current state down to `to`. */
static void unwind_to(uint32_t rec, uint32_t fi, int to) {
    int s = (int)MEM32(rec + 8);
    uint32_t map = MEM32(fi + 8);
    int max = (int)MEM32(fi + 4);
    while (s != to && s >= 0 && s < max) {
        int next = (int)MEM32(map + 8 * (uint32_t)s);
        uint32_t action = MEM32(map + 8 * (uint32_t)s + 4);
        MEM32(rec + 8) = (uint32_t)next;
        if (action) call_funclet(action, rec + 12);
        s = next;
    }
}

static const char *td_name(uint32_t td) { return td ? (const char *)hle_ptr(td + 8) : "?"; }

/* The ThrowInfo's CatchableType that a catch of `type` takes, or 0. */
static uint32_t catchable(uint32_t info, uint32_t type) {
    uint32_t arr = MEM32(info + 12);
    int n = (int)MEM32(arr);
    for (int i = 0; i < n; i++) {
        uint32_t ct = MEM32(arr + 4 + 4 * (uint32_t)i), td = MEM32(ct + 4);
        if (td == type || !strcmp(td_name(td), td_name(type))) return ct;
    }
    return 0;
}

static const char *throw_type(uint32_t info) {
    uint32_t arr = info ? MEM32(info + 12) : 0;
    return arr && MEM32(arr) ? td_name(MEM32(MEM32(arr + 4) + 4)) : "?";
}

/* Store the thrown object into the catch's variable. */
static void bind_catch_object(uint32_t handler, uint32_t ct, uint32_t obj, uint32_t frame_ebp) {
    uint32_t adj = MEM32(handler), disp = MEM32(handler + 8);
    if (!MEM32(handler + 4) || !disp || !ct) return;       /* catch(...) or no variable */
    uint32_t dst = frame_ebp + disp, src = obj + MEM32(ct + 8);
    uint32_t size = MEM32(ct + 20), copy = MEM32(ct + 24);
    if (adj & 8) MEM32(dst) = src;                          /* by reference */
    else if ((MEM32(ct) & 1) || !copy) memcpy(hle_ptr(dst), hle_ptr(src), size);
    else call_this(copy, dst, 1, src);                      /* copy constructor */
}

static void dispatch(uint32_t obj, uint32_t info, uint32_t site) {
    uint32_t top = MEM32(g_fs_base), seh = 0;
    int guard = 0;
    for (uint32_t rec = top; rec && rec != 0xFFFFFFFFu; rec = MEM32(rec)) {
        if (++guard > 100000) hle_fatal("[eh] the fs:[0] chain does not end (at 0x%08X)", rec);
        uint32_t fi = funcinfo(MEM32(rec + 4));
        if (!fi) { if (!seh) seh = rec; continue; }
        int state = (int)MEM32(rec + 8), ntry = (int)MEM32(fi + 12);
        uint32_t tries = MEM32(fi + 16);
        for (int t = 0; t < ntry; t++) {
            uint32_t tb = tries + 20 * (uint32_t)t;
            int lo = (int)MEM32(tb), hi = (int)MEM32(tb + 4), ncatch = (int)MEM32(tb + 12);
            if (state < lo || state > hi) continue;
            for (int c = 0; c < ncatch; c++) {
                uint32_t h = MEM32(tb + 16) + 16 * (uint32_t)c, type = MEM32(h + 4);
                uint32_t ct = type ? catchable(info, type) : 0;
                if (type && !ct) continue;
                uint32_t ebp = rec + 12;
                if (seh)
                    hle_fatal("[eh] %s from 0x%08X would be caught by the frame at 0x%08X above an SEH frame "
                              "(0x%08X, handler 0x%08X), whose __finally/__except would not run",
                              throw_type(info), site, ebp, seh, MEM32(seh + 4));
                if (bw2_eh_trace)
                    fprintf(stderr, "[eh] %s caught by the frame at 0x%08X (catch funclet 0x%08X)\n",
                            throw_type(info), ebp, MEM32(h + 12));
                for (uint32_t r = top; r != rec; r = MEM32(r)) {
                    uint32_t f = funcinfo(MEM32(r + 4));
                    if (f) unwind_to(r, f, -1);
                }
                unwind_to(rec, fi, lo);
                MEM32(g_fs_base) = rec;
                MEM32(rec + 8) = (uint32_t)(hi + 1);
                bind_catch_object(h, ct, obj, ebp);
                uint32_t prev_obj = t_cur_obj, prev_info = t_cur_info;
                t_cur_obj = obj, t_cur_info = info;
                uint32_t resume = call_funclet(MEM32(h + 12), ebp);
                t_cur_obj = prev_obj, t_cur_info = prev_info;
                uint32_t dtor = MEM32(info + 4);             /* the catch is over: the object goes */
                if (dtor) call_this(dtor, obj, 0, 0);
                for (recomp_ehframe_t *f = t_frames; f; f = f->prev) {
                    if (f->entry_esp != ebp + 4) continue;
                    t_frames = f;
                    g_ebp = ebp;
                    g_esp = MEM32(ebp - 0x10);
                    g_eax = resume;
                    f->resume = resume;
                    g_cur_func = f->func;
                    _longjmp(f->jb, 1);
                }
                hle_fatal("[eh] the frame at 0x%08X caught %s, but no lifted body with a landing pad owns it "
                          "(continuation 0x%08X)", ebp, throw_type(info), resume);
            }
        }
    }
}

/* RaiseException(code, flags, nargs, args), stdcall. */
static void k_RaiseException(void) {
    uint32_t code = A32(0), flags = A32(1), n = A32(2), args = A32(3), site = MEM32(g_esp);
    if (code == CXX_EXCEPTION && n >= 3 && args && MEM32(args) >= EH_MAGIC_VC6 && MEM32(args) <= EH_MAGIC_VC8) {
        uint32_t obj = MEM32(args + 4), info = MEM32(args + 8);
        if (!info) {                                        /* `throw;` */
            if (!t_cur_info) hle_fatal("[eh] rethrow from 0x%08X with no exception active", site);
            obj = t_cur_obj, info = t_cur_info;
        }
        if (bw2_eh_trace)
            fprintf(stderr, "[eh] throw %s 0x%08X from 0x%08X (sub_%08X)\n", throw_type(info), obj, site, g_cur_func);
        dispatch(obj, info, site);
        hle_fatal("uncaught C++ exception %s thrown from 0x%08X (in sub_%08X)", throw_type(info), site, g_cur_func);
    }
    hle_fatal("RaiseException(0x%08X, flags 0x%X, %u args) from 0x%08X in sub_%08X: only C++ exceptions "
              "are dispatched", code, flags, n, site, g_cur_func);
}

const win32hle_shim bw2_eh_shims[] = {
    { "RaiseException", k_RaiseException },
    { NULL, NULL }
};
