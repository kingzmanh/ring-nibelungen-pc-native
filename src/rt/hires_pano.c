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
}

/* 412220(this; frame buffer, pitch in bytes): one frame of the view */
void N_pano_frame(void)
{
    if (pano_debug < 0) pano_debug = getenv(ENV("PANODEBUG")) != NULL;
    int n = opt_render < 1 ? 1 : opt_render > 4 ? 4 : opt_render;
    fb_base = ARG(0);
    fb_pitch = ARG(1);
    frame_n = fb_pitch == 1280 ? n : 1;
    if (frame_n > 1) {
        u32 src = RD32(cpu.r[R_ECX] + 4);   /* the node (this+4) - decoded again when it changed */
        if (tex && (src != tex_src || node_hash(src) != tex_hash)) tex_h = 0;
        if (hr_n != frame_n) { free(hr); hr = malloc((size_t)640 * frame_n * 480 * frame_n * 4); hr_n = frame_n; }
        if (!lo) { lo = malloc(640 * 480 * 2); lo_on = malloc(640 * 480); }
        memset(lo_on, 0, 640 * 480);
    }
    rt_wrap_orig("N_pano_frame", NULL)();
    if (frame_n > 1) frame_ms = now_ms();
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
    /* N x: screen position s = (k + 0.5) / N - 0.5 within the block, (u, v) bilinear in the corners */
    int W = 640 * n, S = 16 * n;
    double inv = 1.0 / (16.0 * n);
    for (int yy = 0; yy < S; yy++) {
        double ty = ((yy + 0.5) - 0.5 * n) * inv;
        double lu = cu[0] + (cu[2] - cu[0]) * ty, lv = cv[0] + (cv[2] - cv[0]) * ty;   /* left edge */
        double ru = cu[1] + (cu[3] - cu[1]) * ty, rv = cv[1] + (cv[3] - cv[1]) * ty;   /* right edge */
        double x0 = (0.5 - 0.5 * n) * inv;
        s32 u = (s32)(lu + (ru - lu) * x0), v = (s32)(lv + (rv - lv) * x0);
        s32 du = (s32)((ru - lu) * inv), dv = (s32)((rv - lv) * inv);
        u32 *o = hr + (size_t)(by * n + yy) * W + bx * n;
        for (int xx = 0; xx < S; xx++, u += du, v += dv) o[xx] = sample(u, v, vlo, vhi);
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
