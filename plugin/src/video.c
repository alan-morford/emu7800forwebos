/*
 * video.c
 *
 * OpenGL ES Video Output for webOS
 * Renders TIA/Maria frame buffer to screen via texture
 * Supports both 2600 (160px) and 7800 (320px) modes
 *
 * Copyright (c) 2024 EMU7800
 */

#include <stdio.h>
#include <string.h>
#include <GLES/gl.h>
#include <SDL.h>
#include "video.h"
#include "machine.h"
#include "tia.h"
#include "maria.h"
#include "input.h"
#include "device.h"
#include "sw_render.h"

/* Frame buffer dimensions */
#define FB_WIDTH_2600  160
#define FB_WIDTH_7800  320
#define FB_HEIGHT      262

/* Full 160-pixel visible width.  The Stella-style TIA handles HMOVE
 * extended hblank internally — objects already render at their correct
 * positions and the comb artifact is cosmetically minimal. */
#define FB_VISIBLE_W   FB_WIDTH_2600  /* 160 */

/* Zoom levels */
enum { ZOOM_1X = 0, ZOOM_2X, ZOOM_3X, ZOOM_FULL, ZOOM_COUNT };
static int g_zoom_level = ZOOM_FULL;  /* default */
static void video_update_vertices(void);
static void draw_scanline_overlay(void);

/*
 * Mode-specific texture dimensions (power of 2 for GLES 1.1).
 * 2600: 256x256 fits 160x210 visible content.
 * 7800: 512x256 fits 320x242 visible content.
 */
#define TEX_WIDTH_2600  256
#define TEX_HEIGHT_2600 256
#define TEX_WIDTH_7800  512
#define TEX_HEIGHT_7800 256

/* 7800 visible scanlines.
 * START_LINE_7800=12: MARIA's pipeline outputs scanline N's content to framebuffer
 * row N+1 (double-buffered hardware). First visible build is scanline 11 → row 12.
 * Last visible build is scanline 252 → row 253. Read rows 12..253 for 242 lines. */
#define VISIBLE_7800    242
#define START_LINE_7800 12

/* MAX_VISIBLE_LINES removed — active height is now dynamic via tia_get_active_height() */

static SDL_Surface *g_screen = NULL;

/*
 * Texture ping-pong: two GL texture handles per mode.
 * Upload to texture A while GPU may still read texture B from the previous
 * frame's draw call.  Alternates each frame via g_tex_idx.
 */
static GLuint g_textures_2600[2] = {0, 0};
static GLuint g_textures_7800[2] = {0, 0};
static int g_tex_idx = 0;

/*
 * Mode-specific texture upload buffers.
 * Stride == texture width for Adreno 220 compatibility.
 * BSS-zeroed; only the visible columns are written per frame.
 */
static uint16_t g_tex_buf_2600[TEX_WIDTH_2600 * TEX_HEIGHT_2600];  /* 128KB */
static uint16_t g_tex_buf_7800[TEX_WIDTH_7800 * TEX_HEIGHT_7800];  /* 256KB */

/* GL constant for RGB565 - may not be in headers */
#ifndef GL_UNSIGNED_SHORT_5_6_5
#define GL_UNSIGNED_SHORT_5_6_5 0x8363
#endif


/* Palette switching state */
static int g_maria_palette_index = MARIA_PALETTE_WARM;

/* Scanline overlay state: 0=off, 1=light, 2=medium, 3=dark */
static int g_scanline_mode = 0;
static const float g_scanline_alpha[3] = { 0.37f, 0.55f, 0.75f };

/* Pre-computed RGB565 palette LUTs - avoids 7 ALU ops per pixel */
static uint16_t g_tia_palette_565[256];
static uint16_t g_maria_palette_565[256];

static uint8_t g_fb_copy_2600[FB_WIDTH_2600 * FB_HEIGHT];
static uint8_t g_fb_copy_7800[FB_WIDTH_7800 * FB_HEIGHT];

