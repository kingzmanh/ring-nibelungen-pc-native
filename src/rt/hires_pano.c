/* Real higher internal resolution for Ring's 360-degree panoramas.
 *
 * The game (Arxel's "Aquator" engine) keeps a whole node as a VQ-compressed
 * 2048 x H picture - a table of codebook indices, one per 4 horizontal
 * texels - and warps it onto its 640x480 frame buffer in 16 x 16 screen
 * blocks: 412220(frame buffer, pitch) walks the block grid and 4116d0 fills
 * one block, interpolating the source (u, v) between the block's four
 * corners (16.16 fixed point, u wrapping at 2048).
 *
 * Here every block is drawn a second time, N x N finer, from the same
 * corners, with bilinear filtering, into a host picture of 640N x 480N.  The
 * node is decoded to a plain texture once (again when the node changes).
 * The game's own 1x block is kept as it drew it; at presentation the N x
 * picture is shown wherever the screen still holds that 1x pixel, so
 * everything drawn over the view (cursor, animations, subtitles, inventory)
 * stays exactly as the game draws it. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

extern int opt_render;
#define TABLE_OFF 0x1fa40u          /* codebook (4 RGB565 texels per entry) comes first */
#define PW 2048                     /* panorama width in texels */
#define PH_MAX 1024

static int frame_n;                 /* N of the frame being drawn (1 = off) */
static u32 fb_base, fb_pitch;       /* the game's 640x480 16-bit frame buffer */
static u32 frame_ms;
static int pano_debug = -1;

/* the decoded node */
static u32 *tex;                    /* PW x PH_MAX, 0x00RRGGBB */
static u32 tex_src, tex_hash;
static int tex_h;

/* the N x picture and the game's 1x picture under it */
static u32 *hr;                     /* 640N x 480N */
static int hr_n;
static u16 *lo;                     /* what the warp wrote, 640x480 */
static u8 *lo_on;                   /* 1 = written in this frame */
static u32 *layer;
static int njobs;                   /* N x blocks queued this frame (render_all) */
static void render_all(int n);
static u16 *shadow_tab;             /* the node's index table and codebook as last decoded */
static u16 shadow_cb[TABLE_OFF / 2];

static inline u32 c565(u32 c)
{
    u32 r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
}

static u32 node_hash(u32 src)
{
    u32 h = 2166136261u;
    for (int row = 0; row < 680; row += 37)
        for (int k = 0; k < 512; k += 29) h = (h ^ RD16(src + TABLE_OFF + (u32)(row * 512 + k) * 2)) * 16777619u;
    for (u32 k = 0; k < 256; k += 4) h = (h ^ RD32(src + k * 61)) * 16777619u;
    return h;
}

/* decode the node: texel (u, v) = codebook[table[(v * 2048 + u) >> 2]][u & 3] */
static void decode_node(u32 src, int vmax)
{
    if (tex && src == tex_src && vmax < tex_h) return;
    if (!tex) tex = malloc((size_t)PW * PH_MAX * 4);
    int H = vmax + 1;
    if (H < 688) H = 688;
    if (H > PH_MAX) H = PH_MAX;
    const u16 *cb = (const u16 *)(MEM + src);
    const u16 *tab = (const u16 *)(MEM + src + TABLE_OFF);
    for (int v = 0; v < H; v++) {
        u32 *o = tex + (size_t)v * PW;
        const u16 *t = tab + (size_t)v * (PW / 4);
        for (int g = 0; g < PW / 4; g++) {
            const u16 *p = cb + (u32)t[g] * 4;
            o[4 * g] = c565(p[0]); o[4 * g + 1] = c565(p[1]); o[4 * g + 2] = c565(p[2]); o[4 * g + 3] = c565(p[3]);
        }
    }
    tex_src = src; tex_hash = node_hash(src); tex_h = H;
    if (pano_debug) rt_log("pano: node %08x decoded (%d rows)\n", src, H);
    /* shadow copies to follow later changes (animations written into the node) */
    shadow_tab = realloc(shadow_tab, (size_t)H * (PW / 4) * 2);
    memcpy(shadow_tab, tab, (size_t)H * (PW / 4) * 2);
    memcpy(shadow_cb, cb, TABLE_OFF);
}

