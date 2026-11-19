/* gpu_vram_census.c — see gpu_vram_census.h. */
#include "gpu_vram_census.h"

#include <stdio.h>
#include <string.h>

#define CELL      16
#define CELLS_X   (1024 / CELL)
#define CELLS_Y   (512 / CELL)
#define MAX_AREAS 24
#define MAX_DISPS 8
/* A display rectangle stays "recent" for this many presents. */
#define DISP_RECENT 240

int g_vram_census_on;

enum { OWN_CPU = 0, OWN_GPU = 1 };

typedef struct { int x1, y1, x2, y2; uint64_t prims, tex_prims; int target; } Area;
typedef struct { int x, y, w, h; uint64_t presents, last; } Disp;
typedef struct { int sx, sy, dx, dy, w, h; uint64_t n; } Copy;
#define MAX_COPIES 12
static Copy s_copies[MAX_COPIES];
static int  s_n_copies;

static uint8_t  s_owner[CELLS_Y][CELLS_X];
static int      s_ax1, s_ay1, s_ax2, s_ay2, s_ox, s_oy, s_area_target;
static uint32_t s_twin;
static Area     s_areas[MAX_AREAS];
static int      s_n_areas, s_cur_area = -1;
static Disp     s_disps[MAX_DISPS];
static int      s_n_disps;
static uint64_t s_present_no;
static int      s_frame_had_readback, s_frame_had_upload;

static struct {
    uint64_t presents, present_gpu, present_cpu, present_mixed, present_depth24;
    uint64_t prims_gpu, prims_gpu_tex, prims_cpu, prims_cpu_tex;
    uint64_t fills_gpu, fills_cpu;
    uint64_t uploads, uploads_into_gpu, upload_into_gpu_cells;
    uint64_t copies, copy_gpu_to_gpu, copy_gpu_to_cpu, copy_cpu_to_gpu_target;
    uint64_t tex_from_gpu_by_gpu, tex_from_gpu_by_cpu;
    uint64_t cpu_draw_into_gpu, cpu_draw_into_gpu_cells;
    uint64_t gpuread, gpuread_from_gpu;
    uint64_t readback_cells, readback_events;
    uint64_t sync_up_cells;           /* CPU content a GPU draw had to upload */
    uint64_t frames_with_readback, frames_with_sync_up;
} S;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Cell range of a VRAM pixel rectangle (inclusive, no wrap). */
static int cell_span(int x1, int y1, int x2, int y2,
                     int *cx1, int *cy1, int *cx2, int *cy2) {
    if (x2 < x1 || y2 < y1) return 0;
    x1 = clampi(x1, 0, 1023); x2 = clampi(x2, 0, 1023);
    y1 = clampi(y1, 0, 511);  y2 = clampi(y2, 0, 511);
    *cx1 = x1 / CELL; *cx2 = x2 / CELL; *cy1 = y1 / CELL; *cy2 = y2 / CELL;
    return 1;
}

static int count_owned(int x1, int y1, int x2, int y2, int owner) {
    int cx1, cy1, cx2, cy2, n = 0;
    if (!cell_span(x1, y1, x2, y2, &cx1, &cy1, &cx2, &cy2)) return 0;
    for (int cy = cy1; cy <= cy2; cy++)
        for (int cx = cx1; cx <= cx2; cx++)
            n += s_owner[cy][cx] == owner;
    return n;
}

static int set_owner(int x1, int y1, int x2, int y2, int owner) {
    int cx1, cy1, cx2, cy2, changed = 0;
    if (!cell_span(x1, y1, x2, y2, &cx1, &cy1, &cx2, &cy2)) return 0;
    for (int cy = cy1; cy <= cy2; cy++)
        for (int cx = cx1; cx <= cx2; cx++) {
            changed += s_owner[cy][cx] != owner;
            s_owner[cy][cx] = (uint8_t)owner;
        }
    return changed;
}

/* The CPU needs these pixels: GPU-owned cells come back. */
static int readback(int x1, int y1, int x2, int y2) {
    int n = set_owner(x1, y1, x2, y2, OWN_CPU);
    if (n) { S.readback_cells += (uint64_t)n; S.readback_events++; s_frame_had_readback = 1; }
    return n;
}

static int is_display_target(int x1, int y1, int x2, int y2) {
    long area = (long)(x2 - x1 + 1) * (y2 - y1 + 1);
    if (area <= 0) return 0;
    for (int i = 0; i < s_n_disps; i++) {
        const Disp *d = &s_disps[i];
        if (s_present_no - d->last > DISP_RECENT) continue;
        int ix1 = x1 > d->x ? x1 : d->x, iy1 = y1 > d->y ? y1 : d->y;
        int ix2 = x2 < d->x + d->w - 1 ? x2 : d->x + d->w - 1;
        int iy2 = y2 < d->y + d->h - 1 ? y2 : d->y + d->h - 1;
        if (ix2 < ix1 || iy2 < iy1) continue;
        if ((long)(ix2 - ix1 + 1) * (iy2 - iy1 + 1) * 2 >= area) return 1;
    }
    return 0;
}

