/*
 * bw2_lift.h - what BW2's lifted shards use beyond recomp_types.h.
 *
 * A function whose prologue installs an MSVC C++ EH frame gets a landing pad
 * (bw2/lift.py add_landing_pad): BW2_EH_ENTER right after RECOMP_ENTER,
 * every return wrapped in BW2_EH_LEAVE, and a switch at _eh_resume over the
 * addresses its catch blocks continue at. A caught guest throw (eh.c) longjmps
 * here with the guest registers already set for the continuation. Registers
 * are globals, so only the lifter's own flag locals need resetting.
 * _setjmp/_longjmp: no signal mask, so entering such a function costs no
 * system call.
 */
#ifndef BW2_LIFT_H
#define BW2_LIFT_H
#include <setjmp.h>
#include <stdint.h>

void bw2_eh_bad_resume(uint32_t func, uint32_t resume) __attribute__((noreturn));

#define BW2_EH_ENTER()                                                        \
    recomp_ehframe_t _ehf;                                                    \
    _ehf.entry_esp = g_esp;                                                   \
    _ehf.func = g_cur_func;                                                   \
    _ehf.resume = 0;                                                          \
    recomp_eh_push(&_ehf);                                                    \
    if (_setjmp(_ehf.jb)) { RECOMP_REGS_IN(); _flag_k = FK_NONE; _cf = 0;     \
                            _df = 1; _fpu_cmp = 0; goto _eh_resume; }
#define BW2_EH_LEAVE() recomp_eh_pop(&_ehf)

#endif