/* once per frame: texels whose index or codebook entry changed since the last
   look are decoded again (the game animates things inside the node itself) */
static void sync_node(u32 src)
{
    if (!tex || src != tex_src || !tex_h || !shadow_tab) return;
    const u16 *cb = (const u16 *)(MEM + src);
    const u16 *tab = (const u16 *)(MEM + src + TABLE_OFF);
    int ngroups = tex_h * (PW / 4);
    /* codebook entries (4 texels each) that changed */
    static u8 cbdirty[TABLE_OFF / 8];
    int ncb = 0;
    for (u32 e = 0; e < TABLE_OFF / 8; e++) {
        int d = memcmp(cb + e * 4, shadow_cb + e * 4, 8) != 0;
        cbdirty[e] = (u8)d;
        if (d) { memcpy(shadow_cb + e * 4, cb + e * 4, 8); ncb++; }
    }
    int nt = 0, x0 = PW, x1 = -1, y0 = tex_h, y1 = -1;
    for (int g = 0; g < ngroups; g++) {
        u16 idx = tab[g];
        int changed = idx != shadow_tab[g];
        if (!changed && !(ncb && idx < TABLE_OFF / 8 && cbdirty[idx])) continue;
        shadow_tab[g] = idx;
        const u16 *p = cb + (u32)idx * 4;
        int v = g / (PW / 4), u = (g % (PW / 4)) * 4;
        u32 *o = tex + (size_t)v * PW + u;
        o[0] = c565(p[0]); o[1] = c565(p[1]); o[2] = c565(p[2]); o[3] = c565(p[3]);
        nt++;
        if (u < x0) x0 = u;
        if (u + 3 > x1) x1 = u + 3;
        if (v < y0) y0 = v;
        if (v > y1) y1 = v;
    }
    if (pano_debug && (nt || ncb)) {
        static u32 t;
        static int frames, total;
        frames++; total += nt;
        if (now_ms() - t > 1000) {
            rt_log("pano: node changes: %d texel groups in %d frames (last frame: %d groups, %d codebook entries, area %d,%d-%d,%d)\n",
                   total, frames, nt, ncb, x0, y0, x1, y1);
            t = now_ms(); frames = total = 0;
        }
    }
}

/* 412220(this; frame buffer, pitch in bytes): one frame of the view */
void N_pano_frame(void)
{
    if (pano_debug < 0) pano_debug = getenv(ENV("PANODEBUG")) != NULL;
    int n = opt_render < 1 ? 1 : opt_render > 4 ? 4 : opt_render;
    fb_base = ARG(0);
    fb_pitch = ARG(1);
    frame_n = fb_pitch == 1280 ? n : 1;
    if (pano_debug) { static int nl; if (nl++ < 3) rt_log("pano: frame buf %08x pitch %u n %d\n", fb_base, fb_pitch, frame_n); }
    if (frame_n > 1) {
        u32 src = RD32(cpu.r[R_ECX] + 4);   /* the node (this+4) - decoded again when it changed */
        if (tex && src != tex_src) tex_h = 0;
        else sync_node(src);
        if (hr_n != frame_n) { free(hr); hr = malloc((size_t)640 * frame_n * 480 * frame_n * 4); hr_n = frame_n; }
        if (!lo) { lo = malloc(640 * 480 * 2); lo_on = malloc(640 * 480); }
        memset(lo_on, 0, 640 * 480);
    }
    njobs = 0;
    rt_wrap_orig("N_pano_frame", NULL)();
    if (frame_n > 1) { render_all(frame_n); frame_ms = now_ms(); }
}

