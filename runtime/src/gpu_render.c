/* gpu_render.c — renderer facade / backend dispatch.
 *
 * gpu.c and main.cpp call the gr_* functions; this file forwards each to the
 * selected backend's vtable.  The software rasterizer is the default and the
 * fallback.  The OpenGL backend (gpu_gl_renderer.c) is selected via
 * gr_set_backend(GR_BACKEND_OPENGL) and supplies its table through
 * gl_backend_get(); if that returns NULL (GL unavailable / init failed) we
 * stay on software so a misconfigured [video] renderer never bricks boot.
 *
 * The software backend table points straight at the existing sw_* functions,
 * so the software path is byte-for-byte unchanged. */

#include "gpu_render.h"
#include "gpu_sw_renderer.h"
#include "gpu_hd_textures.h"
#include "gpu_uv.h"
#include "gpu_vram_census.h"
#include <stdio.h>
#include <stdlib.h>

static const GpuRenderBackend SW_BACKEND = {
    .name                          = "software",
    .init                          = sw_renderer_init,
    .set_scale                     = sw_renderer_set_scale,
    .scale                         = sw_renderer_scale,
    .set_texture_filter            = sw_set_texture_filter,
    .texture_filter                = sw_texture_filter,
    .set_semi_transparency         = sw_set_semi_transparency,
    .set_mask_bits                 = sw_set_mask_bits,
    .set_texture_window            = sw_set_texture_window,
    .set_color_modulation          = sw_set_color_modulation,
    .set_precise_triangle          = sw_set_precise_triangle,
    .set_perspective_triangle      = sw_set_perspective_triangle,
    .fill_rect                     = sw_fill_rect,
    .copy_rect                     = sw_copy_rect,
    .draw_flat_triangle            = sw_draw_flat_triangle,
    .draw_gouraud_triangle         = sw_draw_gouraud_triangle,
    .draw_textured_triangle        = sw_draw_textured_triangle,
    .draw_shaded_textured_triangle = sw_draw_shaded_textured_triangle,
    .draw_flat_rect                = sw_draw_flat_rect,
    .draw_textured_rect            = sw_draw_textured_rect,
    .draw_textured_rect_scaled     = sw_draw_textured_rect_scaled,
    .draw_line                     = sw_draw_line,
    .draw_shaded_line              = sw_draw_shaded_line,
    .render_display                = sw_render_display,
    .render_display_hires          = sw_render_display_hires,
    .vram_write                    = sw_vram_write,
    .vram_read                     = sw_vram_read,
    .vram_transfer_in              = sw_vram_transfer_in,
    .vram_transfer_out             = sw_vram_transfer_out,
    .set_draw_area                 = sw_set_draw_area,
    .get_draw_area                 = sw_get_draw_area,
    .set_draw_offset               = sw_set_draw_offset,
    .wide_configure                = sw_wide_configure,
    .wide_set_view                 = sw_wide_set_view,
    .wide_set_target               = sw_wide_set_target,
    .wide_disable_target           = sw_wide_disable_target,
    .wide_clear                    = sw_wide_clear,
    .wide_clear_margins            = sw_wide_clear_margins,
    .render_wide_display           = sw_render_wide_display,
    .wide_dump_full                = sw_wide_dump_full,
    .wide_feedback_rect            = sw_wide_feedback_rect,
};

/* Supplied by gpu_gl_renderer.c; returns NULL until the GL backend is ready. */
extern const GpuRenderBackend *gl_backend_get(void);
/* Supplied by gpu_vk_renderer.c; returns NULL until the Vulkan backend is
 * ready (init succeeded). When Vulkan isn't compiled in it returns NULL. */
extern const GpuRenderBackend *vk_backend_get(void);