/* Build RGB565 LUT from a 32-bit 0x00RRGGBB palette */
static void build_palette_565(const uint32_t *src, uint16_t *dst, int count)
{
    int i;
    uint32_t c;
    for (i = 0; i < count; i++) {
        c = src[i];
        dst[i] = (uint16_t)(
            (((c >> 16) & 0xF8) << 8) |
            (((c >> 8)  & 0xFC) << 3) |
            ((c >> 3)   & 0x1F)
        );
    }
}

/* Helper: create and configure a GL texture at given dimensions */
static GLuint create_texture(int w, int h, const void *data)
{
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0,
                 GL_RGB, GL_UNSIGNED_SHORT_5_6_5, data);
    return tex;
}

/* Initialize video */
int video_init(SDL_Surface *screen)
{
    g_screen = screen;

    /* Pre3 defaults to max-height (2X equivalent) */
    if (device_is_small())
        g_zoom_level = ZOOM_2X;

    /* Set up display quad for default zoom level */
    video_update_vertices();

    /* Build pre-computed RGB565 palette LUTs (needed for both GL and SW) */
    build_palette_565(tia_ntsc_palette, g_tia_palette_565, 256);
    build_palette_565(maria_get_palette(g_maria_palette_index), g_maria_palette_565, 256);

    if (device_has_gl()) {
        /* Default GL_UNPACK_ALIGNMENT of 4 is fine: all texture row strides
         * (256*2=512, 512*2=1024) are multiples of 4. */

        /* Create ping-pong texture pairs for each mode */
        g_textures_2600[0] = create_texture(TEX_WIDTH_2600, TEX_HEIGHT_2600, g_tex_buf_2600);
        g_textures_2600[1] = create_texture(TEX_WIDTH_2600, TEX_HEIGHT_2600, g_tex_buf_2600);
        g_textures_7800[0] = create_texture(TEX_WIDTH_7800, TEX_HEIGHT_7800, g_tex_buf_7800);
        g_textures_7800[1] = create_texture(TEX_WIDTH_7800, TEX_HEIGHT_7800, g_tex_buf_7800);
        g_tex_idx = 0;
    }

    return 0;
}

/* Shutdown video */
void video_shutdown(void)
{
    if (device_has_gl()) {
        glDeleteTextures(2, g_textures_2600);
        g_textures_2600[0] = g_textures_2600[1] = 0;
        glDeleteTextures(2, g_textures_7800);
        g_textures_7800[0] = g_textures_7800[1] = 0;
    }
    g_screen = NULL;
}

/* Mutable vertex array for display quad (updated on zoom change) */
static GLfloat g_vertices[8];

/* Recompute display quad vertices based on current zoom level */
static void video_update_vertices(void)
{
    int w, h, x, y;
    int sw = device_screen_width();
    int sh = device_screen_height();

    if (device_is_small()) {
        /* Pre3 (800x480): 3X same as 2X, FULL stretches to fill */
        switch (g_zoom_level) {
            case ZOOM_1X:   w = 320; h = 240; break;
            case ZOOM_FULL: w = sw;  h = sh;  break;
            default:        w = 640; h = 480; break; /* ZOOM_2X and ZOOM_3X */
        }
    } else {
        /* TouchPad (1024x768) */
        switch (g_zoom_level) {
            case ZOOM_1X:   w = 320;  h = 240; break;
            case ZOOM_2X:   w = 640;  h = 480; break;
            case ZOOM_FULL: w = 1024; h = 768; break;
            default:        w = 960;  h = 720; break; /* ZOOM_3X */
        }
    }
    x = (sw - w) / 2;
    y = (sh - h) / 2;
    g_vertices[0] = (GLfloat)x;       g_vertices[1] = (GLfloat)y;
    g_vertices[2] = (GLfloat)(x + w); g_vertices[3] = (GLfloat)y;
    g_vertices[4] = (GLfloat)x;       g_vertices[5] = (GLfloat)(y + h);
    g_vertices[6] = (GLfloat)(x + w); g_vertices[7] = (GLfloat)(y + h);
}