/* bilinear texel fetch, (u, v) in 16.16 with texel centres at +0.5 */
static inline u32 sample(s32 u, s32 v, int vlo, int vhi)
{
    u -= 0x8000; v -= 0x8000;
    int ui = (u >> 16), vi = (v >> 16);
    u32 fu = (u >> 8) & 0xff, fv = (v >> 8) & 0xff;
    int v0 = vi < vlo ? vlo : vi > vhi ? vhi : vi;
    int v1 = vi + 1 < vlo ? vlo : vi + 1 > vhi ? vhi : vi + 1;
    const u32 *r0 = tex + (size_t)v0 * PW, *r1 = tex + (size_t)v1 * PW;
    int u0 = ui & (PW - 1), u1 = (ui + 1) & (PW - 1);
    u32 a = r0[u0], b = r0[u1], c = r1[u0], d = r1[u1];
    /* red+blue and green lanes, 8-bit fractions */
    u32 rb0 = ((a & 0xff00ff) * (256 - fu) + (b & 0xff00ff) * fu) >> 8 & 0xff00ff;
    u32 g0 = ((a & 0xff00) * (256 - fu) + (b & 0xff00) * fu) >> 8 & 0xff00;
    u32 rb1 = ((c & 0xff00ff) * (256 - fu) + (d & 0xff00ff) * fu) >> 8 & 0xff00ff;
    u32 g1 = ((c & 0xff00) * (256 - fu) + (d & 0xff00) * fu) >> 8 & 0xff00;
    u32 rb = (rb0 * (256 - fv) + rb1 * fv) >> 8 & 0xff00ff;
    u32 g = (g0 * (256 - fv) + g1 * fv) >> 8 & 0xff00;
    return 0xff000000u | rb | g;
}

/* Catmull-Rom (sharp bicubic) texel fetch: 4 x 4 texels, weights from a
   table by the 8-bit fraction, overshoot clamped */
static s16 crw[256][4];
static void crw_init(void)
{
    for (int i = 0; i < 256; i++) {
        double t = i / 256.0, t2 = t * t, t3 = t2 * t;
        double w[4] = { -0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1.0, -1.5 * t3 + 2.0 * t2 + 0.5 * t, 0.5 * t3 - 0.5 * t2 };
        for (int k = 0; k < 4; k++) crw[i][k] = (s16)(w[k] * 1024.0 + (w[k] < 0 ? -0.5 : 0.5));
    }
}
static inline u32 sample_cr(s32 u, s32 v, int vlo, int vhi)
{
    u -= 0x8000; v -= 0x8000;
    int ui = (u >> 16), vi = (v >> 16);
    const s16 *wx = crw[(u >> 8) & 0xff], *wy = crw[(v >> 8) & 0xff];
    int us[4];
    for (int k = 0; k < 4; k++) us[k] = (ui - 1 + k) & (PW - 1);
    int r = 0, g = 0, b = 0;
    for (int j = 0; j < 4; j++) {
        int vv = vi - 1 + j;
        vv = vv < vlo ? vlo : vv > vhi ? vhi : vv;
        const u32 *row = tex + (size_t)vv * PW;
        int hr_ = 0, hg = 0, hb = 0;
        for (int k = 0; k < 4; k++) {
            u32 c = row[us[k]];
            hr_ += (int)((c >> 16) & 255) * wx[k]; hg += (int)((c >> 8) & 255) * wx[k]; hb += (int)(c & 255) * wx[k];
        }
        r += hr_ * wy[j]; g += hg * wy[j]; b += hb * wy[j];
    }
    r = (r + (1 << 19)) >> 20; g = (g + (1 << 19)) >> 20; b = (b + (1 << 19)) >> 20;
    r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
    return 0xff000000u | (u32)r << 16 | (u32)g << 8 | (u32)b;
}