static const GpuRenderBackend *g_b         = &SW_BACKEND;
static GrBackend               g_effective = GR_BACKEND_SOFTWARE;
static int g_hd_offset_x, g_hd_offset_y;
static uint32_t g_hd_texture_window;
static int g_hd_semi, g_hd_perspective;
static void hd_observe_triangle(int x0,int y0,int u0,int v0,int x1,int y1,int u1,int v1,
    int x2,int y2,int u2,int v2,uint16_t cx,uint16_t cy,uint16_t tp) {
    if (g_effective == GR_BACKEND_OPENGL || !gpu_hd_textures_dump_enabled()) return;
    if (g_effective == GR_BACKEND_VULKAN) g_b->vram_read(0,0);
    int limits[4], xs[3]={x0,x1,x2},ys[3]={y0,y1,y2},us[3]={u0,u1,u2},vs[3]={v0,v1,v2};
    if (g_hd_perspective) psx_uv_tri_world_limits(us,vs,limits);
    else psx_uv_tri_limits(xs,ys,us,vs,limits);
    gpu_hd_textures_observe_draw(tp,cx,cy,limits,g_hd_texture_window,g_hd_semi);
}
static void hd_observe_rect(int u0,int v0,int u1,int v1,uint16_t cx,uint16_t cy,uint16_t tp) {
    if (g_effective == GR_BACKEND_OPENGL || !gpu_hd_textures_dump_enabled()) return;
    if (g_effective == GR_BACKEND_VULKAN) g_b->vram_read(0,0);
    int limits[4]; psx_uv_rect_limits(u0,v0,u1,v1,limits);
    gpu_hd_textures_observe_draw(tp,cx,cy,limits,g_hd_texture_window,g_hd_semi);
}

/* A backend that replays commands on another thread (the GL render thread)
 * owns its VRAM copy there, so residency changes travel with its commands. */
static void hd_note(int op, int x, int y, int w, int h, const uint16_t *words) {
    if (!gpu_hd_textures_active()) return;
    if (g_b->hd_texture_note) { g_b->hd_texture_note(op, x, y, w, h, 0, 0); return; }
    if (op == GR_HD_NOTE_TRACK_UPLOAD) gpu_hd_textures_track_upload(x, y, w, h, words);
    else if (op == GR_HD_NOTE_BEGIN_UPLOAD) gpu_hd_textures_begin_upload(x, y, w, h);
    else gpu_hd_textures_invalidate(x, y, w, h);
}

/* Residency describes guest native words, so any draw/copy which can touch
 * an upload ends its identity. Run after submission: an overlapping texture
 * draw must resolve its source before the destination invalidates it. */
static void hd_invalidate_draw(int x0, int y0, int x1, int y1) {
    int ax0, ay0, ax1, ay1;
    if (!gpu_hd_textures_active()) return;
    g_b->get_draw_area(&ax0, &ay0, &ax1, &ay1);
    x0 += g_hd_offset_x; x1 += g_hd_offset_x;
    y0 += g_hd_offset_y; y1 += g_hd_offset_y;
    if (x0 < ax0) x0 = ax0; if (x1 > ax1) x1 = ax1;
    if (y0 < ay0) y0 = ay0; if (y1 > ay1) y1 = ay1;
    if (x0 <= x1 && y0 <= y1)
        hd_note(GR_HD_NOTE_INVALIDATE, x0, y0, x1 - x0 + 1, y1 - y0 + 1, NULL);
}
#define CEN (g_vram_census_on)
static inline int min3(int a, int b, int c) { int m = a < b ? a : b; return m < c ? m : c; }
static inline int max3(int a, int b, int c) { int m = a > b ? a : b; return m > c ? m : c; }
static void census_tri(int x0, int y0, int x1, int y1, int x2, int y2, int tex,
                       int u0, int v0, int u1, int v1, int u2, int v2,
                       uint16_t cx, uint16_t cy, uint16_t tp, int raw) {
    vram_census_prim(min3(x0, x1, x2), min3(y0, y1, y2), max3(x0, x1, x2), max3(y0, y1, y2),
                     tex, min3(u0, u1, u2), min3(v0, v1, v2), max3(u0, u1, u2), max3(v0, v1, v2),
                     cx, cy, tp, raw);
}
static void hd_invalidate_triangle(int x0, int y0, int x1, int y1, int x2, int y2) {
    int xmin = x0, xmax = x0, ymin = y0, ymax = y0;
    if (x1 < xmin) xmin = x1; if (x2 < xmin) xmin = x2;
    if (x1 > xmax) xmax = x1; if (x2 > xmax) xmax = x2;
    if (y1 < ymin) ymin = y1; if (y2 < ymin) ymin = y2;
    if (y1 > ymax) ymax = y1; if (y2 > ymax) ymax = y2;
    hd_invalidate_draw(xmin, ymin, xmax, ymax);
}