/* CRT-like display window for 2600 mode.
 *
 * Standard NTSC: visible area is scanlines 40-231 (192 lines).
 * Midpoint of that range: scanline 136.  A 210-line display window
 * centered at scanline 136 starts at scanline 31.
 *
 * Games that turn VBLANK off near scanline 40 (normal) need no
 * correction — content already starts at row 0 of the frame buffer.
 * Games that turn VBLANK off much earlier (e.g. Asteroids at scanline 0)
 * have blank rows above their content; we skip those to match what a
 * real CRT would show.
 */
#define CRT_DISPLAY_START_SL  31
#define CRT_MAX_DISPLAY_H    210

/* Render 2600 frame (160px wide, TIA) */
static void render_frame_2600(void)
{
    uint8_t *fb;
    int x, y, active_height, vbo_sl, display_offset, display_height;
    const uint8_t *src_row;
    uint16_t *dst_row;
    GLfloat tex_max_u, tex_max_v;
    GLfloat texcoords[8];

    fb = machine_get_frame_buffer();
    if (!fb) return;

    /* Dynamic active height from TIA VBLANK tracking (hysteresis-filtered) */
    active_height = tia_get_active_height();
    if (active_height < 1) active_height = 192;
    if (active_height > TEX_HEIGHT_2600) active_height = TEX_HEIGHT_2600;

    /* CRT display window correction.
     * When VBLANK turns off before scanline 31, the frame buffer contains
     * blank rows at the top that a real CRT would hide via overscan.
     * Skip those rows so content is centered like on original hardware. */
    vbo_sl = tia_get_vblank_off_scanline();
    display_offset = 0;
    if (vbo_sl >= 0 && vbo_sl < CRT_DISPLAY_START_SL) {
        display_offset = CRT_DISPLAY_START_SL - vbo_sl;
        if (display_offset >= active_height)
            display_offset = 0;
    }

    display_height = active_height - display_offset;
    if (display_offset > 0 && display_height > CRT_MAX_DISPLAY_H)
        display_height = CRT_MAX_DISPLAY_H;
    if (display_height < 1) display_height = 192;
    if (display_height > TEX_HEIGHT_2600) display_height = TEX_HEIGHT_2600;

    /* Copy display buffer — g_frame_ready + memory barriers in main.c
     * guarantee the buffer is stable when we reach here. */
    memcpy(g_fb_copy_2600, fb, FB_WIDTH_2600 * FB_HEIGHT);
    fb = g_fb_copy_2600;

    /* Convert display_height rows starting from display_offset to RGB565.
     * Content is anchored at row 0 by VBLANK-OFF offset in TIA.
     * Stride is TEX_WIDTH_2600 (256) — Adreno 220 requires stride == tex width. */
    for (y = 0; y < display_height; y++) {
        src_row = fb + (y + display_offset) * FB_WIDTH_2600;
        dst_row = g_tex_buf_2600 + y * TEX_WIDTH_2600;
        for (x = 0; x < FB_VISIBLE_W; x++) {
            dst_row[x] = g_tia_palette_565[src_row[x]];
        }
    }

    tex_max_u = (GLfloat)FB_VISIBLE_W / TEX_WIDTH_2600;
    tex_max_v = (GLfloat)display_height / TEX_HEIGHT_2600;

    texcoords[0] = 0.0f;      texcoords[1] = 0.0f;
    texcoords[2] = tex_max_u; texcoords[3] = 0.0f;
    texcoords[4] = 0.0f;      texcoords[5] = tex_max_v;
    texcoords[6] = tex_max_u; texcoords[7] = tex_max_v;

    /* Upload only display_height rows to texture */
    glBindTexture(GL_TEXTURE_2D, g_textures_2600[g_tex_idx]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    TEX_WIDTH_2600, display_height,
                    GL_RGB, GL_UNSIGNED_SHORT_5_6_5, g_tex_buf_2600);

    /* Draw textured quad — content stretches to fill 960x720 (CRT-accurate) */
    glEnable(GL_TEXTURE_2D);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);

    glVertexPointer(2, GL_FLOAT, 0, g_vertices);
    glTexCoordPointer(2, GL_FLOAT, 0, texcoords);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_TEXTURE_2D);
}

