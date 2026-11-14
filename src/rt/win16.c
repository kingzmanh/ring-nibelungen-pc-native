/* Win16-style file I/O (OpenFile, _lread, _hread, _llseek...), multimedia
 * file I/O (mmio, with its buffer in guest memory: the game reads the
 * buffer directly through MMIOINFO.pchNext / pchEndRead), GetTickCount and
 * SHFileOperation - what ring.exe uses besides the Win32 file calls. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

void path_to_host(const char *p, char *out, size_t n);
u32 gh_new(int kind, HANDLE h);
GH *gh_get(u32 h);

static HANDLE open_host(const char *g, u32 style, char *hp, size_t n)
{
    path_to_host(g, hp, n);
    DWORD acc = (style & 3) == 1 ? GENERIC_WRITE : (style & 3) == 2 ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
    DWORD disp = (style & 0x1000) ? CREATE_ALWAYS : OPEN_EXISTING;
    return CreateFileA(hp, acc, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, disp, FILE_ATTRIBUTE_NORMAL, NULL);
}

/* OpenFile(name, OFSTRUCT *, style) -> HFILE or -1 */
void api_OpenFile(void)
{
    const char *g = gstr(ARG(0));
    u32 of = ARG(1), style = ARG(2);
    char hp[700];
    if (of) {
        memset(MEM + of, 0, 136);
        WR8(of, 136);
        snprintf((char *)MEM + of + 8, 128, "%s", g ? g : "");
    }
    if (!g) RET(0xffffffffu, 3);
    if (style & 0x200) {   /* OF_DELETE */
        path_to_host(g, hp, sizeof hp);
        RET(DeleteFileA(hp) ? 1 : 0xffffffffu, 3);
    }
    if (style & 0x4000) {  /* OF_EXIST */
        path_to_host(g, hp, sizeof hp);
        DWORD a = GetFileAttributesA(hp);
        if (getenv(ENV("FILEDEBUG"))) rt_log("OpenFile exist %s -> %s\n", g, a != INVALID_FILE_ATTRIBUTES ? "yes" : "no");
        RET(a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0xffffffffu, 3);
    }
    HANDLE h = open_host(g, style, hp, sizeof hp);
    if (h == INVALID_HANDLE_VALUE) {
        rt_log("OpenFile %s (%x) -> %s failed\n", g, style, hp);
        if (of) WR16(of + 2, (u16)GetLastError());
        RET(0xffffffffu, 3);
    }
    if (getenv(ENV("FILEDEBUG"))) rt_log("OpenFile %s (%x)\n", g, style);
    RET(gh_new(GH_FILE, h), 3);
}

static HANDLE hf(u32 h) { GH *g = gh_get(h); return g && g->kind == GH_FILE ? g->h : NULL; }

/* _lopen(name, mode) / _lcreat(name, attr) */
void api__lopen(void)
{
    char hp[700];
    HANDLE h = open_host(gstr(ARG(0)), ARG(1) & 3, hp, sizeof hp);
    RET(h == INVALID_HANDLE_VALUE ? 0xffffffffu : gh_new(GH_FILE, h), 2);
}
void api__lcreat(void)
{
    char hp[700];
    HANDLE h = open_host(gstr(ARG(0)), 0x1002, hp, sizeof hp);
    RET(h == INVALID_HANDLE_VALUE ? 0xffffffffu : gh_new(GH_FILE, h), 2);
}
void api__lclose(void)
{
    GH *g = gh_get(ARG(0));
    if (!g || g->kind != GH_FILE) RET(0xffffffffu, 1);
    CloseHandle(g->h);
    g->used = 0;
    RET(0, 1);
}
static u32 rd(u32 h, u32 buf, u32 n)
{
    HANDLE f = hf(h);
    DWORD got = 0;
    if (!f || !ReadFile(f, MEM + buf, n, &got, NULL)) return 0xffffffffu;
    return got;
}
static u32 wr(u32 h, u32 buf, u32 n)
{
    HANDLE f = hf(h);
    DWORD put = 0;
    if (!f) return 0xffffffffu;
    if (n == 0) { SetEndOfFile(f); return 0; }
    if (!WriteFile(f, MEM + buf, n, &put, NULL)) return 0xffffffffu;
    return put;
}
void api__lread(void) { RET(rd(ARG(0), ARG(1), ARG(2)), 3); }
void api__hread(void) { RET(rd(ARG(0), ARG(1), ARG(2)), 3); }
void api__lwrite(void) { RET(wr(ARG(0), ARG(1), ARG(2)), 3); }
void api__hwrite(void) { RET(wr(ARG(0), ARG(1), ARG(2)), 3); }
void api__llseek(void)
{
    HANDLE f = hf(ARG(0));
    if (!f) RET(0xffffffffu, 3);
    RET(SetFilePointer(f, (LONG)ARG(1), NULL, ARG(2)), 3);
}

void api_GetTickCount(void) { void timers_poll(void); timers_poll(); RET(now_ms(), 0); }