void gr_set_backend(GrBackend backend) {
    if (backend == GR_BACKEND_OPENGL) {
        const GpuRenderBackend *gl = gl_backend_get();
        if (gl) {
            g_b = gl;
            g_effective = GR_BACKEND_OPENGL;
            fprintf(stdout, "psxrecomp: renderer = opengl (%s)\n", gl->name);
            return;
        }
        fprintf(stdout, "psxrecomp: renderer = opengl requested but unavailable "
                        "— falling back to software\n");
    } else if (backend == GR_BACKEND_VULKAN) {
        const GpuRenderBackend *vk = vk_backend_get();
        if (vk) {
            g_b = vk;
            g_effective = GR_BACKEND_VULKAN;
            fprintf(stdout, "psxrecomp: renderer = vulkan (%s)\n", vk->name);
            return;
        }
        fprintf(stdout, "psxrecomp: renderer = vulkan requested but unavailable "
                        "— falling back to software\n");
    }
    g_b = &SW_BACKEND;
    g_effective = GR_BACKEND_SOFTWARE;
}

GrBackend gr_backend(void) { return g_effective; }

/* Re-fetch the OpenGL table after the render thread starts or stops
 * (gl_backend_get then returns the recording table, or the direct one). The
 * effective backend does not change. */
void gr_refresh_backend(void) {
    if (g_effective == GR_BACKEND_OPENGL) {
        const GpuRenderBackend *gl = gl_backend_get();
        if (gl) g_b = gl;
    }
}

/* ---- Host tap -------------------------------------------------------------
 * A platform may observe every renderer call after the active backend has
 * run it (gr_set_host_tap). The 3DS port uses it to drive its GPU renderer
 * alongside the software one. NULL members are skipped. */
static const GpuRenderBackend *g_tap;
void gr_set_host_tap(const GpuRenderBackend *tap) { g_tap = tap; }
#define TAP(fn, ...) do { if (g_tap && g_tap->fn) g_tap->fn(__VA_ARGS__); } while (0)
static int (*g_host_owns)(void);
void gr_set_host_draw_owner(int (*owns)(void)) { g_host_owns = owns; }
/* The backend rasterises this primitive unless the host GPU owns it. */
#define BACKEND_DRAWS() (!g_host_owns || !g_host_owns())

/* PSX_GR_SKIP_TEXTURED=1 (diagnostic): textured primitives are dropped before
 * any backend or tap sees them, so untextured rendering can be compared
 * between renderers. Changes what the game shows; never for play. */
/* PSX_GR_TEXTURED_AS_SHADES=1 (diagnostic): textured primitives are redrawn
 * as untextured gouraud shapes, coloured per texture page/CLUT and lit by
 * their vertex colours, through the same entry points. Every renderer then
 * draws the game's full geometry, overlaps and blending without textures. */
static int g_skip_textured = -1, g_as_shades;
static int skip_textured(void) {
    if (g_skip_textured < 0) {
        const char *e = getenv("PSX_GR_SKIP_TEXTURED");
        const char *s = getenv("PSX_GR_TEXTURED_AS_SHADES");
        g_as_shades = s && s[0] == '1';
        g_skip_textured = (e && e[0] == '1') || g_as_shades;
    }
    return g_skip_textured;
}
static uint16_t shade_of(uint16_t tp, uint16_t cx, uint16_t cy, uint32_t c24) {
    uint32_t h = (uint32_t)tp * 2654435761u ^ (uint32_t)cx * 40503u ^ (uint32_t)cy * 97u;
    int base[3] = { 8 + (int)(h & 15), 8 + (int)((h >> 4) & 15), 8 + (int)((h >> 8) & 15) };
    int out = 0;
    for (int i = 0; i < 3; i++) {
        int v = base[i] * (int)((c24 >> (8 * i)) & 0xFF) / 128;
        out |= (v > 31 ? 31 : v) << (5 * i);
    }
    return (uint16_t)out;
}

