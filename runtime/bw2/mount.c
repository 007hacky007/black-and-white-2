/*
 * mount.c - what the game sees of the disk (spec 5.2, plan 3 Global Constraints):
 *
 *   C:\Program Files\EA GAMES\Black & White 2\   the game tree, read-only;
 *                                                its writes land in <support>/Game/
 *   C:\ (everything else)                        <support>/C/
 *
 * Nothing else on the Mac is reachable: no Z: (win32hle's host root), and a
 * path that resolves outside these two folders does not exist for the game.
 *
 * The game tree is never written: later it lives inside the signed app bundle.
 * win32hle's overlay sends writes through CreateFile and CreateDirectory to
 * <support>/Game; the calls that would change a game file in place (delete,
 * move away, remove a folder, set attributes) fail with ERROR_ACCESS_DENIED.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "win32hle.h"
#include "bw2.h"

#define ERROR_ACCESS_DENIED   5u
#define ERROR_ALREADY_EXISTS  183u

static char g_game[PATH_MAX];       /* realpath of the game tree */

static void mkdirs(const char *path) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    mkdir(p, 0755);
}

int bw2_mount(const char *game_dir, const char *support_dir) {
    if (!realpath(game_dir, g_game)) { fprintf(stderr, "bw2: no game folder %s\n", game_dir); return 1; }
    char c[PATH_MAX], ov[PATH_MAX];
    snprintf(c, sizeof c, "%s/C", support_dir);
    snprintf(ov, sizeof ov, "%s/Game", support_dir);
    mkdirs(c);
    mkdirs(ov);
    char support_real[PATH_MAX];
    if (!realpath(support_dir, support_real)) snprintf(support_real, sizeof support_real, "%s", support_dir);
    const char *roots[] = { g_game, support_real, NULL };   /* nothing else on the Mac */
    hle_set_roots(roots);
    hle_set_drive('C', c);
    hle_set_mount(BW2_INSTALL_DIR, g_game);
    hle_set_overlay(g_game, ov);
    if (chdir(g_game) != 0) { fprintf(stderr, "bw2: cannot enter %s\n", g_game); return 1; }
    char reg[PATH_MAX];
    snprintf(reg, sizeof reg, "%s/registry.txt", support_dir);
    hle_reg_load(strdup(reg));
    return 0;
}

/* host is the game's own file (not the support folder's copy of it). */
int bw2_in_game_tree(const char *host) {
    char abs[PATH_MAX];
    if (!realpath(host, abs)) {
        if (host[0] == '/') snprintf(abs, sizeof abs, "%s", host);
        else { char cwd[PATH_MAX]; snprintf(abs, sizeof abs, "%s/%s", getcwd(cwd, sizeof cwd) ? cwd : "", host); }
    }
    size_t l = strlen(g_game);
    return l && !strncmp(abs, g_game, l) && (abs[l] == '/' || !abs[l]);
}

static void deny_or_forward(const char *name, int nargs) {
    char host[PATH_MAX];
    hle_host_path(ASTR(0), host, sizeof host);
    if (bw2_in_game_tree(host)) {
        hle_set_last_error(ERROR_ACCESS_DENIED);
        RET(0, nargs);
    }
    bw2_hle_shim(name)();
}
static void k_DeleteFileA(void)        { deny_or_forward("DeleteFileA", 1); }
static void k_RemoveDirectoryA(void)   { deny_or_forward("RemoveDirectoryA", 1); }
static void k_SetFileAttributesA(void) { deny_or_forward("SetFileAttributesA", 2); }

/* MoveFileA(from, to): the source may not be a game file; the target goes
 * where a write to it would. */
static void k_MoveFileA(void) {
    char from[PATH_MAX], to[PATH_MAX];
    if (!hle_host_path(ASTR(0), from, sizeof from)) { hle_set_last_error(2); RET(0, 2); }
    if (bw2_in_game_tree(from)) { hle_set_last_error(ERROR_ACCESS_DENIED); RET(0, 2); }
    if (hle_host_path(ASTR(1), to, sizeof to)) { hle_set_last_error(ERROR_ALREADY_EXISTS); RET(0, 2); }
    hle_host_path_for_write(ASTR(1), to, sizeof to, 0);
    if (bw2_in_game_tree(to)) { hle_set_last_error(ERROR_ACCESS_DENIED); RET(0, 2); }
    if (rename(from, to) != 0) { hle_set_last_error(errno == ENOENT ? 3u : ERROR_ACCESS_DENIED); RET(0, 2); }
    hle_path_changed();
    RET(1, 2);
}

const win32hle_shim bw2_mount_shims[] = {
    { "DeleteFileA",        k_DeleteFileA },
    { "RemoveDirectoryA",   k_RemoveDirectoryA },
    { "SetFileAttributesA", k_SetFileAttributesA },
    { "MoveFileA",          k_MoveFileA },
    { NULL, NULL }
};
