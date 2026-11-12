/*
 * imm32.c - the IMM32 calls win32hle does not answer, as Windows answers them
 * with no IME on the US layout (plan 6 Ruling 5): a context can be made,
 * associated and destroyed; it is never open, and the layout has no
 * description and no properties. The game makes one when its name field
 * takes the keyboard ("Measured" item 7). In bw2_input_shims (input.c).
 */
#include <stdlib.h>
#include "win32hle.h"
#include "bw2.h"

#define HIMC_BASE 0x494D0000u            /* 'IM': made-up handles, nonzero */

static uint32_t *g_live;                 /* the contexts made and not destroyed */
static size_t g_nlive, g_clive;
static uint32_t g_next = 1;

/* ImmCreateContext(void) */
void bw2_ImmCreateContext(void) {
    if (g_nlive == g_clive) {
        g_clive = g_clive ? g_clive * 2 : 8;
        g_live = realloc(g_live, g_clive * sizeof *g_live);
        if (!g_live) abort();
    }
    uint32_t h = HIMC_BASE + g_next++;
    g_live[g_nlive++] = h;
    RET(h, 0);
}
/* ImmDestroyContext(himc): TRUE for a live context, FALSE otherwise */
void bw2_ImmDestroyContext(void) {
    uint32_t h = A32(0);
    for (size_t i = 0; i < g_nlive; i++)
        if (g_live[i] == h) {
            g_live[i] = g_live[--g_nlive];
            RET(1, 1);
        }
    RET(0, 1);
}
/* ImmGetOpenStatus(himc): never open */
void bw2_ImmGetOpenStatus(void) { RET(0, 1); }
/* ImmGetDescriptionA(hkl, buf, n): the layout is no IME; writes nothing */
void bw2_ImmGetDescriptionA(void) { RET(0, 3); }
/* ImmGetProperty(hkl, index): no property for any index */
void bw2_ImmGetProperty(void) { RET(0, 2); }
/* ImmSetCompositionStringA(himc, index, comp, compLen, read, readLen): no composition */
void bw2_ImmSetCompositionStringA(void) { RET(0, 6); }
/* ImmSetCompositionWindow(himc, &COMPOSITIONFORM): accepted */
void bw2_ImmSetCompositionWindow(void) { RET(1, 2); }