/* the N x blocks of a frame are drawn after the game's frame, on worker threads */
extern int opt_panosharp;
typedef struct { int bx, by, vlo, vhi; s32 cu[4], cv[4]; } Job;
static Job jobs[2048];
static int njobs;
static void render_job(const Job *j, int n)
{
    int W = 640 * n, S = 16 * n;
    double inv = 1.0 / (16.0 * n);
    int sharp = opt_panosharp;
    for (int yy = 0; yy < S; yy++) {
        double ty = ((yy + 0.5) - 0.5 * n) * inv;
        double lu = j->cu[0] + (j->cu[2] - j->cu[0]) * ty, lv = j->cv[0] + (j->cv[2] - j->cv[0]) * ty;   /* left edge */
        double ru = j->cu[1] + (j->cu[3] - j->cu[1]) * ty, rv = j->cv[1] + (j->cv[3] - j->cv[1]) * ty;   /* right edge */
        double x0 = (0.5 - 0.5 * n) * inv;
        s32 u = (s32)(lu + (ru - lu) * x0), v = (s32)(lv + (rv - lv) * x0);
        s32 du = (s32)((ru - lu) * inv), dv = (s32)((rv - lv) * inv);
        u32 *o = hr + (size_t)(j->by * n + yy) * W + j->bx * n;
        if (sharp) for (int xx = 0; xx < S; xx++, u += du, v += dv) o[xx] = sample_cr(u, v, j->vlo, j->vhi);
        else for (int xx = 0; xx < S; xx++, u += du, v += dv) o[xx] = sample(u, v, j->vlo, j->vhi);
    }
}

#define NWORK 6
static HANDLE work_go[NWORK], work_done[NWORK];
static volatile LONG next_job;
static int work_n;
static DWORD WINAPI worker(LPVOID p)
{
    int id = (int)(intptr_t)p;
    for (;;) {
        WaitForSingleObject(work_go[id], INFINITE);
        for (;;) {
            LONG k = InterlockedIncrement(&next_job) - 1;
            if (k >= njobs) break;
            render_job(&jobs[k], work_n);
        }
        SetEvent(work_done[id]);
    }
    return 0;
}
static void render_all(int n)
{
    static int started;
    if (!started) {
        started = 1;
        crw_init();
        for (int i = 0; i < NWORK; i++) {
            work_go[i] = CreateEventA(NULL, FALSE, FALSE, NULL);
            work_done[i] = CreateEventA(NULL, FALSE, FALSE, NULL);
            CreateThread(NULL, 0, worker, (LPVOID)(intptr_t)i, 0, NULL);
        }
    }
    work_n = n;
    next_job = 0;
    for (int i = 0; i < NWORK; i++) SetEvent(work_go[i]);
    WaitForMultipleObjects(NWORK, work_done, TRUE, INFINITE);
    njobs = 0;
}

/* 4116d0(cdecl src, w, h, TL u,v, TR u,v, BL u,v, BR u,v, dst, skip): one block */
void N_pano_block(void)
{
    u32 src = ARG(0), w = ARG(1), h = ARG(2), dst = ARG(11);
    s32 cu[4] = { (s32)ARG(3), (s32)ARG(5), (s32)ARG(7), (s32)ARG(9) };
    s32 cv[4] = { (s32)ARG(4), (s32)ARG(6), (s32)ARG(8), (s32)ARG(10) };
    rt_wrap_orig("N_pano_block", NULL)();
    int n = frame_n;
    if (n < 2 || w != 16 || h != 16 || dst < fb_base) return;
    u32 off = dst - fb_base;
    int bx = (int)((off % fb_pitch) / 2), by = (int)(off / fb_pitch);
    if (bx + 16 > 640 || by + 16 > 480) return;
    /* the u wrap at 2048 texels: all four corners on the same side (as 411800 does) */
    for (int k = 1; k < 4; k++) {
        if (cu[k] - cu[0] > 0x4000000) cu[k] -= 0x8000000;
        else if (cu[k] - cu[0] < -0x4000000) cu[k] += 0x8000000;
    }
    int vlo = 0x7fffffff, vhi = 0;
    for (int k = 0; k < 4; k++) { int v = cv[k] >> 16; if (v < vlo) vlo = v; if (v > vhi) vhi = v; }
    if (vlo < 0) vlo = 0;
    if (vhi >= PH_MAX) return;
    decode_node(src, vhi);
    if (vhi >= tex_h) vhi = tex_h - 1;
    /* the game's 1x pixels of this block */
    for (int y = 0; y < 16; y++) {
        memcpy(lo + (by + y) * 640 + bx, MEM + dst + (u32)y * fb_pitch, 32);
        memset(lo_on + (by + y) * 640 + bx, 1, 16);
    }
    /* N x, drawn with the rest of the frame (render_job): screen position
       s = (k + 0.5) / N - 0.5 within the block, (u, v) bilinear in the corners */
    if (njobs < (int)(sizeof jobs / sizeof jobs[0])) {
        Job *j = &jobs[njobs++];
        j->bx = bx; j->by = by; j->vlo = vlo; j->vhi = vhi;
        memcpy(j->cu, cu, sizeof cu); memcpy(j->cv, cv, sizeof cv);
    }
}

