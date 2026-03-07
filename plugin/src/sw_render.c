/*
 * sw_render.c
 *
 * Software rendering for Pre3 (landscape, rotated from portrait surface).
 *
 * The SDL surface is physically 480x800 (portrait). We render in logical
 * 800x480 (landscape) coordinates and rotate when writing pixels.
 *
 * Coordinate transform (landscape -> portrait, 90 deg CCW):
 *   physical_x = 479 - landscape_y
 *   physical_y = landscape_x
 *
 * Copyright (c) 2024 EMU7800
 */

#include <string.h>
#include "sw_render.h"

/* Font data from font.c */
#define FONT_CHAR_W 8
#define FONT_CHAR_H 8
#define FONT_FIRST_CHAR 32
#define FONT_LAST_CHAR 126
#define FONT_NUM_CHARS (FONT_LAST_CHAR - FONT_FIRST_CHAR + 1)
extern const unsigned char font_data[];

/* Surface state */
static SDL_Surface *g_surf = NULL;
static int g_phys_w = 0;   /* physical width (480) */
static int g_phys_h = 0;   /* physical height (800) */
static int g_pitch16 = 0;  /* pitch in uint16_t units */
void sw_init(SDL_Surface *screen)
{
    g_surf = screen;
    g_phys_w = screen->w;
    g_phys_h = screen->h;
    g_pitch16 = screen->pitch / 2;
}

/* Convert 8-bit RGB to RGB565 */
static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* Write a single pixel at logical landscape (lx, ly).
 * Caller must ensure surface is locked and coords are in bounds. */
static void put_pixel(uint16_t *pixels, int lx, int ly, uint16_t color)
{
    /* Rotate: physical_x = 479 - ly, physical_y = lx */
    int px = (g_phys_w - 1) - ly;
    int py = lx;
    pixels[py * g_pitch16 + px] = color;
}

/* Read a pixel at logical landscape (lx, ly). */
static uint16_t get_pixel(uint16_t *pixels, int lx, int ly)
{
    int px = (g_phys_w - 1) - ly;
    int py = lx;
    return pixels[py * g_pitch16 + px];
}

/* Alpha-blend two RGB565 colors: result = (fg * a + bg * (255 - a)) / 256
 * Blends R, G, B channels separately to prevent R*a overflow into B field. */