/* Render 7800 frame (320px wide, Maria) */
static void render_frame_7800(void)
{
    uint8_t *fb;
    int x, y, sy;
    const uint8_t *src_row;
    uint16_t *dst_row;
    GLfloat tex_max_u, tex_max_v;
    GLfloat texcoords[8];

    fb = machine_get_frame_buffer();
    if (!fb) return;

    /* Copy display buffer — g_frame_ready + memory barriers in main.c
     * guarantee the buffer is stable when we reach here. */
    memcpy(g_fb_copy_7800, fb, FB_WIDTH_7800 * FB_HEIGHT);
    fb = g_fb_copy_7800;

    /* Convert Maria frame buffer to RGB565 texture.
     * Stride is TEX_WIDTH_7800 (512) — Adreno 220 requires stride == tex width. */
    for (y = 0; y < VISIBLE_7800 && y < TEX_HEIGHT_7800; y++) {
        sy = y + START_LINE_7800;
        if (sy >= FB_HEIGHT) break;
        src_row = fb + sy * FB_WIDTH_7800;
        dst_row = g_tex_buf_7800 + y * TEX_WIDTH_7800;
        for (x = 0; x < FB_WIDTH_7800; x++) {
            dst_row[x] = g_maria_palette_565[src_row[x]];
        }
    }

    tex_max_u = (GLfloat)FB_WIDTH_7800 / TEX_WIDTH_7800;
    tex_max_v = (GLfloat)VISIBLE_7800 / TEX_HEIGHT_7800;

    texcoords[0] = 0.0f;      texcoords[1] = 0.0f;
    texcoords[2] = tex_max_u; texcoords[3] = 0.0f;
    texcoords[4] = 0.0f;      texcoords[5] = tex_max_v;
    texcoords[6] = tex_max_u; texcoords[7] = tex_max_v;

    /* Upload visible scanlines: 512*242 = 248KB */
    glBindTexture(GL_TEXTURE_2D, g_textures_7800[g_tex_idx]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    TEX_WIDTH_7800, VISIBLE_7800,
                    GL_RGB, GL_UNSIGNED_SHORT_5_6_5, g_tex_buf_7800);

    /* Draw textured quad */
    glEnable(GL_TEXTURE_2D);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);

    glVertexPointer(2, GL_FLOAT, 0, g_vertices);
    glTexCoordPointer(2, GL_FLOAT, 0, texcoords);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_TEXTURE_2D);
}

/*
 * Phase 1: palette convert + texture upload + draw quad + controls.
 * Does NOT call SwapBuffers — caller can time upload and swap separately.
 */
