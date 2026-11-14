/* What the installer does that is particular to Ring.
 *
 * The game comes on four CDs.  All four are copied into one folder,
 * <install>\game\cd (the game's D: drive), so no disc is ever asked for:
 * each disc's data\cd.ini says which disc it is, and after the merge the
 * folder keeps disc 1's ("1"); the port answers the game's disc check for
 * every zone from there (src/rt/ring_game.c).
 * <install>\game\hd is C:\RING, what the game's own installer (install.exe)
 * put on the hard disk: the files its English list install\eng.lis names
 * (code 1: copy as is, code 17: language file into data\), and fl.ini with
 * the CD path filled in. */
#include "setup.h"
#include <string.h>
#include <ctype.h>
#include "install.h"

int game_is_disc(CdSrc *c, int n)
{
    static const char *const files[GAME_DISCS][4] = GAME_DISC_FILES;
    if (!c || n < 0 || n >= GAME_DISCS) return 0;
    for (int i = 0; files[n][i]; i++)
        if (!cd_exists(c, files[n][i])) return 0;
    return 1;
}

/* *, ? against a name, ignoring case */
static int wild(const char *p, const char *s)
{
    if (!*p) return !*s;
    if (*p == '*') return wild(p + 1, s) || (*s && wild(p, s + 1));
    if (*s && (*p == '?' || tolower((u8)*p) == tolower((u8)*s))) return wild(p + 1, s + 1);
    return 0;
}

static int copy_matching(const char *srcdir, const char *mask, const char *dstdir)
{
    char pat[MAX_PATH * 2];
    snprintf(pat, sizeof pat, "%s\\*", srcdir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!wild(mask, fd.cFileName)) continue;
        char s[MAX_PATH * 3], d[MAX_PATH * 3];
        snprintf(s, sizeof s, "%s\\%s", srcdir, fd.cFileName);
        snprintf(d, sizeof d, "%s\\%s", dstdir, fd.cFileName);
        make_dirs(dstdir);
        if (!CopyFileA(s, d, FALSE)) { setup_log("ERROR: cannot copy %s", s); FindClose(h); return -1; }
        SetFileAttributesA(d, FILE_ATTRIBUTE_NORMAL);   /* files from a CD are read-only */
        n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

int game_install_files(const char *cd, const char *hd)
{
    char p[MAX_PATH * 2];
    /* disc 1's cd.ini: this folder is "disc 1" (and holds all four) */
    snprintf(p, sizeof p, "%s\\data\\cd.ini", cd);
    SetFileAttributesA(p, FILE_ATTRIBUTE_NORMAL);
    write_file(p, "1\r\n", 3);

    snprintf(p, sizeof p, "%s\\install\\eng.lis", cd);
    size_t n;
    char *lis = (char *)read_file(p, &n);
    if (!lis) { MessageBoxA(NULL, "install\\eng.lis is missing on disc 1.", GAME_TITLE " setup", MB_ICONERROR); return 0; }
    int files = 0;
    char *save = NULL;
    int first = 1;
    for (char *line = strtok_r(lis, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        if (first) { first = 0; continue; }                 /* the count */
        char *code = strrchr(line, ' ');
        if (!code) code = strrchr(line, '\t');
        if (!code) continue;
        *code++ = 0;
        int c = atoi(code);
        if (c != 1 && c != 17) continue;                     /* 18: installer helpers, 19: uninstall list */
        char pat[MAX_PATH], *o = pat;
        for (const char *s = line; *s && o < pat + sizeof pat - 1; s++) {   /* "install\\x" -> "install\x" */
            if (*s == ' ' || *s == '\t') continue;
            if (*s == '\\' && s[1] == '\\') continue;
            *o++ = *s;
        }
        *o = 0;
        char *slash = strrchr(pat, '\\');
        char dir[MAX_PATH] = "", mask[MAX_PATH];
        if (slash) { *slash = 0; snprintf(dir, sizeof dir, "%s", pat); snprintf(mask, sizeof mask, "%s", slash + 1); }
        else snprintf(mask, sizeof mask, "%s", pat);
        char src[MAX_PATH * 2], dst[MAX_PATH * 2];
        snprintf(src, sizeof src, dir[0] ? "%s\\%s" : "%s", cd, dir);
        if (c == 17) snprintf(dst, sizeof dst, "%s\\data", hd);
        else snprintf(dst, sizeof dst, dir[0] ? "%s\\%s" : "%s", hd, dir);
        int k = copy_matching(src, mask, dst);
        if (k < 0) { free(lis); return 0; }
        files += k;
    }
    free(lis);
    setup_log("%d game files set up", files);

    /* fl.ini: the CD is drive D: */
    snprintf(p, sizeof p, "%s\\fl.ini", hd);
    char *fl = (char *)read_file(p, &n);
    if (!fl) { MessageBoxA(NULL, "fl.ini was not copied.", GAME_TITLE " setup", MB_ICONERROR); return 0; }
    char *cdrom = strstr(fl, "CDROM");
    char *colon = strstr(fl, "CDPATH:");
    if (cdrom && colon && cdrom > colon) {
        char out[4096];
        int head = (int)(colon - fl);
        char *eol = strpbrk(cdrom, "\r\n");
        snprintf(out, sizeof out, "%.*sCDPATH:\tD:\\%s", head, fl, eol ? eol : "");
        write_file(p, out, strlen(out));
    }
    free(fl);
    snprintf(p, sizeof p, "%s\\data\\save", hd);
    make_dirs(p);
    return files > 0;
}

/* the exe's icon: ring.ico on disc 1 */
const char *game_icon_exe(void) { return NULL; }
const char *game_icon_lbm(void) { return GAME_ICON_LBM; }
const char *game_icon_ico(void) { return GAME_ICON_ICO; }