static void pick_area(void) {
    s_area_target = is_display_target(s_ax1, s_ay1, s_ax2, s_ay2);
    s_cur_area = -1;
    for (int i = 0; i < s_n_areas; i++)
        if (s_areas[i].x1 == s_ax1 && s_areas[i].y1 == s_ay1 &&
            s_areas[i].x2 == s_ax2 && s_areas[i].y2 == s_ay2) { s_cur_area = i; break; }
    if (s_cur_area < 0 && s_n_areas < MAX_AREAS) {
        s_cur_area = s_n_areas++;
        memset(&s_areas[s_cur_area], 0, sizeof(Area));
        s_areas[s_cur_area].x1 = s_ax1; s_areas[s_cur_area].y1 = s_ay1;
        s_areas[s_cur_area].x2 = s_ax2; s_areas[s_cur_area].y2 = s_ay2;
    }
    if (s_cur_area >= 0 && s_area_target) s_areas[s_cur_area].target = 1;
}

void vram_census_reset(int enable) {
    memset(&S, 0, sizeof(S));
    s_n_areas = 0; s_cur_area = -1; s_n_copies = 0;
    s_frame_had_readback = s_frame_had_upload = 0;
    for (int i = 0; i < s_n_disps; i++) s_disps[i].presents = 0;
    if (enable && !g_vram_census_on) {
        /* Fresh start: everything is CPU VRAM, no displays known yet. */
        memset(s_owner, 0, sizeof(s_owner));
        s_n_disps = 0;
    }
    g_vram_census_on = enable;
    if (enable) pick_area();
}

void vram_census_draw_area(int x1, int y1, int x2, int y2) {
    s_ax1 = x1; s_ay1 = y1; s_ax2 = x2; s_ay2 = y2;
    pick_area();
}
void vram_census_draw_offset(int x, int y) { s_ox = x; s_oy = y; }
void vram_census_texture_window(uint32_t raw) { s_twin = raw; }