/* ---- Dispatch wrappers (one line each; forward to the active backend) ---- */
void gr_init(uint16_t *vram)                         { gpu_hd_textures_set_vram(vram); g_b->init(vram); g_hd_offset_x = g_hd_offset_y = 0; g_hd_texture_window = 0; g_hd_semi = g_hd_perspective = 0; }
void gr_set_scale(int scale)                         { g_b->set_scale(scale); }
int  gr_scale(void)                                  { return g_b->scale(); }
void gr_set_texture_filter(int bilinear)             { g_b->set_texture_filter(bilinear); }
int  gr_texture_filter(void)                         { return g_b->texture_filter(); }
void gr_set_semi_transparency(int e, int m)          { g_b->set_semi_transparency(e, m); g_hd_semi=e; TAP(set_semi_transparency, e, m); }
void gr_set_mask_bits(int s, int c)                  { g_b->set_mask_bits(s, c); TAP(set_mask_bits, s, c); }
void gr_set_texture_window(uint32_t raw)             { vram_census_texture_window(raw); g_b->set_texture_window(raw); g_hd_texture_window=raw; TAP(set_texture_window, raw); }
void gr_set_color_modulation(int r, int g, int b, int raw) { g_b->set_color_modulation(r, g, b, raw); TAP(set_color_modulation, r, g, b, raw); }
void gr_set_precise_triangle(int enabled, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                             int32_t x2, int32_t y2) {
    if (g_b->set_precise_triangle)
        g_b->set_precise_triangle(enabled, x0, y0, x1, y1, x2, y2);
}
void gr_set_perspective_triangle(int enabled, float q0, float q1, float q2) {
    g_hd_perspective = enabled && q0 > 0 && q1 > 0 && q2 > 0;
    if (g_b->set_perspective_triangle)
        g_b->set_perspective_triangle(enabled, q0, q1, q2);
}
void gr_set_dither(int enabled) {
    if (g_b->set_dither) g_b->set_dither(enabled);
    TAP(set_dither, enabled);
}
void gr_set_depth_triangle(int enabled, float z0, float z1, float z2) {
    if (g_b->set_depth_triangle)
        g_b->set_depth_triangle(enabled, z0, z1, z2);
}
void gr_fill_rect(int x, int y, int w, int h, uint16_t c)  { if (CEN) vram_census_fill(x, y, w, h); g_b->fill_rect(x, y, w, h, c); hd_note(GR_HD_NOTE_INVALIDATE, x, y, w, h, NULL); TAP(fill_rect, x, y, w, h, c); }
static void gr_copy_rect_body(int sx, int sy, int dx, int dy, int w, int h);
void gr_copy_rect(int sx, int sy, int dx, int dy, int w, int h) {
    gr_copy_rect_body(sx, sy, dx, dy, w, h);
    TAP(copy_rect, sx, sy, dx, dy, w, h);
}
static void gr_copy_rect_body(int sx, int sy, int dx, int dy, int w, int h) {
    if (CEN) vram_census_copy(sx, sy, dx, dy, w, h);
    if (!gpu_hd_textures_active()) { g_b->copy_rect(sx, sy, dx, dy, w, h); return; }
    if (g_b->hd_texture_note) {
        /* Pre/post copy observations against the backend's own VRAM. */
        g_b->hd_texture_note(GR_HD_NOTE_BEGIN_COPY, dx, dy, w, h, sx, sy);
        g_b->copy_rect(sx, sy, dx, dy, w, h);
        g_b->hd_texture_note(GR_HD_NOTE_END_COPY, 0, 0, 0, 0, 0, 0);
        return;
    }
    if (g_effective == GR_BACKEND_VULKAN) g_b->vram_read(0,0);
    gpu_hd_textures_begin_copy(sx,sy,dx,dy,w,h);
    g_b->copy_rect(sx, sy, dx, dy, w, h);
    if (g_effective == GR_BACKEND_VULKAN) g_b->vram_read(0,0);
    gpu_hd_textures_end_copy();
}
void gr_draw_flat_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c) {
    if (CEN) census_tri(x0, y0, x1, y1, x2, y2, 0, 0,0,0,0,0,0, 0,0,0,0);
    if (BACKEND_DRAWS()) g_b->draw_flat_triangle(x0, y0, x1, y1, x2, y2, c);
    TAP(draw_flat_triangle, x0, y0, x1, y1, x2, y2, c);
    hd_invalidate_triangle(x0, y0, x1, y1, x2, y2);
    g_hd_perspective = 0;
}
void gr_draw_gouraud_triangle(int x0, int y0, uint16_t c0, int x1, int y1, uint16_t c1,
                              int x2, int y2, uint16_t c2) {
    if (CEN) census_tri(x0, y0, x1, y1, x2, y2, 0, 0,0,0,0,0,0, 0,0,0,0);
    if (BACKEND_DRAWS()) g_b->draw_gouraud_triangle(x0, y0, c0, x1, y1, c1, x2, y2, c2);
    TAP(draw_gouraud_triangle, x0, y0, c0, x1, y1, c1, x2, y2, c2);
    hd_invalidate_triangle(x0, y0, x1, y1, x2, y2);
    g_hd_perspective = 0;
}
void gr_draw_textured_triangle(int x0, int y0, int u0, int v0, int x1, int y1, int u1, int v1,
                               int x2, int y2, int u2, int v2,
                               uint16_t clut_x, uint16_t clut_y, uint16_t texpage) {
    if (skip_textured()) {
        if (g_as_shades) {
            const uint16_t c = shade_of(texpage, clut_x, clut_y, 0x808080u);
            gr_draw_gouraud_triangle(x0, y0, c, x1, y1, c, x2, y2, c);
        }
        return;
    }
    hd_observe_triangle(x0,y0,u0,v0,x1,y1,u1,v1,x2,y2,u2,v2,clut_x,clut_y,texpage);
    if (CEN) census_tri(x0, y0, x1, y1, x2, y2, 1, u0, v0, u1, v1, u2, v2, clut_x, clut_y, texpage, 0);
    if (BACKEND_DRAWS())
        g_b->draw_textured_triangle(x0, y0, u0, v0, x1, y1, u1, v1, x2, y2, u2, v2,
                                    clut_x, clut_y, texpage);
    TAP(draw_textured_triangle, x0, y0, u0, v0, x1, y1, u1, v1, x2, y2, u2, v2,
        clut_x, clut_y, texpage);
    hd_invalidate_triangle(x0, y0, x1, y1, x2, y2);
    g_hd_perspective = 0;
}
void gr_draw_shaded_textured_triangle(int x0, int y0, int u0, int v0, uint32_t c0,
                                      int x1, int y1, int u1, int v1, uint32_t c1,
                                      int x2, int y2, int u2, int v2, uint32_t c2,
                                      uint16_t clut_x, uint16_t clut_y,
                                      uint16_t texpage, int raw) {
    if (skip_textured()) {
        if (g_as_shades) {
            const uint32_t n = 0x808080u;
            gr_draw_gouraud_triangle(x0, y0, shade_of(texpage, clut_x, clut_y, raw ? n : c0),
                                     x1, y1, shade_of(texpage, clut_x, clut_y, raw ? n : c1),
                                     x2, y2, shade_of(texpage, clut_x, clut_y, raw ? n : c2));
        }
        return;
    }
    hd_observe_triangle(x0,y0,u0,v0,x1,y1,u1,v1,x2,y2,u2,v2,clut_x,clut_y,texpage);
    if (CEN) census_tri(x0, y0, x1, y1, x2, y2, 1, u0, v0, u1, v1, u2, v2, clut_x, clut_y, texpage, raw);
    if (BACKEND_DRAWS())
        g_b->draw_shaded_textured_triangle(x0, y0, u0, v0, c0, x1, y1, u1, v1, c1,
                                           x2, y2, u2, v2, c2, clut_x, clut_y, texpage, raw);
    TAP(draw_shaded_textured_triangle, x0, y0, u0, v0, c0, x1, y1, u1, v1, c1,
        x2, y2, u2, v2, c2, clut_x, clut_y, texpage, raw);
    hd_invalidate_triangle(x0, y0, x1, y1, x2, y2);
    g_hd_perspective = 0;
}
void gr_draw_flat_rect(int x, int y, int w, int h, uint16_t c) { TAP(draw_flat_rect, x, y, w, h, c); if (CEN && w > 0 && h > 0) vram_census_prim(x, y, x+w-1, y+h-1, 0, 0,0,0,0, 0,0,0, 0); if (BACKEND_DRAWS()) g_b->draw_flat_rect(x, y, w, h, c); if (w > 0 && h > 0) hd_invalidate_draw(x,y,x+w-1,y+h-1); }
void gr_draw_textured_rect(int x, int y, int w, int h, int u, int v,
                           uint16_t clut_x, uint16_t clut_y, uint16_t texpage) {
    if (skip_textured()) {
        if (g_as_shades) gr_draw_flat_rect(x, y, w, h, shade_of(texpage, clut_x, clut_y, 0x808080u));
        return;
    }
    TAP(draw_textured_rect, x, y, w, h, u, v, clut_x, clut_y, texpage);
    hd_observe_rect(u,v,u+w,v+h,clut_x,clut_y,texpage);
    if (CEN && w > 0 && h > 0) vram_census_prim(x, y, x+w-1, y+h-1, 1, u, v, u+w-1, v+h-1, clut_x, clut_y, texpage, 0);
    if (BACKEND_DRAWS()) g_b->draw_textured_rect(x, y, w, h, u, v, clut_x, clut_y, texpage);
    if (w > 0 && h > 0) hd_invalidate_draw(x,y,x+w-1,y+h-1);
}
void gr_draw_textured_rect_scaled(int x, int y, int w, int h, int u0, int v0, int u1, int v1,
                                  uint16_t clut_x, uint16_t clut_y, uint16_t texpage) {
    if (skip_textured()) {
        if (g_as_shades) gr_draw_flat_rect(x, y, w, h, shade_of(texpage, clut_x, clut_y, 0x808080u));
        return;
    }
    TAP(draw_textured_rect_scaled, x, y, w, h, u0, v0, u1, v1, clut_x, clut_y, texpage);
    hd_observe_rect(u0,v0,u1,v1,clut_x,clut_y,texpage);
    if (CEN && w > 0 && h > 0) vram_census_prim(x, y, x+w-1, y+h-1, 1, u0 < u1 ? u0 : u1, v0 < v1 ? v0 : v1, u0 < u1 ? u1 : u0, v0 < v1 ? v1 : v0, clut_x, clut_y, texpage, 0);
    if (BACKEND_DRAWS()) g_b->draw_textured_rect_scaled(x, y, w, h, u0, v0, u1, v1, clut_x, clut_y, texpage);
    if (w > 0 && h > 0) hd_invalidate_draw(x,y,x+w-1,y+h-1);
}
void gr_draw_line(int x0, int y0, int x1, int y1, uint16_t c) { TAP(draw_line, x0, y0, x1, y1, c); if (CEN) census_tri(x0, y0, x1, y1, x1, y1, 0, 0,0,0,0,0,0, 0,0,0,0); g_b->draw_line(x0, y0, x1, y1, c); hd_invalidate_triangle(x0,y0,x1,y1,x1,y1); }
void gr_draw_shaded_line(int x0, int y0, uint16_t c0, int x1, int y1, uint16_t c1) {
    if (CEN) census_tri(x0, y0, x1, y1, x1, y1, 0, 0,0,0,0,0,0, 0,0,0,0);
    g_b->draw_shaded_line(x0, y0, c0, x1, y1, c1);
    TAP(draw_shaded_line, x0, y0, c0, x1, y1, c1);
    hd_invalidate_triangle(x0,y0,x1,y1,x1,y1);
}
int gr_render_display(uint32_t *o, int p, int dx, int dy, int dw, int dh) {
    return g_b->render_display(o, p, dx, dy, dw, dh);
}
int gr_render_display_hires(uint32_t *o, int p, int dx, int dy, int dw, int dh) {
    return g_b->render_display_hires(o, p, dx, dy, dw, dh);
}
void gr_vram_upload_begin(int x, int y, int w, int h) {
    if (g_b->vram_upload_open) g_b->vram_upload_open(1);
    hd_note(GR_HD_NOTE_BEGIN_UPLOAD, x, y, w, h, NULL);
}
void gr_vram_upload_commit(int x, int y, int w, int h, const uint16_t *d) {
    gr_vram_transfer_in(x, y, w, h, d);
    if (g_b->vram_upload_open) g_b->vram_upload_open(0);
}
void gr_vram_upload_set_open(int open) { if (g_b->vram_upload_open) g_b->vram_upload_open(open ? 1 : 0); }
void gr_vram_write(int x, int y, uint16_t pixel)     { g_b->vram_write(x, y, pixel); hd_note(GR_HD_NOTE_INVALIDATE, x, y, 1, 1, NULL); }
uint16_t gr_vram_read(int x, int y)                  { return g_b->vram_read(x, y); }
void gr_vram_transfer_in(int x, int y, int w, int h, const uint16_t *d)  { TAP(vram_transfer_in, x, y, w, h, d); if (CEN) vram_census_upload(x, y, w, h); g_b->vram_transfer_in(x, y, w, h, d); hd_note(GR_HD_NOTE_TRACK_UPLOAD, x, y, w, h, d); }
void gr_vram_transfer_out(int x, int y, int w, int h, uint16_t *d)       { g_b->vram_transfer_out(x, y, w, h, d); }
void gr_set_draw_area(int x1, int y1, int x2, int y2){ TAP(set_draw_area, x1, y1, x2, y2); vram_census_draw_area(x1, y1, x2, y2); g_b->set_draw_area(x1, y1, x2, y2); }
void gr_get_draw_area(int *x1, int *y1, int *x2, int *y2) { g_b->get_draw_area(x1, y1, x2, y2); }
void gr_set_draw_offset(int x, int y)                { TAP(set_draw_offset, x, y); vram_census_draw_offset(x, y); g_b->set_draw_offset(x, y); g_hd_offset_x=x; g_hd_offset_y=y; }

