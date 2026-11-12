/*
 * shims_user.c - user32 imports win32hle does not answer, as BW2 calls them
 * (each one checked in shims_selftest.c first).
 */
#include <string.h>
#include "win32hle.h"
#include "bw2.h"

#define SPI_GETMOUSE           0x0003u
#define SPI_SETMOUSE           0x0004u
#define SPI_GETSCREENSAVEACTIVE 0x0010u
#define SPI_SETSCREENSAVEACTIVE 0x0011u
#define SPI_GETFILTERKEYS      0x0032u
#define SPI_SETFILTERKEYS      0x0033u
#define SPI_GETTOGGLEKEYS      0x0034u
#define SPI_SETTOGGLEKEYS      0x0035u
#define SPI_GETSTICKYKEYS      0x003Au
#define SPI_SETSTICKYKEYS      0x003Bu
#define SPI_GETMOUSESPEED      0x0070u
#define SPI_SETMOUSESPEED      0x0071u

/* SystemParametersInfoA(action, uiParam, pvParam, fWinIni). The game reads the
 * mouse and accessibility-key settings at start and puts them back on exit:
 * reads answer Windows' defaults, writes succeed and change nothing on the Mac.
 * Any other action stops the run, naming it. */
static void k_SystemParametersInfoA(void) {
    uint32_t action = A32(0), pv = A32(2);
    switch (action) {
    case SPI_GETMOUSESPEED:
        if (pv) MEM32(pv) = 10;
        RET(1, 4);
    case SPI_GETMOUSE:                                     /* thresholds 6, 10; acceleration on */
        if (pv) MEM32(pv) = 6, MEM32(pv + 4) = 10, MEM32(pv + 8) = 1;
        RET(1, 4);
    case SPI_GETSCREENSAVEACTIVE:
        if (pv) MEM32(pv) = 0;
        RET(1, 4);
    case SPI_GETSTICKYKEYS: case SPI_GETTOGGLEKEYS: case SPI_GETFILTERKEYS:
        if (pv) {                                          /* {cbSize, dwFlags, ...}: all off */
            uint32_t size = MEM32(pv);
            if (size >= 8 && size <= 64) memset(hle_ptr(pv + 4), 0, size - 4);
        }
        RET(1, 4);
    case SPI_SETMOUSESPEED: case SPI_SETMOUSE: case SPI_SETSCREENSAVEACTIVE:
    case SPI_SETSTICKYKEYS: case SPI_SETTOGGLEKEYS: case SPI_SETFILTERKEYS:
        RET(1, 4);
    default:
        hle_fatal("SystemParametersInfoA action 0x%04X not answered (from guest 0x%08X)", action, MEM32(g_esp));
    }
}

static void k_GetDoubleClickTime(void) { RET(500, 0); }                 /* Windows' default, ms */

/* One keyboard layout: US English (0x0409 language, 0x0409 device). */
#define US_LAYOUT 0x04090409u
static void k_GetKeyboardLayout(void) { RET(US_LAYOUT, 1); }
static void k_GetKeyboardLayoutList(void) {             /* (n, list): with n 0, how many */
    if (A32(0) && A32(1)) MEM32(A32(1)) = US_LAYOUT;
    RET(1, 2);
}

/* _TrackMouseEvent(tme) (comctl32): the request for WM_MOUSELEAVE/WM_MOUSEHOVER
 * is accepted; the input layer (a later plan) sends them. TRUE. */
static void k_TrackMouseEvent(void) { RET(1, 1); }

/* LoadImageA(hinst, name, type, cx, cy, flags). A bitmap from a file that is
 * not there is NULL with ERROR_FILE_NOT_FOUND, as on Windows: the game tries
 * three paths for a bitmap the shipped tree does not have, and takes its own
 * error path on NULL. (win32hle answers its placeholder bitmap there, which
 * other titles rely on.) A file that is there is win32hle's to read, and its
 * placeholder for one it cannot read stops the run. Every other call is
 * win32hle's. */
#define IMAGE_BITMAP          0u
#define LR_LOADFROMFILE       0x0010u
#define ERROR_FILE_NOT_FOUND  2u
#define HLE_PLACEHOLDER_IMAGE 0x1C000003u
static void k_LoadImageA(void) {
    if (A32(2) != IMAGE_BITMAP || !(A32(5) & LR_LOADFROMFILE)) { bw2_hle_shim("LoadImageA")(); return; }
    const char *path = ASTR(1);
    char host[1024];
    if (!path || !hle_host_path(path, host, sizeof host)) {
        hle_set_last_error(ERROR_FILE_NOT_FOUND);
        RET(0, 6);
    }
    uint32_t from = MEM32(g_esp);
    bw2_hle_shim("LoadImageA")();
    if (g_eax == HLE_PLACEHOLDER_IMAGE)
        hle_fatal("LoadImageA: \"%s\" is not a bitmap win32hle reads (from guest 0x%08X)", path, from);
}

const win32hle_shim bw2_user_shims[] = {
    { "LoadImageA",            k_LoadImageA },
    { "_TrackMouseEvent",      k_TrackMouseEvent },
    { "SystemParametersInfoA", k_SystemParametersInfoA },
    { "GetDoubleClickTime",    k_GetDoubleClickTime },
    { "GetKeyboardLayout",     k_GetKeyboardLayout },
    { "GetKeyboardLayoutList", k_GetKeyboardLayoutList },
    { NULL, NULL }
};
