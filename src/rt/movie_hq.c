/* Ring's movies (CNM) in their full colour.
 *
 * The movie decoder writes every frame as a 24-bit (or 32-bit) aImage:
 * 42c310(this; image, bpp) decodes the next frame into image (+09 pixels,
 * +1d stride, +21 orientation, +29 / +2d size).  The game then reduces it to
 * its 16-bit screen.  Here the decoded frame is kept as it was; at
 * presentation it is shown where the screen holds the 16-bit copy of it. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

static int mdebug = -1;
static u32 *mframe;                 /* last decoded frame, top-down 0x00RRGGBB */
static int mw, mh;
static u32 mserial, mframe_ms;

int movie_frame(const u32 **pix, int *w, int *h, u32 *serial, u32 *ms)
{
    if (!mframe) return 0;
    *pix = mframe; *w = mw; *h = mh; *serial = mserial; *ms = mframe_ms;
    return 1;
}

/* 42c310(this; image, bpp): one frame decoded */
void N_movie_decode(void)
{
    if (mdebug < 0) mdebug = getenv(ENV("MOVIEDEBUG")) != NULL;
    u32 img = ARG(0);
    rt_wrap_orig("N_movie_decode", NULL)();
    if (!(cpu.r[R_EAX] & 0xff) || !img) return;
    u32 data = RD32(img + 9), stride = RD32(img + 0x1d), orient = RD32(img + 0x21), bpp = RD32(img + 0x11);
    int w = (int)RD32(img + 0x29), h = (int)RD32(img + 0x2d);
    if (!data || w <= 0 || h <= 0 || w > 1024 || h > 1024 || (bpp != 0x18 && bpp != 0x20)) return;
    if (w != mw || h != mh) { free(mframe); mframe = malloc((size_t)w * h * 4); mw = w; mh = h; }
    int b = bpp == 0x18 ? 3 : 4;
    for (int y = 0; y < h; y++) {
        /* orientation 2 = bottom-up rows */
        const u8 *s = MEM + data + (u32)((orient == 1 ? y : h - 1 - y) * stride);
        u32 *d = mframe + (size_t)y * w;
        for (int x = 0; x < w; x++, s += b) d[x] = (u32)s[2] << 16 | (u32)s[1] << 8 | s[0];   /* BGR in memory */
    }
    mserial++;
    mframe_ms = now_ms();
    if (mdebug) {
        static u32 t, n0;
        if (now_ms() - t > 1000) {
            rt_log("movie: frame %dx%d %d-bit orient %u stride %u, %u frames/s\n", w, h, b * 8, orient, stride, mserial - n0);
            t = now_ms(); n0 = mserial;
        }
    }
}

/* ---------------- presentation ---------------- */
extern int opt_moviecolor;
void present_frame32(const u32 *pix, int w, int h);
static u32 *out;
static int oy_best = 16;

static inline u16 to565(u32 c) { return (u16)(((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x1f)); }
static inline u32 from565(u32 c)
{
    u32 r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
}

static void save_bmp(const char *name, const u32 *pix, int w, int h)
{
    char path[700];
    snprintf(path, sizeof path, "%s/shots/%s.bmp", rt_exe_dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    BITMAPFILEHEADER fh = { 0x4d42, (DWORD)(sizeof fh + 40 + (size_t)w * h * 4), 0, 0, sizeof fh + 40 };
    BITMAPINFOHEADER ih = { 40, w, -h, 1, 32, 0, (DWORD)((size_t)w * h * 4), 0, 0, 0, 0 };
    fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, 40, 1, f); fwrite(pix, 4, (size_t)w * h, f);
    fclose(f);
}

/* ---------------- clean-up of the decoded frame ---------------- */
extern int opt_moviedeblock, opt_moviedeband;
static u32 *proc;                   /* the frame after deblocking / debanding */
static u32 proc_serial = ~0u;
static s16 *wk;                     /* working copy, 3 channels x 16 (12.4 fixed) */

/* the codec works in 4 x 4 blocks: soften block edges where both sides are
   flat and the step between them is small (a compression edge, not a real one) */
static void deblock(int w, int h)
{
    for (int pass = 0; pass < 2; pass++) {
        int along = pass == 0 ? h : w, across = pass == 0 ? w : h;
        for (int a = 0; a < along; a++)
            for (int e = 4; e + 1 < across; e += 4)
                for (int c = 0; c < 3; c++) {
                    int st = pass == 0 ? 3 : 3 * w;
                    s16 *q0 = wk + (pass == 0 ? ((size_t)a * w + e) : ((size_t)e * w + a)) * 3 + c;
                    s16 *p0 = q0 - st, *p1 = q0 - 2 * st, *q1 = q0 + st;
                    int d = *q0 - *p0;
                    if (abs(d) >= 12 * 16 || abs(*p1 - *p0) >= 3 * 16 || abs(*q1 - *q0) >= 3 * 16) continue;
                    *p0 += d / 4; *q0 -= d / 4;
                    *p1 += d / 8; *q1 -= d / 8;
                }
    }
}

/* smooth gradients that compression turned into steps: average four
   pixels around (a fixed random offset per pixel, so no flicker) wherever
   all of them are within a few levels, then dither back to 8 bits */
static void deband(int w, int h, u32 *dst)
{
    static const u8 bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
    const int thr = 4 * 16;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            u32 hsh = (u32)(x * 73856093) ^ (u32)(y * 19349663);
            hsh ^= hsh >> 13; hsh *= 0x5bd1e995u; hsh ^= hsh >> 15;
            int r = 3 + (int)(hsh % 12);
            int dx = (int)((hsh >> 8) % (u32)(2 * r + 1)) - r, dy = r - abs(dx);
            if (hsh & 0x10000) dy = -dy;
            int sx[4] = { x + dx, x - dx, x - dy, x + dy }, sy[4] = { y + dy, y - dy, y + dx, y - dx };
            const s16 *pp = wk + ((size_t)y * w + x) * 3;
            int acc[3] = { 0, 0, 0 }, ok = 1;
            for (int k = 0; k < 4 && ok; k++) {
                int xx = sx[k] < 0 ? 0 : sx[k] >= w ? w - 1 : sx[k];
                int yy = sy[k] < 0 ? 0 : sy[k] >= h ? h - 1 : sy[k];
                const s16 *q = wk + ((size_t)yy * w + xx) * 3;
                for (int c = 0; c < 3; c++) { if (abs(q[c] - pp[c]) > thr) ok = 0; acc[c] += q[c]; }
            }
            int dith = bayer[(y & 3) * 4 + (x & 3)];
            u32 o = 0;
            for (int c = 0; c < 3; c++) {
                int v = ok ? (acc[c] + 2) / 4 : pp[c];
                v = (v + dith) >> 4;
                if (v < 0) v = 0;
                if (v > 255) v = 255;
                o |= (u32)v << (16 - 8 * c);
            }
            dst[(size_t)y * w + x] = o;
        }
}

