/*
 * shims_net.c - BW2's ws2_32 (plan 4 Task 10): no network stack. Online play
 * is a non-goal and the game must not open host sockets, so WSAStartup fails
 * with WSASYSNOTREADY, as on a machine without networking; the game checks it
 * (0x8645C4) and carries on with its network flag off. WS2_32 is imported by
 * ordinal; only what the boot reaches is answered here, the rest stays loud.
 */
#include "win32hle.h"
#include "bw2.h"

#define WSASYSNOTREADY     10091u
#define WSANOTINITIALISED  10093u

static void n_WSAStartup(void) { RET(WSASYSNOTREADY, 2); }            /* (version, WSADATA*) */
static void n_WSACleanup(void) { RET(0xFFFFFFFFu, 0); }               /* SOCKET_ERROR: never started */
static void n_WSAGetLastError(void) { RET(WSANOTINITIALISED, 0); }

const win32hle_shim bw2_net_shims[] = {
    { "ws2_32.dll#115", n_WSAStartup },
    { "ws2_32.dll#116", n_WSACleanup },
    { "ws2_32.dll#111", n_WSAGetLastError },
    { NULL, NULL }
};
