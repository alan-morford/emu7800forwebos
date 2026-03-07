/*
 * video.h
 *
 * SDL Video Output Header
 *
 * Copyright (c) 2024 EMU7800
 */

#ifndef VIDEO_H
#define VIDEO_H

#include <SDL.h>

/* Initialize video subsystem */
int video_init(SDL_Surface *screen);

/* Shutdown video subsystem */
void video_shutdown(void);

/* Render current frame (calls upload + swap) */
void video_render_frame(void);

/* Split render: upload texture + draw quad (does NOT swap buffers) */
void video_render_upload(void);

/* Split render: swap buffers (may block on VSYNC) */
void video_render_swap(void);

/* Cycle to next zoom level (1X -> 2X -> 3X -> FULLSCREEN -> 1X) */
void video_cycle_zoom(void);

/* Get label for current zoom level ("ORIGINAL", "2X", "3X", "FULLSCREEN") */
const char *video_get_zoom_label(void);

/* Palette switching (7800 Maria palette) */
void video_set_maria_palette(int idx);
int  video_get_maria_palette(void);
const char *video_get_palette_label(void);

/* Scanline overlay toggle */
void video_set_scanlines(int enabled);
int  video_get_scanlines(void);

/* Scanline brightness (0=light, 1=medium, 2=dark) */
void video_set_scanline_brightness(int idx);
int  video_get_scanline_brightness(void);
const char *video_get_scanline_brightness_label(void);

/* Software rendering path (Pre3) */
void video_render_frame_sw(void);

/* Returns 1 if SW display covers full screen (no clear needed) */
int video_sw_is_fullscreen(void);

#endif /* VIDEO_H */