static void tex_range(int lo, int hi, int mask, int off, int *out_lo, int *out_hi) {
    if (!mask) { *out_lo = lo; *out_hi = hi; return; }
    int mn = 255, mx = 0;
    for (int t = lo; t <= hi; t++) {
        int v = (t & ~(mask * 8)) | ((off & mask) * 8);
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    *out_lo = mn; *out_hi = mx;
}

/* Texture page + CLUT pixels a primitive reads; applies f to each rect. */
static int texture_touches_gpu(int umin, int vmin, int umax, int vmax,
                               uint16_t cx, uint16_t cy, uint16_t tp, int do_readback) {
    int u0, u1, v0, v1;
    umin = clampi(umin, 0, 255); umax = clampi(umax, 0, 255);
    vmin = clampi(vmin, 0, 255); vmax = clampi(vmax, 0, 255);
    tex_range(umin, umax, (int)(s_twin & 31), (int)((s_twin >> 10) & 31), &u0, &u1);
    tex_range(vmin, vmax, (int)((s_twin >> 5) & 31), (int)((s_twin >> 15) & 31), &v0, &v1);
    int tpx = (tp & 0xF) * 64, tpy = ((tp >> 4) & 1) * 256, depth = (tp >> 7) & 3;
    int x1, x2, clut_w = 0;
    if (depth == 0)      { x1 = tpx + u0 / 4; x2 = tpx + u1 / 4; clut_w = 16; }
    else if (depth == 1) { x1 = tpx + u0 / 2; x2 = tpx + u1 / 2; clut_w = 256; }
    else                 { x1 = tpx + u0;     x2 = tpx + u1; }
    int hit = count_owned(x1, tpy + v0, x2, tpy + v1, OWN_GPU);
    if (clut_w) hit += count_owned(cx, cy, cx + clut_w - 1, cy, OWN_GPU);
    if (hit && do_readback) {
        readback(x1, tpy + v0, x2, tpy + v1);
        if (clut_w) readback(cx, cy, cx + clut_w - 1, cy);
    }
    return hit;
}

void vram_census_prim(int xmin, int ymin, int xmax, int ymax, int textured,
                      int umin, int vmin, int umax, int vmax,
                      uint16_t clut_x, uint16_t clut_y, uint16_t texpage,
                      int raw_texture) {
    (void)raw_texture;
    /* gr_* coordinates already include the draw offset. */
    int x1 = xmin, y1 = ymin, x2 = xmax, y2 = ymax;
    if (x1 < s_ax1) x1 = s_ax1;
    if (y1 < s_ay1) y1 = s_ay1;
    if (x2 > s_ax2) x2 = s_ax2;
    if (y2 > s_ay2) y2 = s_ay2;
    if (s_cur_area >= 0) {
        s_areas[s_cur_area].prims++;
        if (textured) s_areas[s_cur_area].tex_prims++;
    }
    /* Sampling first: a texture drawn earlier on the GPU must come back to
     * the CPU decode cache whichever side draws this primitive. */
    if (textured && texture_touches_gpu(umin, vmin, umax, vmax, clut_x, clut_y,
                                        texpage, 1)) {
        if (s_area_target) S.tex_from_gpu_by_gpu++; else S.tex_from_gpu_by_cpu++;
    }
    if (s_area_target) {
        S.prims_gpu++;
        if (textured) S.prims_gpu_tex++;
        if (x2 < x1 || y2 < y1) return;
        int up = count_owned(x1, y1, x2, y2, OWN_CPU);
        if (up) { S.sync_up_cells += (uint64_t)up; s_frame_had_upload = 1; }
        set_owner(x1, y1, x2, y2, OWN_GPU);
    } else {
        S.prims_cpu++;
        if (textured) S.prims_cpu_tex++;
        if (x2 < x1 || y2 < y1) return;
        int n = readback(x1, y1, x2, y2);
        if (n) { S.cpu_draw_into_gpu++; S.cpu_draw_into_gpu_cells += (uint64_t)n; }
    }
}

void vram_census_fill(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    /* A fill is a GPU clear when it lands in a display target. */
    if (is_display_target(x, y, x + w - 1, y + h - 1)) {
        S.fills_gpu++;
        set_owner(x, y, x + w - 1, y + h - 1, OWN_GPU);
    } else {
        S.fills_cpu++;
        set_owner(x, y, x + w - 1, y + h - 1, OWN_CPU);
    }
}

void vram_census_copy(int sx, int sy, int dx, int dy, int w, int h) {
    if (w <= 0 || h <= 0) return;
    S.copies++;
    {
        int i;
        for (i = 0; i < s_n_copies; i++)
            if (s_copies[i].sx == sx && s_copies[i].sy == sy && s_copies[i].dx == dx &&
                s_copies[i].dy == dy && s_copies[i].w == w && s_copies[i].h == h) break;
        if (i == s_n_copies && s_n_copies < MAX_COPIES) {
            Copy c = { sx, sy, dx, dy, w, h, 0 };
            s_copies[s_n_copies++] = c;
        }
        if (i < s_n_copies) s_copies[i].n++;
    }
    int src_gpu = count_owned(sx, sy, sx + w - 1, sy + h - 1, OWN_GPU);
    int dst_target = is_display_target(dx, dy, dx + w - 1, dy + h - 1);
    if (src_gpu && dst_target) {
        S.copy_gpu_to_gpu++;                 /* GPU-side blit */
        set_owner(dx, dy, dx + w - 1, dy + h - 1, OWN_GPU);
        return;
    }
    if (src_gpu) { S.copy_gpu_to_cpu++; readback(sx, sy, sx + w - 1, sy + h - 1); }
    if (dst_target) S.copy_cpu_to_gpu_target++;
    set_owner(dx, dy, dx + w - 1, dy + h - 1, OWN_CPU);
}

void vram_census_upload(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    S.uploads++;
    int n = count_owned(x, y, x + w - 1, y + h - 1, OWN_GPU);
    if (n) { S.uploads_into_gpu++; S.upload_into_gpu_cells += (uint64_t)n; }
    set_owner(x, y, x + w - 1, y + h - 1, OWN_CPU);
}

void vram_census_cpu_read(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    S.gpuread++;
    if (readback(x, y, x + w - 1, y + h - 1)) S.gpuread_from_gpu++;
}

void vram_census_present(int x, int y, int w, int h, int depth24) {
    s_present_no++;
    S.presents++;
    if (s_frame_had_readback) S.frames_with_readback++;
    if (s_frame_had_upload) S.frames_with_sync_up++;
    s_frame_had_readback = s_frame_had_upload = 0;
    if (depth24) S.present_depth24++;
    int pw = depth24 ? (w * 3 + 1) / 2 : w;
    int gpu = count_owned(x, y, x + pw - 1, y + h - 1, OWN_GPU);
    int cpu = count_owned(x, y, x + pw - 1, y + h - 1, OWN_CPU);
    if (gpu && !cpu) S.present_gpu++;
    else if (cpu && !gpu) S.present_cpu++;
    else S.present_mixed++;
    int i;
    for (i = 0; i < s_n_disps; i++)
        if (s_disps[i].x == x && s_disps[i].y == y &&
            s_disps[i].w == pw && s_disps[i].h == h) break;
    if (i == s_n_disps) {
        if (s_n_disps < MAX_DISPS) s_n_disps++;
        else {                                  /* replace the stalest */
            i = 0;
            for (int j = 1; j < MAX_DISPS; j++)
                if (s_disps[j].last < s_disps[i].last) i = j;
        }
        s_disps[i].x = x; s_disps[i].y = y; s_disps[i].w = pw; s_disps[i].h = h;
        s_disps[i].presents = 0;
        pick_area();                            /* targets may have changed */
    }
    s_disps[i].presents++;
    s_disps[i].last = s_present_no;
}

#define F(name) "\"" #name "\":%llu"
#define V(name) (unsigned long long)S.name

int vram_census_json(char *buf, size_t cap) {
    int n = snprintf(buf, cap,
        "{\"on\":%d," F(presents) "," F(present_gpu) "," F(present_cpu) ","
        F(present_mixed) "," F(present_depth24) "," F(prims_gpu) ","
        F(prims_gpu_tex) "," F(prims_cpu) "," F(prims_cpu_tex) ","
        F(fills_gpu) "," F(fills_cpu) "," F(uploads) "," F(uploads_into_gpu) ","
        F(upload_into_gpu_cells) "," F(copies) "," F(copy_gpu_to_gpu) ","
        F(copy_gpu_to_cpu) "," F(copy_cpu_to_gpu_target) ","
        F(tex_from_gpu_by_gpu) "," F(tex_from_gpu_by_cpu) ","
        F(cpu_draw_into_gpu) "," F(cpu_draw_into_gpu_cells) ","
        F(gpuread) "," F(gpuread_from_gpu) "," F(readback_cells) ","
        F(readback_events) "," F(sync_up_cells) "," F(frames_with_readback) ","
        F(frames_with_sync_up),
        g_vram_census_on, V(presents), V(present_gpu), V(present_cpu),
        V(present_mixed), V(present_depth24), V(prims_gpu), V(prims_gpu_tex),
        V(prims_cpu), V(prims_cpu_tex), V(fills_gpu), V(fills_cpu), V(uploads),
        V(uploads_into_gpu), V(upload_into_gpu_cells), V(copies),
        V(copy_gpu_to_gpu), V(copy_gpu_to_cpu), V(copy_cpu_to_gpu_target),
        V(tex_from_gpu_by_gpu), V(tex_from_gpu_by_cpu), V(cpu_draw_into_gpu),
        V(cpu_draw_into_gpu_cells), V(gpuread), V(gpuread_from_gpu),
        V(readback_cells), V(readback_events), V(sync_up_cells),
        V(frames_with_readback), V(frames_with_sync_up));
    if (n < 0 || (size_t)n >= cap) return n < 0 ? 0 : (int)cap - 1;
    n += snprintf(buf + n, cap - (size_t)n, ",\"areas\":[");
    for (int i = 0; i < s_n_areas && (size_t)n < cap; i++)
        n += snprintf(buf + n, cap - (size_t)n,
                      "%s{\"x1\":%d,\"y1\":%d,\"x2\":%d,\"y2\":%d,\"prims\":%llu,"
                      "\"tex\":%llu,\"target\":%d}", i ? "," : "",
                      s_areas[i].x1, s_areas[i].y1, s_areas[i].x2, s_areas[i].y2,
                      (unsigned long long)s_areas[i].prims,
                      (unsigned long long)s_areas[i].tex_prims, s_areas[i].target);
    if ((size_t)n < cap) n += snprintf(buf + n, cap - (size_t)n, "],\"displays\":[");
    for (int i = 0; i < s_n_disps && (size_t)n < cap; i++)
        n += snprintf(buf + n, cap - (size_t)n,
                      "%s{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"presents\":%llu}",
                      i ? "," : "", s_disps[i].x, s_disps[i].y, s_disps[i].w,
                      s_disps[i].h, (unsigned long long)s_disps[i].presents);
    if ((size_t)n < cap) n += snprintf(buf + n, cap - (size_t)n, "],\"copy_rects\":[");
    for (int i = 0; i < s_n_copies && (size_t)n < cap; i++)
        n += snprintf(buf + n, cap - (size_t)n,
                      "%s{\"sx\":%d,\"sy\":%d,\"dx\":%d,\"dy\":%d,\"w\":%d,\"h\":%d,\"n\":%llu}",
                      i ? "," : "", s_copies[i].sx, s_copies[i].sy, s_copies[i].dx,
                      s_copies[i].dy, s_copies[i].w, s_copies[i].h,
                      (unsigned long long)s_copies[i].n);
    if ((size_t)n < cap) n += snprintf(buf + n, cap - (size_t)n, "]}");
    if ((size_t)n >= cap) n = (int)cap - 1;
    return n;
}