static const u32 *processed(void)
{
    if (!opt_moviedeblock && !opt_moviedeband) return mframe;
    if (proc_serial == mserial && proc) return proc;
    proc = realloc(proc, (size_t)mw * mh * 4);
    wk = realloc(wk, (size_t)mw * mh * 3 * sizeof *wk);
    for (size_t i = 0; i < (size_t)mw * mh; i++) {
        u32 c = mframe[i];
        wk[3 * i] = (s16)(((c >> 16) & 255) << 4); wk[3 * i + 1] = (s16)(((c >> 8) & 255) << 4); wk[3 * i + 2] = (s16)((c & 255) << 4);
    }
    if (opt_moviedeblock) deblock(mw, mh);
    if (opt_moviedeband) deband(mw, mh, proc);
    else
        for (size_t i = 0; i < (size_t)mw * mh; i++) {
            int r = (wk[3 * i] + 8) >> 4, g = (wk[3 * i + 1] + 8) >> 4, b = (wk[3 * i + 2] + 8) >> 4;
            r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
            proc[i] = (u32)r << 16 | (u32)g << 8 | (u32)b;
        }
    proc_serial = mserial;
    return proc;
}

/* ddraw.c, a 16-bit frame of the primary: 1 = presented here */
int movie_present(u32 mem, int pitch)
{
    if (!opt_moviecolor || !mframe || now_ms() - mframe_ms > 300) return 0;
    /* where the frame sits (rows), found on pixels with detail */
    static u32 t_search;
    if (now_ms() - t_search > 500) {
        t_search = now_ms();
        int best = -1, bdy = oy_best;
        for (int dy = 0; dy + mh <= 480; dy++) {
            int m = 0;
            for (int y = 8; y < mh; y += 13)
                for (int x = 5; x < mw && x < 640; x += 17) {
                    u32 c = mframe[y * mw + x];
                    if ((c & 0xe0e0e0) && RD16(mem + (u32)(y + dy) * pitch + x * 2) == to565(c)) m++;
                }
            if (m > best) { best = m; bdy = dy; }
        }
        if (best > 10) oy_best = bdy;
    }
    if (!out) out = malloc(640 * 480 * 4);
    const u32 *pf = processed();
    int hit = 0, tot = 0;
    for (int y = 0; y < 480; y++) {
        const u16 *s = (const u16 *)(MEM + mem + (u32)y * pitch);
        u32 *d = out + y * 640;
        int fy = y - oy_best;
        const u32 *m = fy >= 0 && fy < mh && mw == 640 ? mframe + (size_t)fy * mw : NULL;
        const u32 *pm = m ? pf + (size_t)fy * mw : NULL;
        for (int x = 0; x < 640; x++) {
            if (m) { tot++; if (s[x] == to565(m[x])) { d[x] = pm[x]; hit++; continue; } }
            d[x] = from565(s[x]);
        }
    }
    if (mdebug) {
        static u32 t;
        if (now_ms() - t > 1000) { t = now_ms(); rt_log("movie: %d of %d pixels in full colour (row %d)\n", hit, tot, oy_best); }
        const char *sv = getenv(ENV("MOVIESAVE"));
        static int saved;
        extern u32 win_time(void);
        if (sv && !saved && win_time() >= (u32)atoi(sv)) {
            saved = 1;
            static u32 o16[640 * 480];
            for (int y = 0; y < 480; y++) for (int x = 0; x < 640; x++) o16[y * 640 + x] = from565(RD16(mem + (u32)y * pitch + x * 2));
            save_bmp("movie_16bit", o16, 640, 480);
            save_bmp("movie_ours", out, 640, 480);
        }
    }
    if (tot && hit * 4 < tot) return 0;   /* the frame is not (or no longer) on the screen */
    present_frame32(out, 640, 480);
    return 1;
}
