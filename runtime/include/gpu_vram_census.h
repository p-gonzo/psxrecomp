/* gpu_vram_census.h — measures how a game uses PS1 VRAM, for designing a
 * renderer that keeps CPU VRAM authoritative and draws only displayed
 * buffers on a host GPU (the 3DS PICA200 plan, docs in the title repo).
 *
 * It simulates ownership on a 16x16-pixel cell grid: draws whose draw area
 * is mostly a recently displayed rectangle count as "GPU" draws and make the
 * cells they touch GPU-owned; anything the CPU must read from GPU-owned cells
 * (texture sampling, VRAM copies, GPUREAD) counts as a readback. Inert until
 * armed through the debug server command `vram_census`; costs one branch per
 * renderer call otherwise. Observation only: never changes guest state. */
#ifndef GPU_VRAM_CENSUS_H
#define GPU_VRAM_CENSUS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern int g_vram_census_on;

void vram_census_reset(int enable);
/* Writes the accumulated counts since the last reset as one JSON object.
 * Returns the length written (truncated to cap - 1). */
int  vram_census_json(char *buf, size_t cap);

void vram_census_draw_area(int x1, int y1, int x2, int y2);
void vram_census_draw_offset(int x, int y);
void vram_census_texture_window(uint32_t raw);
/* Primitive in pre-offset coordinates. textured: tp/clut valid and the
 * u/v range is the sampled texel range (0..255). */
void vram_census_prim(int xmin, int ymin, int xmax, int ymax, int textured,
                      int umin, int vmin, int umax, int vmax,
                      uint16_t clut_x, uint16_t clut_y, uint16_t texpage,
                      int raw_texture);
void vram_census_fill(int x, int y, int w, int h);
void vram_census_copy(int sx, int sy, int dx, int dy, int w, int h);
void vram_census_upload(int x, int y, int w, int h);
void vram_census_cpu_read(int x, int y, int w, int h);
void vram_census_present(int x, int y, int w, int h, int depth24);

#ifdef __cplusplus
}
#endif
#endif