/* SHFileOperationA(SHFILEOPSTRUCTA *): hwnd, wFunc, pFrom, pTo, fFlags... -
   the game copies / deletes save files with it */
void api_SHFileOperationA(void)
{
    u32 s = ARG(0);
    u32 fn = RD32(s + 4);
    const char *from = (const char *)MEM + RD32(s + 8);
    const char *to = RD32(s + 12) ? (const char *)MEM + RD32(s + 12) : "";
    int ok = 1;
    /* double-NUL-terminated lists; the game passes one name each */
    char a[700], b[700];
    path_to_host(from, a, sizeof a);
    if (*to) path_to_host(to, b, sizeof b);
    rt_log("SHFileOperation %u %s -> %s\n", fn, from, to);
    if (fn == FO_DELETE) ok = DeleteFileA(a) || GetLastError() == ERROR_FILE_NOT_FOUND;
    else if (fn == FO_COPY) ok = CopyFileA(a, b, FALSE);
    else if (fn == FO_MOVE) { DeleteFileA(b); ok = MoveFileA(a, b); }
    else if (fn == FO_RENAME) ok = MoveFileA(a, b);
    if (RD32(s + 0x12) && 0) WR32(s + 0x12, 0);   /* fAnyOperationsAborted stays as it is */
    RET(ok ? 0 : 1, 1);
}

/* ---------------- mmio ---------------- */
/* MMIOINFO: dwFlags 0, fccIOProc 4, pIOProc 8, wErrorRet 12, htask 16,
   cchBuffer 20, pchBuffer 24, pchNext 28, pchEndRead 32, pchEndWrite 36,
   lBufOffset 40, lDiskOffset 44, adwInfo 48, res 60, res 64, hmmio 68 (72 bytes)
   MMCKINFO: ckid 0, cksize 4, fccType 8, dwDataOffset 12, dwFlags 16 */
#define MAXMM 32
typedef struct {
    int used;
    HANDLE f;
    u32 buf, cch;          /* guest buffer */
    u32 buf_off;           /* file offset of buf[0] */
    u32 next, end;         /* read position / valid bytes in buf */
    u32 flags;
} MM;
static MM mms[MAXMM];
#define MM_BASE 0x00e00000u
static MM *mmget(u32 h) { u32 i = h - MM_BASE; return i < MAXMM && mms[i].used ? &mms[i] : NULL; }
static u32 mpos(MM *m) { return m->buf_off + m->next; }
static void mjump(MM *m, u32 pos)
{
    if (pos >= m->buf_off && pos <= m->buf_off + m->end) { m->next = pos - m->buf_off; return; }
    m->buf_off = pos; m->next = m->end = 0;
}
static void mfill(MM *m)
{
    u32 pos = mpos(m);
    DWORD got = 0;
    SetFilePointer(m->f, (LONG)pos, NULL, FILE_BEGIN);
    if (!ReadFile(m->f, MEM + m->buf, m->cch, &got, NULL)) got = 0;
    m->buf_off = pos; m->next = 0; m->end = got;
}
static u32 mread(MM *m, u8 *dst, u32 n)
{
    u32 done = 0;
    while (done < n) {
        if (m->next >= m->end) { mfill(m); if (!m->end) break; }
        u32 k = m->end - m->next;
        if (k > n - done) k = n - done;
        memcpy(dst + done, MEM + m->buf + m->next, k);
        m->next += k; done += k;
    }
    return done;
}

