/*
 * font.h
 *
 * Bitmap Font Renderer Header
 * 8x8 monospace font rendered via OpenGL ES texture
 *
 * Copyright (c) 2024 EMU7800
 */

#ifndef FONT_H
#define FONT_H

/* Initialize font system (create GL texture atlas) */
int font_init(void);

/* Shutdown font system */
void font_shutdown(void);

/* Draw a string at screen coordinates
 * x, y: top-left position in screen pixels
 * scale: multiplier (1 = 8px tall, 2 = 16px tall, etc.)
 * r, g, b, a: color and alpha (0.0 - 1.0)
 */
void font_draw_string(const char *text, float x, float y, int scale,
                      float r, float g, float b, float a);

/* Measure string width in pixels at given scale */
int font_string_width(const char *text, int scale);

#endif /* FONT_H */