/* ddraw.c, presenting a 16-bit frame: the N x view where the screen still
   shows the game's warped pixels */
void pano_compose(u32 mem, int pitch)
{
    hires_layer.valid = 0;
    int n = hr_n;
    if (frame_n < 2 || n != frame_n || !lo || now_ms() - frame_ms > 250) return;
    int W = 640 * n, H = 480 * n;
    if (!layer) layer = malloc((size_t)2560 * 1920 * 4);
    /* where the frame buffer sits on the screen: the row offset that matches
       best on pixels with detail (black sky matches anywhere) */
    static int dy_best;
    {
        static u32 t;
        if (now_ms() - t > 500) {
            t = now_ms();
            int best = -1, bdy = dy_best;
            for (int dy = -64; dy <= 64; dy++) {
                int m = 0;
                for (int y = 40; y < 440; y += 7) {
                    int sy = y + dy;
                    if (sy < 0 || sy >= 480) continue;
                    const u16 *scr = (const u16 *)(MEM + mem + (u32)sy * pitch);
                    for (int x = 3; x < 640; x += 11)
                        if (lo_on[y * 640 + x] && (lo[y * 640 + x] & 0xe79c) && scr[x] == lo[y * 640 + x]) m++;
                }
                if (m > best) { best = m; bdy = dy; }
            }
            if (best > 20) dy_best = bdy;
        }
    }
    int oy = dy_best;
    int any = 0, hit = 0, tot = 0;
    for (int y = 0; y < 480; y++) {
        int sy = y + oy;
        int inside = sy >= 0 && sy < 480;
        const u16 *scr = (const u16 *)(MEM + mem + (u32)(inside ? sy : 0) * pitch);
        const u16 *l = lo + y * 640;
        static const u8 none[640];
        const u8 *on = inside ? lo_on + y * 640 : none;
        for (int x = 0; x < 640; x++) {
            int ok = on[x] && scr[x] == l[x];
            tot += on[x]; hit += ok; any |= ok;
            for (int yy = 0; yy < n; yy++) {
                u32 *o = layer + (size_t)(y * n + yy) * W + x * n;
                const u32 *h = hr + (size_t)(y * n + yy) * W + x * n;
                if (ok) for (int xx = 0; xx < n; xx++) o[xx] = h[xx];
                else for (int xx = 0; xx < n; xx++) o[xx] = 0;
            }
        }
    }
    if (pano_debug) {
        static u32 t;
        if (now_ms() - t > 1000) { t = now_ms(); rt_log("pano: %dx, %d of %d warped pixels on screen (row offset %d)\n", n, hit, tot, oy); }
    }
    if (!any) return;
    /* layer row y = frame buffer row y = screen row y + oy (rows outside: alpha 0) */
    hires_layer = (HiresLayer){ layer, W, H, 0, oy, 640, 480, 1 };
}