/* mmioOpenA(name, info, flags) */
void api_mmioOpenA(void)
{
    char hp[700];
    const char *g = gstr(ARG(0));
    u32 info = ARG(1), fl = ARG(2);
    if (!g) RET(0, 3);
    path_to_host(g, hp, sizeof hp);
    DWORD acc = (fl & 3) == 1 ? GENERIC_WRITE : (fl & 3) == 2 ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
    HANDLE f = CreateFileA(hp, acc, FILE_SHARE_READ, NULL, (fl & 0x1000) ? CREATE_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        rt_log("mmioOpenA %s failed\n", g);
        if (info) WR32(info + 12, 257);   /* MMIOERR_CANNOTOPEN */
        RET(0, 3);
    }
    for (int i = 0; i < MAXMM; i++)
        if (!mms[i].used) {
            MM *m = &mms[i];
            memset(m, 0, sizeof *m);
            m->used = 1; m->f = f; m->flags = fl;
            m->cch = (info && RD32(info + 20)) ? RD32(info + 20) : 8192;
            m->buf = gmalloc(m->cch);
            if (info) WR32(info + 12, 0);
            if (getenv(ENV("FILEDEBUG"))) rt_log("mmioOpenA %s\n", g);
            RET(MM_BASE + i, 3);
        }
    CloseHandle(f);
    RET(0, 3);
}
void api_mmioClose(void)
{
    MM *m = mmget(ARG(0));
    if (m) { CloseHandle(m->f); gfree(m->buf); m->used = 0; }
    RET(0, 2);
}
void api_mmioRead(void)
{
    MM *m = mmget(ARG(0));
    if (!m) RET(0xffffffffu, 3);
    RET(mread(m, MEM + ARG(1), ARG(2)), 3);
}
void api_mmioWrite(void)
{
    MM *m = mmget(ARG(0));
    if (!m) RET(0xffffffffu, 3);
    DWORD put = 0;
    SetFilePointer(m->f, (LONG)mpos(m), NULL, FILE_BEGIN);
    WriteFile(m->f, MEM + ARG(1), ARG(2), &put, NULL);
    m->buf_off = mpos(m) + put; m->next = m->end = 0;
    RET(put, 3);
}
void api_mmioSeek(void)
{
    MM *m = mmget(ARG(0));
    if (!m) RET(0xffffffffu, 3);
    s32 off = (s32)ARG(1);
    u32 pos;
    if (ARG(2) == 1) pos = mpos(m) + off;
    else if (ARG(2) == 2) pos = GetFileSize(m->f, NULL) + off;
    else pos = (u32)off;
    mjump(m, pos);
    RET(pos, 3);
}
static void info_out(MM *m, u32 info, u32 h)
{
    WR32(info + 0, m->flags);
    WR32(info + 20, m->cch);
    WR32(info + 24, m->buf);
    WR32(info + 28, m->buf + m->next);
    WR32(info + 32, m->buf + m->end);
    WR32(info + 36, m->buf + m->cch);
    WR32(info + 40, m->buf_off);
    WR32(info + 44, m->buf_off + m->end);
    WR32(info + 68, h);
}
void api_mmioGetInfo(void)
{
    MM *m = mmget(ARG(0));
    if (!m) RET(5, 3);   /* MMSYSERR_INVALHANDLE */
    memset(MEM + ARG(1), 0, 72);
    info_out(m, ARG(1), ARG(0));
    RET(0, 3);
}
void api_mmioSetInfo(void)
{
    MM *m = mmget(ARG(0));
    if (!m) RET(5, 3);
    u32 nx = RD32(ARG(1) + 28);
    if (nx >= m->buf && nx <= m->buf + m->end) m->next = nx - m->buf;
    RET(0, 3);
}
/* mmioAdvance(h, info, MMIO_READ): the next buffer-full from the read position */
void api_mmioAdvance(void)
{
    MM *m = mmget(ARG(0));
    if (!m) RET(5, 3);
    if (ARG(1)) {
        u32 nx = RD32(ARG(1) + 28);
        if (nx >= m->buf && nx <= m->buf + m->end) m->next = nx - m->buf;
    }
    mfill(m);
    if (ARG(1)) info_out(m, ARG(1), ARG(0));
    RET(0, 3);
}
static u32 rd32(MM *m, int *ok) { u8 b[4]; *ok = mread(m, b, 4) == 4; return b[0] | b[1] << 8 | b[2] << 16 | (u32)b[3] << 24; }
#define FCC(a, b, c, d) ((u32)(a) | (u32)(b) << 8 | (u32)(c) << 16 | (u32)(d) << 24)
/* mmioDescend(h, ck, parent, flags) */
void api_mmioDescend(void)
{
    MM *m = mmget(ARG(0));
    u32 ck = ARG(1), par = ARG(2), fl = ARG(3);
    if (!m) RET(5, 4);
    u32 want_id = RD32(ck), want_type = RD32(ck + 8);
    u32 limit = par ? RD32(par + 12) + RD32(par + 4) : 0xffffffffu;
    for (;;) {
        u32 at = mpos(m);
        if (at + 8 > limit) RET(265, 4);   /* MMIOERR_CHUNKNOTFOUND */
        int ok1, ok2, ok3 = 1;
        u32 id = rd32(m, &ok1), sz = rd32(m, &ok2), type = 0;
        if (!ok1 || !ok2) RET(265, 4);
        u32 data = at + 8;
        if (id == FCC('R', 'I', 'F', 'F') || id == FCC('L', 'I', 'S', 'T')) type = rd32(m, &ok3);
        if (!ok3) RET(265, 4);
        int match = 1;
        if (fl & 0x10) match = id == want_id;                                      /* MMIO_FINDCHUNK */
        else if (fl & 0x20) match = id == FCC('R', 'I', 'F', 'F') && type == want_type; /* MMIO_FINDRIFF */
        else if (fl & 0x40) match = id == FCC('L', 'I', 'S', 'T') && type == want_type; /* MMIO_FINDLIST */
        if (match) {
            WR32(ck, id); WR32(ck + 4, sz); WR32(ck + 8, type); WR32(ck + 12, data); WR32(ck + 16, 0);
            RET(0, 4);
        }
        mjump(m, data + ((sz + 1) & ~1u));
    }
}
/* mmioAscend(h, ck, 0): to the end of the chunk */
void api_mmioAscend(void)
{
    MM *m = mmget(ARG(0));
    if (!m) RET(5, 3);
    u32 ck = ARG(1);
    mjump(m, RD32(ck + 12) + ((RD32(ck + 4) + 1) & ~1u));
    RET(0, 3);
}
