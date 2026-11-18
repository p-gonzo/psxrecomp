/* host_present.h - let a platform show a finished frame itself.
 *
 * The software present path (main.cpp, sdl_vblank_present_body) builds the
 * displayed frame as ARGB8888 and normally hands it to an SDL texture,
 * SDL_RenderCopy and SDL_RenderPresent. On a host where that is the expensive
 * part (the Nintendo 3DS: SDL's software renderer, a 640x480 logical canvas
 * and a second window-surface copy, all on the CPU), the platform can write
 * the frame to the screen directly instead.
 *
 * A platform registers its presenter with psx_host_present_register (for
 * example from a constructor that runs before main). With none registered,
 * psx_host_present_frame returns 0 and the SDL path is unchanged.
 */
#ifndef PSX_HOST_PRESENT_H
#define PSX_HOST_PRESENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Show one finished frame: `argb` is `w` x `h` ARGB8888 pixels, `pitch`
 * pixels per row. `pin_43` means the image must keep a 4:3 shape (FMV, a
 * canonical frame in a wide window); otherwise the host may fill its screen.
 * Pace to the display as the SDL path's driver vsync would.
 * Return 1 when the frame was shown (the caller skips its SDL path), 0 to let
 * the caller present it through SDL. */
typedef int (*psx_host_present_fn)(const uint32_t *argb, int w, int h,
                                   int pitch, int pin_43);

void psx_host_present_register(psx_host_present_fn fn);
int  psx_host_present_frame(const uint32_t *argb, int w, int h, int pitch, int pin_43);

#ifdef __cplusplus
}
#endif

#endif /* PSX_HOST_PRESENT_H */