static uint16_t blend565(uint16_t bg, uint16_t fg, uint8_t a)
{
    uint32_t inv = 255 - a;
    uint32_t r = (((fg >> 11) & 0x1F) * a + ((bg >> 11) & 0x1F) * inv) >> 8;
    uint32_t g = (((fg >> 5)  & 0x3F) * a + ((bg >> 5)  & 0x3F) * inv) >> 8;
    uint32_t b = ((fg & 0x1F) * a + (bg & 0x1F) * inv) >> 8;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

void sw_clear(uint8_t r, uint8_t g, uint8_t b)
{
    if (!g_surf) return;
    SDL_LockSurface(g_surf);
    if (r == 0 && g == 0 && b == 0) {
        memset(g_surf->pixels, 0, g_phys_h * g_surf->pitch);
    } else {
        uint16_t color = rgb565(r, g, b);
        uint16_t *pixels = (uint16_t *)g_surf->pixels;
        int i, total = g_phys_h * g_pitch16;
        for (i = 0; i < total; i++)
            pixels[i] = color;
    }
    SDL_UnlockSurface(g_surf);
}

void sw_fill_rect(int x, int y, int w, int h,
                  uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t color = rgb565(r, g, b);
    uint16_t *pixels;
    int lx, ly;
    int x0, y0, x1, y1;

    if (!g_surf) return;

    x0 = x < 0 ? 0 : x;
    y0 = y < 0 ? 0 : y;
    x1 = x + w;
    y1 = y + h;
    if (x1 > SW_LOGICAL_W) x1 = SW_LOGICAL_W;
    if (y1 > SW_LOGICAL_H) y1 = SW_LOGICAL_H;
    if (x0 >= x1 || y0 >= y1) return;

    SDL_LockSurface(g_surf);
    pixels = (uint16_t *)g_surf->pixels;
    for (ly = y0; ly < y1; ly++) {
        for (lx = x0; lx < x1; lx++) {
            put_pixel(pixels, lx, ly, color);
        }
    }
    SDL_UnlockSurface(g_surf);
}

void sw_fill_rect_a(int x, int y, int w, int h,
                    uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    uint16_t fg_color;
    uint16_t *pixels;
    int lx, ly;
    int x0, y0, x1, y1;

    if (!g_surf || a == 0) return;
    if (a == 255) { sw_fill_rect(x, y, w, h, r, g, b); return; }

    fg_color = rgb565(r, g, b);

    x0 = x < 0 ? 0 : x;
    y0 = y < 0 ? 0 : y;
    x1 = x + w;
    y1 = y + h;
    if (x1 > SW_LOGICAL_W) x1 = SW_LOGICAL_W;
    if (y1 > SW_LOGICAL_H) y1 = SW_LOGICAL_H;
    if (x0 >= x1 || y0 >= y1) return;

    SDL_LockSurface(g_surf);
    pixels = (uint16_t *)g_surf->pixels;
    for (ly = y0; ly < y1; ly++) {
        for (lx = x0; lx < x1; lx++) {
            uint16_t bg = get_pixel(pixels, lx, ly);
            put_pixel(pixels, lx, ly, blend565(bg, fg_color, a));
        }
    }
    SDL_UnlockSurface(g_surf);
}

int sw_string_width(const char *str, int scale)
{
    int len = 0;
    while (*str) { len++; str++; }
    return len * FONT_CHAR_W * scale;
}

void sw_draw_string(int x, int y, const char *str, int scale,
                    uint8_t r, uint8_t g, uint8_t b)
{
    sw_draw_string_a(x, y, str, scale, r, g, b, 255);
}

void sw_draw_string_a(int x, int y, const char *str, int scale,
                      uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    uint16_t fg_color = rgb565(r, g, b);
    uint16_t *pixels;
    int ch_x = x;

    if (!g_surf || a == 0) return;

    SDL_LockSurface(g_surf);
    pixels = (uint16_t *)g_surf->pixels;

    while (*str) {
        int ch = (unsigned char)*str;
        if (ch >= FONT_FIRST_CHAR && ch <= FONT_LAST_CHAR) {
            const unsigned char *glyph = font_data + (ch - FONT_FIRST_CHAR) * FONT_CHAR_H;
            int row, col, sy, sx;
            for (row = 0; row < FONT_CHAR_H; row++) {
                unsigned char bits = glyph[row];
                for (col = 0; col < FONT_CHAR_W; col++) {
                    if (bits & (0x80 >> col)) {
                        /* Draw scaled pixel */
                        for (sy = 0; sy < scale; sy++) {
                            for (sx = 0; sx < scale; sx++) {
                                int lx = ch_x + col * scale + sx;
                                int ly = y + row * scale + sy;
                                if (lx >= 0 && lx < SW_LOGICAL_W &&
                                    ly >= 0 && ly < SW_LOGICAL_H) {
                                    if (a == 255) {
                                        put_pixel(pixels, lx, ly, fg_color);
                                    } else {
                                        uint16_t bg = get_pixel(pixels, lx, ly);
                                        put_pixel(pixels, lx, ly,
                                                  blend565(bg, fg_color, a));
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        ch_x += FONT_CHAR_W * scale;
        str++;
    }

    SDL_UnlockSurface(g_surf);
}

void sw_blit_indexed(const uint8_t *src, int src_w, int src_h, int src_pitch,
                     const uint16_t *palette565,
                     int dst_x, int dst_y, int dst_w, int dst_h)
{
    uint16_t *pixels;
    int dx, dy;
    int x_ratio, y_ratio;

    if (!g_surf || !src || !palette565) return;
    if (dst_w <= 0 || dst_h <= 0) return;

    /* Fixed-point scaling ratios (16.16) */
    x_ratio = (src_w << 16) / dst_w;
    y_ratio = (src_h << 16) / dst_h;

    SDL_LockSurface(g_surf);
    pixels = (uint16_t *)g_surf->pixels;

    for (dy = 0; dy < dst_h; dy++) {
        int ly = dst_y + dy;
        int sy = (dy * y_ratio) >> 16;
        const uint8_t *src_row;
        if (ly < 0 || ly >= SW_LOGICAL_H) continue;
        if (sy >= src_h) break;
        src_row = src + sy * src_pitch;
        for (dx = 0; dx < dst_w; dx++) {
            int lx = dst_x + dx;
            int sx = (dx * x_ratio) >> 16;
            if (lx < 0 || lx >= SW_LOGICAL_W) continue;
            if (sx >= src_w) break;
            put_pixel(pixels, lx, ly, palette565[src_row[sx]]);
        }
    }

    SDL_UnlockSurface(g_surf);
}

void sw_blit_rgba(const uint8_t *src, int src_w, int src_h, int src_stride,
                  int dst_x, int dst_y, int dst_w, int dst_h)
{
    uint16_t *pixels;
    int dx, dy, x_ratio, y_ratio;

    if (!g_surf || !src || dst_w <= 0 || dst_h <= 0) return;

    x_ratio = (src_w << 16) / dst_w;
    y_ratio = (src_h << 16) / dst_h;

    SDL_LockSurface(g_surf);
    pixels = (uint16_t *)g_surf->pixels;

    for (dy = 0; dy < dst_h; dy++) {
        int ly = dst_y + dy;
        int sy = (dy * y_ratio) >> 16;
        if (ly < 0 || ly >= SW_LOGICAL_H) continue;
        if (sy >= src_h) break;
        for (dx = 0; dx < dst_w; dx++) {
            int lx = dst_x + dx;
            int sx = (dx * x_ratio) >> 16;
            const uint8_t *p;
            uint8_t a;
            if (lx < 0 || lx >= SW_LOGICAL_W) continue;
            if (sx >= src_w) break;
            p = src + (sy * src_stride + sx) * 4;
            a = p[3];
            if (a == 0) continue;
            if (a == 255) {
                put_pixel(pixels, lx, ly, rgb565(p[0], p[1], p[2]));
            } else {
                uint16_t fg = rgb565(p[0], p[1], p[2]);
                uint16_t bg = get_pixel(pixels, lx, ly);
                put_pixel(pixels, lx, ly, blend565(bg, fg, a));
            }
        }
    }

    SDL_UnlockSurface(g_surf);
}

void sw_blit_rgb565(const uint16_t *src, int src_w, int src_h, int src_stride,
                    int dst_x, int dst_y, int dst_w, int dst_h)
{
    uint16_t *pixels;
    int dx, dy, x_ratio, y_ratio;

    if (!g_surf || !src || dst_w <= 0 || dst_h <= 0) return;

    x_ratio = (src_w << 16) / dst_w;
    y_ratio = (src_h << 16) / dst_h;

    SDL_LockSurface(g_surf);
    pixels = (uint16_t *)g_surf->pixels;

    for (dy = 0; dy < dst_h; dy++) {
        int ly = dst_y + dy;
        int sy = (dy * y_ratio) >> 16;
        const uint16_t *src_row;
        if (ly < 0 || ly >= SW_LOGICAL_H) continue;
        if (sy >= src_h) break;
        src_row = src + sy * src_stride;
        for (dx = 0; dx < dst_w; dx++) {
            int lx = dst_x + dx;
            int sx = (dx * x_ratio) >> 16;
            if (lx < 0 || lx >= SW_LOGICAL_W) continue;
            if (sx >= src_w) break;
            put_pixel(pixels, lx, ly, src_row[sx]);
        }
    }

    SDL_UnlockSurface(g_surf);
}

void sw_blit_rgb565_a(const uint16_t *src, int src_w, int src_h, int src_stride,
                      int dst_x, int dst_y, int dst_w, int dst_h, uint8_t a)
{
    uint16_t *pixels;
    int dx, dy, x_ratio, y_ratio;

    if (!g_surf || !src || dst_w <= 0 || dst_h <= 0 || a == 0) return;

    x_ratio = (src_w << 16) / dst_w;
    y_ratio = (src_h << 16) / dst_h;

    SDL_LockSurface(g_surf);
    pixels = (uint16_t *)g_surf->pixels;

    for (dy = 0; dy < dst_h; dy++) {
        int ly = dst_y + dy;
        int sy = (dy * y_ratio) >> 16;
        const uint16_t *src_row;
        if (ly < 0 || ly >= SW_LOGICAL_H) continue;
        if (sy >= src_h) break;
        src_row = src + sy * src_stride;
        for (dx = 0; dx < dst_w; dx++) {
            int lx = dst_x + dx;
            int sx = (dx * x_ratio) >> 16;
            uint16_t sc;
            if (lx < 0 || lx >= SW_LOGICAL_W) continue;
            if (sx >= src_w) break;
            sc = src_row[sx];
            if (sc == 0x0000) continue;  /* skip transparent black */
            if (a == 255) {
                put_pixel(pixels, lx, ly, sc);
            } else {
                uint16_t bg = get_pixel(pixels, lx, ly);
                put_pixel(pixels, lx, ly, blend565(bg, sc, a));
            }
        }
    }

    SDL_UnlockSurface(g_surf);
}

void sw_fill_circle(int cx, int cy, int radius,
                    uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t color = rgb565(r, g, b);
    uint16_t *pixels;
    int y, x;
    int r2 = radius * radius;

    if (!g_surf || radius <= 0) return;

    SDL_LockSurface(g_surf);
    pixels = (uint16_t *)g_surf->pixels;

    for (y = -radius; y <= radius; y++) {
        int ly = cy + y;
        int xspan;
        if (ly < 0 || ly >= SW_LOGICAL_H) continue;
        xspan = 0;
        while (xspan <= radius && xspan * xspan + y * y <= r2) xspan++;
        xspan--;
        for (x = cx - xspan; x <= cx + xspan; x++) {
            if (x >= 0 && x < SW_LOGICAL_W)
                put_pixel(pixels, x, ly, color);
        }
    }

    SDL_UnlockSurface(g_surf);
}

void sw_fill_circle_a(int cx, int cy, int radius,
                      uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    uint16_t fg_color;
    uint16_t *pixels;
    int y, x;
    int r2;

    if (!g_surf || radius <= 0 || a == 0) return;
    if (a == 255) { sw_fill_circle(cx, cy, radius, r, g, b); return; }

    fg_color = rgb565(r, g, b);
    r2 = radius * radius;

    SDL_LockSurface(g_surf);
    pixels = (uint16_t *)g_surf->pixels;

    for (y = -radius; y <= radius; y++) {
        int ly = cy + y;
        int xspan;
        if (ly < 0 || ly >= SW_LOGICAL_H) continue;
        xspan = 0;
        while (xspan <= radius && xspan * xspan + y * y <= r2) xspan++;
        xspan--;
        for (x = cx - xspan; x <= cx + xspan; x++) {
            if (x >= 0 && x < SW_LOGICAL_W) {
                uint16_t bg = get_pixel(pixels, x, ly);
                put_pixel(pixels, x, ly, blend565(bg, fg_color, a));
            }
        }
    }

    SDL_UnlockSurface(g_surf);
}

void sw_draw_scanlines(int x, int y, int w, int h, uint8_t alpha)
{
    uint16_t *pixels;
    int lx, ly;
    int x0, y0, x1, y1;
    uint8_t inv = 255 - alpha;

    if (!g_surf || alpha == 0) return;

    x0 = x < 0 ? 0 : x;
    y0 = y < 0 ? 0 : y;
    x1 = x + w;
    y1 = y + h;
    if (x1 > SW_LOGICAL_W) x1 = SW_LOGICAL_W;
    if (y1 > SW_LOGICAL_H) y1 = SW_LOGICAL_H;
    if (x0 >= x1 || y0 >= y1) return;

    /* Start on even boundary relative to display rect */
    if ((y0 - y) % 2 != 0) y0++;

    SDL_LockSurface(g_surf);
    pixels = (uint16_t *)g_surf->pixels;
    for (ly = y0 + 2; ly < y1; ly += 2) {
        for (lx = x0; lx < x1; lx++) {
            uint16_t bg = get_pixel(pixels, lx, ly);
            uint32_t r = (((bg >> 11) & 0x1F) * inv) >> 8;
            uint32_t g = (((bg >> 5) & 0x3F) * inv) >> 8;
            uint32_t b = ((bg & 0x1F) * inv) >> 8;
            put_pixel(pixels, lx, ly, (uint16_t)((r << 11) | (g << 5) | b));
        }
    }
    SDL_UnlockSurface(g_surf);
}

void sw_flip(void)
{
    if (g_surf) {
        SDL_Flip(g_surf);
    }
}