void video_render_upload(void)
{
    if (!g_screen) return;

    /* Clear screen */
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    /* Set up for 2D rendering */
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    /* Render based on machine type */
    if (machine_get_type() == MACHINE_7800) {
        render_frame_7800();
    } else {
        render_frame_2600();
    }

    /* Draw scanline overlay if enabled */
    if (g_scanline_mode > 0) {
        draw_scanline_overlay();
    }

    /* Draw 1px orange border around display (not in fullscreen) */
    if (g_zoom_level != ZOOM_FULL) {
        GLfloat x0 = g_vertices[0] - 1.0f;
        GLfloat y0 = g_vertices[1] - 1.0f;
        GLfloat x1 = g_vertices[2] + 1.0f;
        GLfloat y1 = g_vertices[5] + 1.0f;
        GLfloat edges[4][8] = {
            { x0, y0, x1, y0, x0, y0+1, x1, y0+1 },  /* top */
            { x0, y1-1, x1, y1-1, x0, y1, x1, y1 },  /* bottom */
            { x0, y0, x0+1, y0, x0, y1, x0+1, y1 },  /* left */
            { x1-1, y0, x1, y0, x1-1, y1, x1, y1 }   /* right */
        };
        int i;
        glDisable(GL_TEXTURE_2D);
        glEnableClientState(GL_VERTEX_ARRAY);
        glColor4f(1.0f, 0.5f, 0.15f, 1.0f);
        for (i = 0; i < 4; i++) {
            glVertexPointer(2, GL_FLOAT, 0, edges[i]);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
        glDisableClientState(GL_VERTEX_ARRAY);
    }

    /* Draw touch control overlays */
    input_draw_controls_gl();

    /* Draw popup overlay (OPTIONS / confirm dialog) if visible */
    input_draw_popup_gl();

    /* Ping-pong: next frame will use the other texture */
    g_tex_idx ^= 1;
}

/* Phase 2: swap buffers (may block on VSYNC) */
void video_render_swap(void)
{
    SDL_GL_SwapBuffers();
}

/* Combined render (backward compat) — calls both phases */
void video_render_frame(void)
{
    video_render_upload();
    video_render_swap();
}

/* Draw scanline overlay: 1px dark line every 2 pixels (fixed screen spacing).
 * All lines batched into a single GL_TRIANGLES draw call. */
#define SCANLINE_SPACING 2
static void draw_scanline_overlay(void)
{
    /* 768/2 = 384 max lines × 6 verts × 2 floats */
    GLfloat verts[384 * 6 * 2];
    int vi = 0;
    float x0 = g_vertices[0];
    float x1 = g_vertices[2];
    int y_top = (int)(g_vertices[1] + 0.5f);
    int y_bot = (int)(g_vertices[5] + 0.5f);
    int y;

    for (y = y_top + SCANLINE_SPACING; y < y_bot; y += SCANLINE_SPACING) {
        float fy = (float)y;
        /* Triangle 1 */
        verts[vi++] = x0; verts[vi++] = fy;
        verts[vi++] = x1; verts[vi++] = fy;
        verts[vi++] = x0; verts[vi++] = fy + 1.0f;
        /* Triangle 2 */
        verts[vi++] = x1; verts[vi++] = fy;
        verts[vi++] = x0; verts[vi++] = fy + 1.0f;
        verts[vi++] = x1; verts[vi++] = fy + 1.0f;
    }

    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glEnableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glColor4f(0.0f, 0.0f, 0.0f, g_scanline_alpha[g_scanline_mode - 1]);
    glVertexPointer(2, GL_FLOAT, 0, verts);
    glDrawArrays(GL_TRIANGLES, 0, vi / 2);

    glDisableClientState(GL_VERTEX_ARRAY);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}

/* Palette switching */
void video_set_maria_palette(int idx)
{
    if (idx < 0 || idx >= MARIA_PALETTE_COUNT)
        idx = MARIA_PALETTE_WARM;
    g_maria_palette_index = idx;
    build_palette_565(maria_get_palette(idx), g_maria_palette_565, 256);
}

int video_get_maria_palette(void)
{
    return g_maria_palette_index;
}

const char *video_get_palette_label(void)
{
    switch (g_maria_palette_index) {
        case MARIA_PALETTE_COOL: return "COOL";
        case MARIA_PALETTE_HOT:  return "HOT";
        default:                 return "WARM";
    }
}

/* Scanline toggle */
/* Scanline mode: 0=off, 1=light, 2=medium, 3=dark */
void video_set_scanline_mode(int mode)
{
    if (mode < 0 || mode > 3) mode = 0;
    g_scanline_mode = mode;
}

int video_get_scanline_mode(void)
{
    return g_scanline_mode;
}

int video_get_scanlines(void)
{
    return g_scanline_mode > 0;
}

void video_cycle_scanlines(void)
{
    g_scanline_mode = (g_scanline_mode + 1) % 4;
}

const char *video_get_scanlines_label(void)
{
    switch (g_scanline_mode) {
        case 1: return "LIGHT";
        case 2: return "MEDIUM";
        case 3: return "DARK";
        default: return "OFF";
    }
}

/* Cycle to next zoom level */
void video_cycle_zoom(void)
{
    if (device_is_small()) {
        /* Pre3: toggle between max-height and fullscreen */
        g_zoom_level = (g_zoom_level == ZOOM_FULL) ? ZOOM_2X : ZOOM_FULL;
    } else {
        g_zoom_level = (g_zoom_level + 1) % ZOOM_COUNT;
    }
    video_update_vertices();
}

/* Get label for current zoom level */
const char *video_get_zoom_label(void)
{
    if (device_is_small()) {
        return (g_zoom_level == ZOOM_FULL) ? "FULLSCREEN" : "ORIGINAL";
    }
    switch (g_zoom_level) {
        case ZOOM_1X:   return "ORIGINAL";
        case ZOOM_2X:   return "2X";
        case ZOOM_FULL: return "FULLSCREEN";
        default:        return "3X";
    }
}

/* ---- Software rendering path (Pre3) ---- */

/* SW display rect (logical landscape coords) */
static int g_sw_disp_x = 0, g_sw_disp_y = 0;
static int g_sw_disp_w = 640, g_sw_disp_h = 480;

/* Recompute SW display rect from current zoom/mode */
static void video_update_sw_rect(void)
{
    int scr_w = device_screen_width();
    int scr_h = device_screen_height();
    int w, h;

    if (g_zoom_level == ZOOM_FULL) {
        w = scr_w; h = scr_h;
    } else {
        /* Max height: scale to fill vertical, maintain aspect ratio */
        if (machine_get_type() == MACHINE_7800) {
            w = 640; h = 480;
        } else {
            int active = tia_get_active_height();
            if (active < 1) active = 192;
            w = 480; h = active * 3;
        }
    }

    /* Clamp to screen */
    if (w > scr_w) w = scr_w;
    if (h > scr_h) h = scr_h;

    g_sw_disp_x = (scr_w - w) / 2;
    g_sw_disp_y = (scr_h - h) / 2;
    g_sw_disp_w = w;
    g_sw_disp_h = h;
}

int video_sw_is_fullscreen(void)
{
    return g_zoom_level == ZOOM_FULL;
}

/* Render the emulator frame via software blitting */
void video_render_frame_sw(void)
{
    uint8_t *fb;

    if (!g_screen) return;

    video_update_sw_rect();

    fb = machine_get_frame_buffer();
    if (!fb) return;

    if (machine_get_type() == MACHINE_7800) {
        /* Copy display buffer for thread safety */
        memcpy(g_fb_copy_7800, fb, FB_WIDTH_7800 * FB_HEIGHT);

        sw_blit_indexed(g_fb_copy_7800 + START_LINE_7800 * FB_WIDTH_7800,
                        FB_WIDTH_7800, VISIBLE_7800, FB_WIDTH_7800,
                        g_maria_palette_565,
                        g_sw_disp_x, g_sw_disp_y,
                        g_sw_disp_w, g_sw_disp_h);
    } else {
        int active_height, vbo_sl, display_offset, display_height;

        memcpy(g_fb_copy_2600, fb, FB_WIDTH_2600 * FB_HEIGHT);

        active_height = tia_get_active_height();
        if (active_height < 1) active_height = 192;
        if (active_height > FB_HEIGHT) active_height = FB_HEIGHT;

        vbo_sl = tia_get_vblank_off_scanline();
        display_offset = 0;
        if (vbo_sl >= 0 && vbo_sl < CRT_DISPLAY_START_SL) {
            display_offset = CRT_DISPLAY_START_SL - vbo_sl;
            if (display_offset >= active_height)
                display_offset = 0;
        }

        display_height = active_height - display_offset;
        if (display_offset > 0 && display_height > CRT_MAX_DISPLAY_H)
            display_height = CRT_MAX_DISPLAY_H;
        if (display_height < 1) display_height = 192;
        if (display_height > FB_HEIGHT) display_height = FB_HEIGHT;

        sw_blit_indexed(g_fb_copy_2600 + display_offset * FB_WIDTH_2600,
                        FB_VISIBLE_W, display_height, FB_WIDTH_2600,
                        g_tia_palette_565,
                        g_sw_disp_x, g_sw_disp_y,
                        g_sw_disp_w, g_sw_disp_h);
    }

    /* Scanline overlay */
    if (g_scanline_mode > 0) {
        static const uint8_t alpha_lut[3] = { 94, 140, 191 };
        sw_draw_scanlines(g_sw_disp_x, g_sw_disp_y, g_sw_disp_w, g_sw_disp_h,
                          alpha_lut[g_scanline_mode - 1]);
    }

}