/* Native-wide compositor — present only on backends that supply it. */
int  gr_wide_supported(void) { return g_b->render_wide_display != 0; }
void gr_wide_configure(int wide_w, int offset) {
    if (g_b->wide_configure) g_b->wide_configure(wide_w, offset);
}
void gr_wide_set_view(int enabled, int shift, int pad_left, int pad_right) {
    if (g_b->wide_set_view) g_b->wide_set_view(enabled, shift, pad_left, pad_right);
}
void gr_wide_set_target(int base_x) {
    if (g_b->wide_set_target) g_b->wide_set_target(base_x);
}
void gr_wide_disable_target(void) {
    if (g_b->wide_disable_target) g_b->wide_disable_target();
}
void gr_wide_clear(int base_x, int y, int h, uint16_t color) {
    if (g_b->wide_clear) g_b->wide_clear(base_x, y, h, color);
}
void gr_wide_clear_margins(int base_x, int y, int h, uint16_t color, int sides) {
    if (g_b->wide_clear_margins)
        g_b->wide_clear_margins(base_x, y, h, color, sides);
}
void gr_wide_feedback_rect(int src_base, int src_x, int src_y, int dst_x, int dst_y,
                           int w, int h, int r, int g, int b, int raw, int semi) {
    if (g_b->wide_feedback_rect)
        g_b->wide_feedback_rect(src_base, src_x, src_y, dst_x, dst_y, w, h, r, g, b,
                                raw, semi);
}
int gr_render_wide_display(uint32_t *out, int pitch, int base_x,
                           int disp_y, int disp_h) {
    if (g_b->render_wide_display)
        return g_b->render_wide_display(out, pitch, base_x, disp_y, disp_h);
    return 0;
}
int gr_wide_dump_full(uint32_t *out, int cap_pixels, int *ow, int *oh, int base_x) {
    if (g_b->wide_dump_full)
        return g_b->wide_dump_full(out, cap_pixels, ow, oh, base_x);
    return 0;
}

/* GP1 display registers are already updated by the caller. */
void gr_display_mode_changed(void) {
    if (g_b->display_mode_changed) g_b->display_mode_changed();
}
