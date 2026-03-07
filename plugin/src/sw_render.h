/*
 * sw_render.h
 *
 * Software rendering for Pre3 (landscape, rotated from portrait surface).
 * All coordinates are logical 800x480 landscape.
 * Rotation to physical 480x800 portrait handled internally.
 *
 * Copyright (c) 2024 EMU7800
 */

#ifndef SW_RENDER_H
#define SW_RENDER_H

#include <SDL.h>
#include <stdint.h>

/* Logical landscape dimensions */
#define SW_LOGICAL_W 800
#define SW_LOGICAL_H 480

/* Initialize with the physical portrait SDL surface */
void sw_init(SDL_Surface *screen);

/* Clear entire screen to solid color */
void sw_clear(uint8_t r, uint8_t g, uint8_t b);

/* Fill a rectangle (solid) */
void sw_fill_rect(int x, int y, int w, int h,
                  uint8_t r, uint8_t g, uint8_t b);

/* Fill a rectangle with alpha blending */
void sw_fill_rect_a(int x, int y, int w, int h,
                    uint8_t r, uint8_t g, uint8_t b, uint8_t a);

/* Draw a string using the 8x8 bitmap font */
void sw_draw_string(int x, int y, const char *str, int scale,
                    uint8_t r, uint8_t g, uint8_t b);

/* Draw a string with alpha */
void sw_draw_string_a(int x, int y, const char *str, int scale,
                      uint8_t r, uint8_t g, uint8_t b, uint8_t a);

/* Measure string width in pixels at given scale */
int sw_string_width(const char *str, int scale);

/* Blit an indexed-color source buffer through a palette LUT.
 * Scales src to fill dst_w x dst_h at (dst_x, dst_y). */
void sw_blit_indexed(const uint8_t *src, int src_w, int src_h, int src_pitch,
                     const uint16_t *palette565,
                     int dst_x, int dst_y, int dst_w, int dst_h);

/* Fill a circle (solid) */
void sw_fill_circle(int cx, int cy, int radius,
                    uint8_t r, uint8_t g, uint8_t b);

/* Fill a circle with alpha blending */
void sw_fill_circle_a(int cx, int cy, int radius,
                      uint8_t r, uint8_t g, uint8_t b, uint8_t a);

/* Blit an RGBA source buffer with alpha blending, scaling to dst rect.
 * src_stride is in pixels (4 bytes per pixel). */
void sw_blit_rgba(const uint8_t *src, int src_w, int src_h, int src_stride,
                  int dst_x, int dst_y, int dst_w, int dst_h);

/* Blit an RGB565 source buffer, scaling to dst rect.
 * src_stride is in uint16_t units (pixels per row in src buffer). */
void sw_blit_rgb565(const uint16_t *src, int src_w, int src_h, int src_stride,
                    int dst_x, int dst_y, int dst_w, int dst_h);

/* Blit RGB565 with alpha dimming (0=transparent, 255=opaque).
 * Pixels with value 0x0000 (black) are treated as transparent. */
void sw_blit_rgb565_a(const uint16_t *src, int src_w, int src_h, int src_stride,
                      int dst_x, int dst_y, int dst_w, int dst_h, uint8_t a);

/* Draw scanline overlay (1px black line every 2 pixels, alpha blended) */
void sw_draw_scanlines(int x, int y, int w, int h, uint8_t alpha);

/* Flip the surface (SDL_Flip) */
void sw_flip(void);

#endif /* SW_RENDER_H */
