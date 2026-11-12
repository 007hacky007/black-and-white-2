/*
 * shell.c - the user's folders and name, as the game asks shell32, shlwapi and
 * advapi32 for them. The user is "Player"; their folders are under
 * C:\Documents and Settings\Player\ (which is <support>/C/... on the Mac).
 */
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "win32hle.h"
#include "bw2.h"

#define CSIDL_PERSONAL        0x05
#define CSIDL_APPDATA         0x1A
#define CSIDL_LOCAL_APPDATA   0x1C
#define CSIDL_COMMON_APPDATA  0x23
#define E_INVALIDARG          0x80070057u
#define ERROR_INSUFFICIENT_BUFFER 122u
#define ERROR_ALREADY_EXISTS  183u

static const char *folder(uint32_t csidl) {
    switch (csidl & 0xFF) {
    case CSIDL_PERSONAL:       return "C:\\Documents and Settings\\Player\\My Documents";
    case CSIDL_APPDATA:        return "C:\\Documents and Settings\\Player\\Application Data";
    case CSIDL_LOCAL_APPDATA:  return "C:\\Documents and Settings\\Player\\Local Settings\\Application Data";
    case CSIDL_COMMON_APPDATA: return "C:\\Documents and Settings\\All Users\\Application Data";
    default:                   return NULL;
    }
}

/* mkdir -p for a guest folder; 1 if it already existed. It resolves as a
 * write does, so a folder in the install folder goes to the support folder's
 * copy of it, never into the game tree. */
static int make_guest_dirs(const char *guest) {
    char host[PATH_MAX];
    int existed = hle_host_path(guest, host, sizeof host);
    hle_host_path_for_write(guest, host, sizeof host, 0);
    if (bw2_in_game_tree(host)) hle_fatal("SHCreateDirectoryExA: %s would be made in the game tree", guest);
    for (char *s = host + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(host, 0755); *s = '/'; }
    mkdir(host, 0755);
    hle_path_changed();
    return existed;
}

/* SHGetFolderPathA(hwnd, csidl, token, flags, path): S_OK, the folder made. */
static void k_SHGetFolderPathA(void) {
    const char *f = folder(A32(1));
    if (!f) { fprintf(stderr, "[bw2] SHGetFolderPathA: CSIDL 0x%X not answered\n", A32(1)); RET(E_INVALIDARG, 5); }
    make_guest_dirs(f);
    strcpy(ASTR(4), f);
    RET(0, 5);
}

/* SHCreateDirectoryExA(hwnd, path, sa): ERROR_SUCCESS or ERROR_ALREADY_EXISTS. */
static void k_SHCreateDirectoryExA(void) {
    RET(make_guest_dirs(ASTR(1)) ? ERROR_ALREADY_EXISTS : 0u, 3);
}

/* GetUserNameA/W(buf, &size): size counts the terminator, in and out. */
static void user_name(int wide) {
    static const char name[] = "Player";
    uint32_t *size = (uint32_t *)APTR(1), need = sizeof name;
    if (!size || *size < need) { if (size) *size = need; hle_set_last_error(ERROR_INSUFFICIENT_BUFFER); RET(0, 2); }
    if (wide) { uint16_t *w = (uint16_t *)APTR(0); for (uint32_t i = 0; i < need; i++) w[i] = (uint8_t)name[i]; }
    else memcpy(ASTR(0), name, need);
    *size = need;
    RET(1, 2);
}
static void k_GetUserNameA(void) { user_name(0); }
static void k_GetUserNameW(void) { user_name(1); }

/* PathGetCharTypeW(ch): shlwapi's GCT_* class of a path character. Control characters and " < > | are
 * GCT_INVALID; * ? are GCT_WILD; \ / : are GCT_SEPARATOR; space and ; , + = [ ] are valid in long names
 * only (GCT_LFNCHAR); every other character, the non-ASCII range included, is valid in both long and
 * short names (GCT_LFNCHAR | GCT_SHORTCHAR). The WCHAR is the low 16 bits of its argument slot. The
 * profile dialog checks each character of the name with it. */
#define GCT_INVALID   0u
#define GCT_LFNCHAR   1u
#define GCT_SHORTCHAR 2u
#define GCT_WILD      4u
#define GCT_SEPARATOR 8u
static void k_PathGetCharTypeW(void) {
    uint32_t ch = A32(0) & 0xFFFFu, r;
    if (ch < 0x20u || ch == '"' || ch == '<' || ch == '>' || ch == '|') r = GCT_INVALID;
    else if (ch == '*' || ch == '?') r = GCT_WILD;
    else if (ch == '\\' || ch == '/' || ch == ':') r = GCT_SEPARATOR;
    else if (ch < 0x80u && strchr(" ;,+=[]", (int)ch)) r = GCT_LFNCHAR;
    else r = GCT_LFNCHAR | GCT_SHORTCHAR;
    RET(r, 1);
}

const win32hle_shim bw2_shell_shims[] = {
    { "PathGetCharTypeW",     k_PathGetCharTypeW },
    { "SHGetFolderPathA",     k_SHGetFolderPathA },
    { "SHCreateDirectoryExA", k_SHCreateDirectoryExA },
    { "GetUserNameA",         k_GetUserNameA },
    { "GetUserNameW",         k_GetUserNameW },
    { NULL, NULL }
};
