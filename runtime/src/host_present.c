/* host_present.c - the host frame-presenter slot (host_present.h).
 *
 * Empty unless a platform layer registers a presenter, so every platform that
 * does not keeps the SDL texture/renderer path unchanged. A registration
 * function rather than a weak symbol keeps this portable to MSVC. */
#include "host_present.h"

static psx_host_present_fn s_presenter;

void psx_host_present_register(psx_host_present_fn fn)
{
    s_presenter = fn;
}

int psx_host_present_frame(const uint32_t *argb, int w, int h, int pitch, int pin_43)
{
    return s_presenter ? s_presenter(argb, w, h, pitch, pin_43) : 0;
}

static psx_host_present_vram_fn s_vram_presenter;

void psx_host_present_vram_register(psx_host_present_vram_fn fn)
{
    s_vram_presenter = fn;
}

int psx_host_present_vram(int x, int y, int w, int h, int depth24)
{
    return s_vram_presenter ? s_vram_presenter(x, y, w, h, depth24) : 0;
}
