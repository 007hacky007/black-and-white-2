/*
 * guard.c - the tripwire for the user's standing rule (spec 6.2): the
 * protection VM runs as shipped, and if it, or the game, asks for a disc or
 * driver check, the run stops and the user decides how to answer.
 *
 * Always a trip: DeviceIoControl, Netbios, a CreateFile of a device path
 * (\\.\ or \\?\), an MCI command naming cdaudio. Also a trip, whoever asks:
 * GetDriveType, GetVolumeInformation, GetLogicalDrives, GetLogicalDriveStrings,
 * QueryDosDevice. The VM could ask through an ordinary game function, so the
 * caller's address cannot tell a disc check from a harmless question. A call
 * site the user has approved goes on g_allowed (call, calling function);
 * those go to win32hle's answer, and the few it lacks stop by name, as any
 * unanswered import does. "VM: yes" in a trip report means the call came
 * straight from the VM's sections.
 *
 * A trip prints the call, its arguments and where it came from, plus the
 * return addresses on the guest stack, and exits 3. GetProcAddress of any of
 * these names finds these shims too (they are registered first).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "win32hle.h"
#include "bw2.h"

#define MAX_RANGES 4
static struct { uint32_t lo, hi; } g_vm[MAX_RANGES], g_code[8];
static int g_nvm, g_ncode;

static void vm_import_audit(const char *name);

void bw2_guard_init(uint32_t base) {
    uint32_t nt = base + MEM32(base + 0x3C);
    uint32_t nsec = MEM16(nt + 6), sh = nt + 24 + MEM16(nt + 20);
    for (uint32_t i = 0; i < nsec; i++, sh += 40) {
        char name[9] = { 0 };
        memcpy(name, hle_ptr(sh), 8);
        uint32_t lo = base + MEM32(sh + 12), hi = lo + MEM32(sh + 8), flags = MEM32(sh + 36);
        if ((!strcmp(name, "stxt774") || !strcmp(name, ".name0")) && g_nvm < MAX_RANGES)
            g_vm[g_nvm].lo = lo, g_vm[g_nvm++].hi = hi;
        if ((flags & 0x20000020u) && g_ncode < 8) g_code[g_ncode].lo = lo, g_code[g_ncode++].hi = hi;
    }
    hle_import_hook = vm_import_audit;
}

static int in_vm(uint32_t va) {
    for (int i = 0; i < g_nvm; i++) if (va >= g_vm[i].lo && va < g_vm[i].hi) return 1;
    return 0;
}
static int in_code(uint32_t va) {
    for (int i = 0; i < g_ncode; i++) if (va >= g_code[i].lo && va < g_code[i].hi) return 1;
    return 0;
}

static int from_vm(void) { return in_vm(MEM32(g_esp)) || in_vm(g_cur_func); }

/* Drive and volume questions the user has approved, by call and calling
 * function (sub_<va>). Empty: nothing has been approved yet. */
static const struct { const char *call; uint32_t func; } g_allowed[] = {
    { NULL, 0 }
};

static int allowed(const char *call) {
    for (int i = 0; g_allowed[i].call; i++)
        if (!strcmp(g_allowed[i].call, call) && g_allowed[i].func == g_cur_func) return 1;
    return 0;
}

/* Stop for the user: the call, where from, and the guest stack's return addresses. */
static void trip(const char *call, const char *args) {
    uint32_t ret = MEM32(g_esp), stack_hi = MEM32(g_fs_base + 4);
    fprintf(stderr, "TRIPWIRE: %s(%s) from 0x%08X in sub_%08X (VM: %s). "
            "Per the user's rule this needs their decision.\n",
            call, args, ret, g_cur_func, from_vm() ? "yes" : "no");
    fprintf(stderr, "TRIPWIRE: return addresses on the guest stack:");
    int n = 0;
    for (uint32_t a = g_esp; a + 4 <= stack_hi && n < 64; a += 4) {
        uint32_t v = MEM32(a);
        if (in_code(v)) fprintf(stderr, "%s0x%08X%s", n % 8 ? " " : "\n  ", v, in_vm(v) ? "(vm)" : ""), n++;
    }
    fprintf(stderr, "\n");
    hle_exit(BW2_EXIT_TRIPWIRE);
}

static const char *str_or(uint32_t va, const char *none) { return va ? (const char *)hle_ptr(va) : none; }

/* A guest UTF-16 string as ASCII (other characters as '?'). */
static void narrow(uint32_t va, char *out, size_t n) {
    size_t k = 0;
    if (va) for (uint16_t c; k + 1 < n && (c = MEM16(va + 2 * (uint32_t)k)); k++) out[k] = c < 128 ? (char)c : '?';
    out[k] = 0;
}

static int sep(char c) { return c == '\\' || c == '/'; }

/* \\.\x and \\?\x (Win32 device and raw paths) and \??\x (an NT one). */
static int device_path(const char *p) {
    if (!p) return 0;
    if (sep(p[0]) && sep(p[1]) && (p[2] == '.' || p[2] == '?') && sep(p[3])) return 1;
    return sep(p[0]) && p[1] == '?' && p[2] == '?' && sep(p[3]);
}

static void unanswered(const char *name) {
    hle_fatal("unimplemented import %s called from guest 0x%08X (sub_%08X)", name, MEM32(g_esp), g_cur_func);
}

