/*
 * shims_adv.c - BW2's own advapi32 answers (plan 4 Task 10). Privileges are
 * answered as for a standard user: the names look up to Windows' own LUIDs,
 * the process token opens, and AdjustTokenPrivileges succeeds without
 * granting anything but SeChangeNotifyPrivilege (ERROR_NOT_ALL_ASSIGNED), so
 * the game's SeLockMemoryPrivilege (large pages) request falls back as on a
 * normal account.
 */
#include <string.h>
#include <strings.h>
#include "win32hle.h"
#include "bw2.h"

#define ERROR_NOT_ALL_ASSIGNED 1300u
#define ERROR_NO_SUCH_PRIVILEGE 1313u
#define ERROR_INVALID_HANDLE 6u
#define SE_CHANGE_NOTIFY 23u
#define SE_PRIVILEGE_ENABLED 2u

static const char *const g_privs[] = {           /* LUID = index + 2 */
    "SeCreateTokenPrivilege", "SeAssignPrimaryTokenPrivilege", "SeLockMemoryPrivilege", "SeIncreaseQuotaPrivilege",
    "SeMachineAccountPrivilege", "SeTcbPrivilege", "SeSecurityPrivilege", "SeTakeOwnershipPrivilege",
    "SeLoadDriverPrivilege", "SeSystemProfilePrivilege", "SeSystemtimePrivilege", "SeProfileSingleProcessPrivilege",
    "SeIncreaseBasePriorityPrivilege", "SeCreatePagefilePrivilege", "SeCreatePermanentPrivilege", "SeBackupPrivilege",
    "SeRestorePrivilege", "SeShutdownPrivilege", "SeDebugPrivilege", "SeAuditPrivilege",
    "SeSystemEnvironmentPrivilege", "SeChangeNotifyPrivilege", "SeRemoteShutdownPrivilege", "SeUndockPrivilege", NULL };

static int g_token;                              /* the token handles' object */

static void a_LookupPrivilegeValueA(void) {      /* (system, name, LUID*) */
    const char *name = APTR(1);
    if (!name || !A32(2) || A32(0)) { hle_set_last_error(ERROR_NO_SUCH_PRIVILEGE); RET(0, 3); }
    for (int i = 0; g_privs[i]; i++)
        if (!strcasecmp(name, g_privs[i])) {
            MEM32(A32(2)) = (uint32_t)i + 2, MEM32(A32(2) + 4) = 0;
            hle_set_last_error(0);
            RET(1, 3);
        }
    hle_set_last_error(ERROR_NO_SUCH_PRIVILEGE);
    RET(0, 3);
}
static void a_OpenProcessToken(void) {           /* (process, access, HANDLE*) */
    if (!A32(2)) { hle_set_last_error(ERROR_INVALID_HANDLE); RET(0, 3); }
    uint32_t h = hle_handle_alloc(HLE_H_OTHER, &g_token);
    if (!h) hle_fatal("no handle for a process token");
    MEM32(A32(2)) = h;
    hle_set_last_error(0);
    RET(1, 3);
}
/* (token, disable all, TOKEN_PRIVILEGES* new, buffer length, TOKEN_PRIVILEGES* previous, DWORD* length) */
static void a_AdjustTokenPrivileges(void) {
    if (hle_handle_obj(A32(0), HLE_H_OTHER) != &g_token) { hle_set_last_error(ERROR_INVALID_HANDLE); RET(0, 6); }
    uint32_t tp = A32(2), missing = 0;
    if (A32(4)) MEM32(A32(4)) = 0;               /* nothing was changed, so nothing to restore */
    if (A32(5)) MEM32(A32(5)) = 4;
    if (!A32(1) && tp) {
        uint32_t n = MEM32(tp);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t luid = MEM32(tp + 4 + 12 * i), attr = MEM32(tp + 12 + 12 * i);
            if ((attr & SE_PRIVILEGE_ENABLED) && luid != SE_CHANGE_NOTIFY) missing = 1;
        }
    }
    hle_set_last_error(missing ? ERROR_NOT_ALL_ASSIGNED : 0);
    RET(1, 6);
}

const win32hle_shim bw2_adv_shims[] = {
    { "LookupPrivilegeValueA", a_LookupPrivilegeValueA },
    { "OpenProcessToken",      a_OpenProcessToken },
    { "AdjustTokenPrivileges", a_AdjustTokenPrivileges },
    { NULL, NULL }
};