static void k_DeviceIoControl(void) {
    char a[96];
    snprintf(a, sizeof a, "handle 0x%X, code 0x%08X, in %u bytes, out %u bytes", A32(0), A32(1), A32(3), A32(5));
    trip("DeviceIoControl", a);
}
static void k_Netbios(void) {
    char a[64];
    uint32_t ncb = A32(0);
    snprintf(a, sizeof a, "NCB command 0x%02X", ncb ? MEM8(ncb) : 0);
    trip("Netbios", a);
}

static void k_GetDriveTypeA(void) {
    if (!allowed("GetDriveTypeA")) trip("GetDriveTypeA", str_or(A32(0), "NULL"));
    bw2_hle_shim("GetDriveTypeA")();
}
static void k_GetDriveTypeW(void) {
    char p[260];
    narrow(A32(0), p, sizeof p);
    if (!allowed("GetDriveTypeW")) trip("GetDriveTypeW", A32(0) ? p : "NULL");
    RET(!A32(0) || (p[0] | 0x20) == 'c' ? 3u /* DRIVE_FIXED */ : 1u /* DRIVE_NO_ROOT_DIR */, 1);
}
static void k_GetVolumeInformationA(void) {
    if (!allowed("GetVolumeInformationA")) trip("GetVolumeInformationA", str_or(A32(0), "NULL"));
    bw2_hle_shim("GetVolumeInformationA")();
}
static void k_GetVolumeInformationW(void) {
    char p[260];
    narrow(A32(0), p, sizeof p);
    if (!allowed("GetVolumeInformationW")) trip("GetVolumeInformationW", A32(0) ? p : "NULL");
    unanswered("GetVolumeInformationW");
}
static void k_GetLogicalDrives(void) {
    if (!allowed("GetLogicalDrives")) trip("GetLogicalDrives", "");
    bw2_hle_shim("GetLogicalDrives")();
}
static void k_GetLogicalDriveStringsA(void) {
    if (!allowed("GetLogicalDriveStringsA")) trip("GetLogicalDriveStringsA", "");
    bw2_hle_shim("GetLogicalDriveStringsA")();
}
static void k_QueryDosDeviceA(void) {
    if (!allowed("QueryDosDeviceA")) trip("QueryDosDeviceA", str_or(A32(0), "NULL"));
    unanswered("QueryDosDeviceA");
}
static void k_mciSendStringA(void) {
    const char *cmd = str_or(A32(0), "");
    for (const char *p = cmd; *p; p++)
        if (!strncasecmp(p, "cdaudio", 7)) trip("mciSendStringA", cmd);
    bw2_hle_shim("mciSendStringA")();
}
static void k_mciSendCommandA(void) {
    uint32_t msg = A32(1), flags = A32(2), parms = A32(3);
    if (msg == 0x0803u && parms) {                     /* MCI_OPEN: MCI_OPEN_PARMS.lpstrDeviceType at +8 */
        uint32_t type = MEM32(parms + 8);
        if ((flags & 0x1000u) ? (type & 0xFFFFu) == 516u   /* MCI_OPEN_TYPE_ID, MCI_DEVTYPE_CD_AUDIO */
                              : (flags & 0x2000u) && type && !strncasecmp(str_or(type, ""), "cdaudio", 7))
            trip("mciSendCommandA", "MCI_OPEN cdaudio");
    }
    unanswered("mciSendCommandA");
}

static void k_CreateFileA(void) {
    const char *p = str_or(A32(0), NULL);
    if (device_path(p)) trip("CreateFileA", p);
    bw2_hle_shim("CreateFileA")();
}
/* CreateFileW: win32hle answers only the A form, so the name is narrowed into
 * this thread's scratch and CreateFileA answers (same arguments, same purge). */
static void k_CreateFileW(void) {
    char p[1024];
    narrow(A32(0), p, sizeof p);
    if (device_path(p)) trip("CreateFileW", p);
    uint32_t mark = hle_scratch_mark();
    if (A32(0)) MEM32(g_esp + 4) = hle_scratch_copy(p, (uint32_t)strlen(p) + 1);
    bw2_hle_shim("CreateFileA")();
    hle_scratch_release(mark);
}

/* Every import the protection VM's own code calls (its function or return
 * address in stxt774/.name0): the boot shows it calls only GetCurrentThreadId,
 * so anything else, a GetProcAddress or LoadLibrary included, stops for the
 * user (a check made through a game function is caught by the calls above). */
static const char *const g_vm_imports[] = { "GetCurrentThreadId", NULL };

static void vm_import_audit(const char *name) {
    if (!from_vm()) return;
    for (int i = 0; g_vm_imports[i]; i++)
        if (!strcmp(g_vm_imports[i], name)) return;
    trip(name, "called by the protection VM");
}

const win32hle_shim bw2_guard_shims[] = {
    { "DeviceIoControl",         k_DeviceIoControl },
    { "Netbios",                 k_Netbios },
    { "GetDriveTypeA",           k_GetDriveTypeA },
    { "GetDriveTypeW",           k_GetDriveTypeW },
    { "GetVolumeInformationA",   k_GetVolumeInformationA },
    { "GetVolumeInformationW",   k_GetVolumeInformationW },
    { "GetLogicalDrives",        k_GetLogicalDrives },
    { "GetLogicalDriveStringsA", k_GetLogicalDriveStringsA },
    { "QueryDosDeviceA",         k_QueryDosDeviceA },
    { "mciSendStringA",          k_mciSendStringA },
    { "mciSendCommandA",         k_mciSendCommandA },
    { "CreateFileA",             k_CreateFileA },
    { "CreateFileW",             k_CreateFileW },
    { NULL, NULL }
};
