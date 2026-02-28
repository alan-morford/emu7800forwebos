/*
 * input.c
 *
 * Multitouch Input Handler
 * Tracks up to 5 simultaneous finger touches, each mapped to a virtual
 * button region. Only the button associated with a specific finger is
 * released when that finger lifts.
 *
 * Copyright (c) 2024 EMU7800
 */

#define _GNU_SOURCE  /* for RTLD_DEFAULT */
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <dlfcn.h>
#include <GLES/gl.h>
#include <SDL.h>
#include "input.h"
#include "machine.h"
#include "font.h"
#include "video.h"
#include "filepicker.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Screen dimensions */
#define SCREEN_WIDTH  1024
#define SCREEN_HEIGHT 768

/* Touch control regions */

/* D-pad region (bottom-left) */
#define DPAD_X       0
#define DPAD_Y       568
#define DPAD_WIDTH   200
#define DPAD_HEIGHT  200
#define DPAD_CENTER_X (DPAD_X + DPAD_WIDTH / 2)
#define DPAD_CENTER_Y (DPAD_Y + DPAD_HEIGHT / 2)
#define DPAD_DEADZONE 30

/* Fire button (bottom-right) */
#define FIRE_SIZE    96
#define FIRE_X       (1024 - 30 - FIRE_SIZE)
#define FIRE_Y       642

/* Fire2 button (left of fire, for 7800) */
#define FIRE2_SIZE   96
#define FIRE2_X      (FIRE_X - 20 - FIRE2_SIZE)
#define FIRE2_Y      642

/* Top button bar: BACK, PAUSE on left; SELECT, RESET on right */
#define BTN_Y        10
#define BTN_HEIGHT   40
#define BTN_GAP      10

/* Left group */
#define BACK_X       10
#define BACK_WIDTH   96

#define PAUSE_X      (BACK_X + BACK_WIDTH + BTN_GAP)
#define PAUSE_WIDTH  96

/* Right group */
#define RESET_WIDTH  112
#define RESET_X      (SCREEN_WIDTH - 10 - RESET_WIDTH)

#define SELECT_WIDTH 112
#define SELECT_X     (RESET_X - BTN_GAP - SELECT_WIDTH)

/* ---- Per-finger multitouch tracking ---- */

#define MAX_TOUCHES 5

/* Bottom button bar: SAVE, LOAD, ZOOM, OPTIONS
 * LOAD and ZOOM split screen center (512). All buttons same width. */
#define BTN_BOT_WIDTH   128
#define BTN_BOT_HEIGHT  40
#define BTN_BOT_Y       720
#define BTN_BOT_GAP     10
/* LOAD right edge at center - gap/2, ZOOM left edge at center + gap/2 */
#define LOAD_X       (SCREEN_WIDTH / 2 - BTN_BOT_GAP / 2 - BTN_BOT_WIDTH)
#define LOAD_Y       BTN_BOT_Y
#define LOAD_WIDTH   BTN_BOT_WIDTH
#define LOAD_HEIGHT  BTN_BOT_HEIGHT
#define ZOOM_X       (SCREEN_WIDTH / 2 + BTN_BOT_GAP / 2)
#define ZOOM_Y       BTN_BOT_Y
#define ZOOM_WIDTH   BTN_BOT_WIDTH
#define ZOOM_HEIGHT  BTN_BOT_HEIGHT
#define SAVE_X       (LOAD_X - BTN_BOT_GAP - BTN_BOT_WIDTH)
#define SAVE_Y       BTN_BOT_Y
#define SAVE_WIDTH   BTN_BOT_WIDTH
#define SAVE_HEIGHT  BTN_BOT_HEIGHT
#define OPTIONS_X    (ZOOM_X + ZOOM_WIDTH + BTN_BOT_GAP)
#define OPTIONS_Y    BTN_BOT_Y
#define OPTIONS_WIDTH  BTN_BOT_WIDTH
#define OPTIONS_HEIGHT BTN_BOT_HEIGHT

typedef enum {
    TOUCH_NONE = 0,
    TOUCH_DPAD,
    TOUCH_FIRE,
    TOUCH_FIRE2,
    TOUCH_BACK,
    TOUCH_PAUSE,
    TOUCH_RESET,
    TOUCH_SELECT,
    TOUCH_OPTIONS,
    TOUCH_SAVE,
    TOUCH_LOAD,
    TOUCH_ZOOM
} TouchTarget;

typedef struct {
    int active;
    int finger_id;
    TouchTarget target;
} TouchSlot;

static TouchSlot g_touches[MAX_TOUCHES];

/* One-shot button flags (consumed by main loop) */
static int g_back_pressed = 0;
static int g_pause_pressed = 0;
static int g_save_pressed = 0;
static int g_load_pressed = 0;
static int g_zoom_pressed = 0;
static int g_options_pressed = 0;

/* OPTIONS popup / confirm dialog state */
static int g_options_popup_visible = 0;
static int g_confirm_visible = 0;
static int g_confirm_result = -1;  /* -1=pending, 0=No, 1=Yes */

/* Auto-save warning dialog state */
static int g_autosave_warn_visible = 0;
static int g_autosave_warn_result = -1;  /* -1=pending, 0=No, 1=Yes */
static int g_autosave_warn_action = 0;   /* 0=enabling autosave, 1=disabling ask */

/* Settings state (loaded from file at startup, persisted on change) */
static int g_autosave = 0;       /* 0=OFF, 1=ON */
static int g_autosave_ask = 1;   /* 0=OFF, 1=ON */
static int g_control_dim = 0;    /* 0=BRIGHT, 1=DIM, 2=DIMMER */

/* Keyboard state */
static int g_keyboard_active = 0;  /* Set to 1 on first key event, cleared on touch */
static int g_key_up = 0;
static int g_key_down = 0;
static int g_key_left = 0;
static int g_key_right = 0;
static int g_key_fire = 0;
static int g_key_fire2 = 0;

/* Save file exists flag (for greying out LOAD button) */
static int g_save_exists = 0;

/* D-pad finger tracking for visual feedback */
static int g_dpad_active = 0;
static int g_dpad_touch_x = 0;
static int g_dpad_touch_y = 0;

/* Notification system */
#define NOTIFY_FRAMES 120  /* ~2 seconds at 60fps */
static char g_notify_text[32] = {0};
static int g_notify_timer = 0;

/* Check if point is in rectangle */
static int point_in_rect(int px, int py, int rx, int ry, int rw, int rh)
{
    return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
}

/* Check if point is in circle */
static int point_in_circle(int px, int py, int cx, int cy, int radius)
{
    int dx = px - cx;
    int dy = py - cy;
    return (dx * dx + dy * dy) <= (radius * radius);
}

/* Forward declaration */
static void input_popup_handle_touch(int x, int y);

/* Control dim multiplier */
static float get_dim_multiplier(void) {
    switch (g_control_dim) {
        case 1: return 0.75f;
        case 2: return 0.50f;
        default: return 1.0f;
    }
}

/* Find a free touch slot, returns index or -1 */
static int find_free_slot(void)
{
    int i;
    for (i = 0; i < MAX_TOUCHES; i++) {
        if (!g_touches[i].active) return i;
    }
    return -1;
}

/* Find the slot for a given finger ID, returns index or -1 */
static int find_slot_by_finger(int finger_id)
{
    int i;
    for (i = 0; i < MAX_TOUCHES; i++) {
        if (g_touches[i].active && g_touches[i].finger_id == finger_id)
            return i;
    }
    return -1;
}

/* Check if a target is currently pressed by any finger */
static int is_target_active(TouchTarget target)
{
    int i;
    for (i = 0; i < MAX_TOUCHES; i++) {
        if (g_touches[i].active && g_touches[i].target == target)
            return 1;
    }
    return 0;
}

/* Update D-pad from touch position */
static void update_dpad(int x, int y)
{
    int dx, dy;

    /* Track finger position for visual feedback */
    g_dpad_active = 1;
    g_dpad_touch_x = x;
    g_dpad_touch_y = y;

    /* Clear all directions first */
    machine_set_joystick(0, 0, 0);  /* Up */
    machine_set_joystick(0, 1, 0);  /* Down */
    machine_set_joystick(0, 2, 0);  /* Left */
    machine_set_joystick(0, 3, 0);  /* Right */

    /* Calculate offset from center */
    dx = x - DPAD_CENTER_X;
    dy = y - DPAD_CENTER_Y;

    /* Check deadzone */
    if (dx * dx + dy * dy < DPAD_DEADZONE * DPAD_DEADZONE) {
        return;
    }

    /* Determine direction(s) */
    if (dy < -DPAD_DEADZONE) {
        machine_set_joystick(0, 0, 1);  /* Up */
    } else if (dy > DPAD_DEADZONE) {
        machine_set_joystick(0, 1, 1);  /* Down */
    }

    if (dx < -DPAD_DEADZONE) {
        machine_set_joystick(0, 2, 1);  /* Left */
    } else if (dx > DPAD_DEADZONE) {
        machine_set_joystick(0, 3, 1);  /* Right */
    }
}

/* Release the game input for a given target */
static void release_target(TouchTarget target)
{
    switch (target) {
        case TOUCH_DPAD:
            machine_set_joystick(0, 0, 0);
            machine_set_joystick(0, 1, 0);
            machine_set_joystick(0, 2, 0);
            machine_set_joystick(0, 3, 0);
            g_dpad_active = 0;
            break;
        case TOUCH_FIRE:
            machine_set_trigger(0, 0);
            break;
        case TOUCH_FIRE2:
            machine_set_trigger2(0, 0);
            break;
        case TOUCH_RESET:
            machine_set_switch(0, 0);
            break;
        case TOUCH_SELECT:
            machine_set_switch(1, 0);
            break;
        default:
            break;
    }
}

/* Initialize input */
void input_init(void)
{
    int i;
    for (i = 0; i < MAX_TOUCHES; i++) {
        g_touches[i].active = 0;
        g_touches[i].finger_id = -1;
        g_touches[i].target = TOUCH_NONE;
    }
    g_back_pressed = 0;
    g_pause_pressed = 0;
    g_save_pressed = 0;
    g_load_pressed = 0;
    g_zoom_pressed = 0;
    g_options_pressed = 0;
    g_options_popup_visible = 0;
    g_confirm_visible = 0;
    g_confirm_result = -1;
    g_autosave_warn_visible = 0;
    g_autosave_warn_result = -1;
    g_keyboard_active = 0;
    g_key_up = g_key_down = g_key_left = g_key_right = 0;
    g_key_fire = g_key_fire2 = 0;
    machine_clear_input();
}

/* Handle touch down - assign finger to a button region */
void input_handle_touch_down(int finger_id, int x, int y)
{
    int slot;
    TouchTarget target = TOUCH_NONE;

    /* Intercept all touches when popup/confirm/warning is visible */
    if (g_options_popup_visible || g_confirm_visible || g_autosave_warn_visible) {
        input_popup_handle_touch(x, y);
        return;
    }

    /* Touch hides keyboard labels */
    g_keyboard_active = 0;

    /* If this finger is already tracked, ignore (shouldn't happen) */
    if (find_slot_by_finger(finger_id) >= 0) return;

    /* Hit-test button regions (priority order) */
    if (point_in_rect(x, y, BACK_X, BTN_Y, BACK_WIDTH, BTN_HEIGHT)) {
        target = TOUCH_BACK;
    } else if (point_in_rect(x, y, PAUSE_X, BTN_Y, PAUSE_WIDTH, BTN_HEIGHT)) {
        target = TOUCH_PAUSE;
    } else if (point_in_rect(x, y, SAVE_X, SAVE_Y, SAVE_WIDTH, SAVE_HEIGHT)) {
        target = TOUCH_SAVE;
    } else if (point_in_rect(x, y, LOAD_X, LOAD_Y, LOAD_WIDTH, LOAD_HEIGHT)) {
        target = TOUCH_LOAD;
    } else if (point_in_rect(x, y, ZOOM_X, ZOOM_Y, ZOOM_WIDTH, ZOOM_HEIGHT)) {
        target = TOUCH_ZOOM;
    } else if (point_in_rect(x, y, OPTIONS_X, OPTIONS_Y, OPTIONS_WIDTH, OPTIONS_HEIGHT)) {
        target = TOUCH_OPTIONS;
    } else if (point_in_rect(x, y, DPAD_X, DPAD_Y, DPAD_WIDTH, DPAD_HEIGHT)) {
        target = TOUCH_DPAD;
    } else if (point_in_circle(x, y, FIRE_X + FIRE_SIZE/2, FIRE_Y + FIRE_SIZE/2, FIRE_SIZE/2)) {
        target = TOUCH_FIRE;
    } else if (machine_get_type() == MACHINE_7800 &&
               point_in_circle(x, y, FIRE2_X + FIRE2_SIZE/2, FIRE2_Y + FIRE2_SIZE/2, FIRE2_SIZE/2)) {
        target = TOUCH_FIRE2;
    } else if (point_in_rect(x, y, RESET_X, BTN_Y, RESET_WIDTH, BTN_HEIGHT)) {
        target = TOUCH_RESET;
    } else if (point_in_rect(x, y, SELECT_X, BTN_Y, SELECT_WIDTH, BTN_HEIGHT)) {
        target = TOUCH_SELECT;
    }

    if (target == TOUCH_NONE) return;

    /* Find a free slot */
    slot = find_free_slot();
    if (slot < 0) return;  /* All slots full */

    /* Assign finger to slot */
    g_touches[slot].active = 1;
    g_touches[slot].finger_id = finger_id;
    g_touches[slot].target = target;

    /* Activate the corresponding game input */
    switch (target) {
        case TOUCH_DPAD:
            update_dpad(x, y);
            break;
        case TOUCH_FIRE:
            machine_set_trigger(0, 1);
            break;
        case TOUCH_FIRE2:
            machine_set_trigger2(0, 1);
            break;
        case TOUCH_BACK:
            g_back_pressed = 1;
            break;
        case TOUCH_PAUSE:
            g_pause_pressed = 1;
            break;
        case TOUCH_SAVE:
            g_save_pressed = 1;
            break;
        case TOUCH_LOAD:
            g_load_pressed = 1;
            break;
        case TOUCH_ZOOM:
            g_zoom_pressed = 1;
            break;
        case TOUCH_OPTIONS:
            g_options_pressed = 1;
            g_options_popup_visible = 1;
            break;
        case TOUCH_RESET:
            machine_set_switch(0, 1);
            break;
        case TOUCH_SELECT:
            machine_set_switch(1, 1);
            break;
        default:
            break;
    }
}

/* Handle touch up - release only the button this finger was mapped to */
void input_handle_touch_up(int finger_id, int x, int y)
{
    int slot;
    (void)x;
    (void)y;

    slot = find_slot_by_finger(finger_id);
    if (slot < 0) return;  /* Unknown finger, ignore */

    /* Release the game input for this target */
    release_target(g_touches[slot].target);

    /* Free the slot */
    g_touches[slot].active = 0;
    g_touches[slot].finger_id = -1;
    g_touches[slot].target = TOUCH_NONE;
}

/* Handle touch move - only D-pad tracks finger motion */
void input_handle_touch_move(int finger_id, int x, int y)
{
    int slot = find_slot_by_finger(finger_id);
    if (slot < 0) return;

    if (g_touches[slot].target == TOUCH_DPAD) {
        /* Allow some slack outside the D-pad region */
        if (point_in_rect(x, y, DPAD_X - 50, DPAD_Y - 50, DPAD_WIDTH + 100, DPAD_HEIGHT + 100)) {
            update_dpad(x, y);
        } else {
            /* Finger moved too far outside D-pad area */
            machine_set_joystick(0, 0, 0);
            machine_set_joystick(0, 1, 0);
            machine_set_joystick(0, 2, 0);
            machine_set_joystick(0, 3, 0);
        }
    }
}

/* Draw a filled circle using OpenGL (triangle fan) */
static void draw_circle_gl(float cx, float cy, float radius,
                           float r, float g, float b, float a)
{
    GLfloat vertices[64];  /* 32 points max */
    int i, segments = 24;
    float angle_step = 2.0f * M_PI / segments;

    /* Center vertex */
    vertices[0] = cx;
    vertices[1] = cy;

    /* Circle vertices */
    for (i = 0; i <= segments; i++) {
        float angle = i * angle_step;
        vertices[2 + i * 2] = cx + radius * cosf(angle);
        vertices[2 + i * 2 + 1] = cy + radius * sinf(angle);
    }

    glColor4f(r, g, b, a);
    glVertexPointer(2, GL_FLOAT, 0, vertices);
    glEnableClientState(GL_VERTEX_ARRAY);
    glDrawArrays(GL_TRIANGLE_FAN, 0, segments + 2);
    glDisableClientState(GL_VERTEX_ARRAY);
}

/* Draw a filled rectangle using OpenGL */
static void draw_rect_gl(float x, float y, float w, float h,
                         float r, float g, float b, float a)
{
    GLfloat vertices[] = {
        x,     y,
        x + w, y,
        x,     y + h,
        x + w, y + h
    };

    glColor4f(r, g, b, a);
    glVertexPointer(2, GL_FLOAT, 0, vertices);
    glEnableClientState(GL_VERTEX_ARRAY);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableClientState(GL_VERTEX_ARRAY);
}

/* Draw touch control overlays using OpenGL */
void input_draw_controls_gl(void)
{
    int fire_pressed = is_target_active(TOUCH_FIRE);
    int fire2_pressed = is_target_active(TOUCH_FIRE2);
    float dim = get_dim_multiplier();

    /* Enable blending for transparency */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_TEXTURE_2D);

    /* Draw D-pad finger tracker (orange circle follows touch position) */
    if (g_dpad_active) {
        draw_circle_gl(g_dpad_touch_x, g_dpad_touch_y, DPAD_DEADZONE,
                       1.0f, 0.5f, 0.15f, 0.5f * dim);
    }

    /* Draw D-pad background (semi-transparent white circle) */
    draw_circle_gl(DPAD_CENTER_X, DPAD_CENTER_Y, 80, 1.0f, 1.0f, 1.0f, 0.25f * dim);

    /* Draw cross lines in D-pad (orange) */
    draw_rect_gl(DPAD_CENTER_X - 5, DPAD_CENTER_Y - 60, 10, 120, 1.0f, 0.5f, 0.15f, 0.25f * dim);
    draw_rect_gl(DPAD_CENTER_X - 60, DPAD_CENTER_Y - 5, 120, 10, 1.0f, 0.5f, 0.15f, 0.25f * dim);

    /* Draw Fire button (orange) */
    if (fire_pressed) {
        draw_circle_gl(FIRE_X + FIRE_SIZE/2, FIRE_Y + FIRE_SIZE/2,
                       FIRE_SIZE/2, 1.0f, 0.6f, 0.2f, 0.5f * dim);
    } else {
        draw_circle_gl(FIRE_X + FIRE_SIZE/2, FIRE_Y + FIRE_SIZE/2,
                       FIRE_SIZE/2, 1.0f, 0.5f, 0.15f, 0.4f * dim);
    }

    /* Draw Fire2 button (orange - 7800 only) */
    if (machine_get_type() == MACHINE_7800) {
        if (fire2_pressed) {
            draw_circle_gl(FIRE2_X + FIRE2_SIZE/2, FIRE2_Y + FIRE2_SIZE/2,
                           FIRE2_SIZE/2, 1.0f, 0.6f, 0.2f, 0.5f * dim);
        } else {
            draw_circle_gl(FIRE2_X + FIRE2_SIZE/2, FIRE2_Y + FIRE2_SIZE/2,
                           FIRE2_SIZE/2, 1.0f, 0.5f, 0.15f, 0.4f * dim);
        }
    }

    /* Draw top button bar: BACK, PAUSE (left) ... SELECT, RESET (right) */
    /* BACK (orange, same as fire button) */
    draw_rect_gl(BACK_X, BTN_Y, BACK_WIDTH, BTN_HEIGHT, 1.0f, 0.5f, 0.15f, 0.4f * dim);
    {
        int bw = font_string_width("BACK", 2);
        font_draw_string("BACK", BACK_X + (BACK_WIDTH - bw) / 2, BTN_Y + 12, 2,
                         1.0f, 1.0f, 1.0f, 0.5f * dim);
    }

    /* PAUSE */
    draw_rect_gl(PAUSE_X, BTN_Y, PAUSE_WIDTH, BTN_HEIGHT, 0.3f, 0.3f, 0.3f, 0.4f * dim);
    {
        int pw = font_string_width("PAUSE", 2);
        font_draw_string("PAUSE", PAUSE_X + (PAUSE_WIDTH - pw) / 2, BTN_Y + 12, 2,
                         1.0f, 0.5f, 0.15f, 0.5f * dim);
    }

    /* SELECT */
    draw_rect_gl(SELECT_X, BTN_Y, SELECT_WIDTH, BTN_HEIGHT, 0.3f, 0.3f, 0.3f, 0.4f * dim);
    {
        int sw = font_string_width("SELECT", 2);
        font_draw_string("SELECT", SELECT_X + (SELECT_WIDTH - sw) / 2, BTN_Y + 12, 2,
                         1.0f, 0.5f, 0.15f, 0.5f * dim);
    }

    /* RESET */
    draw_rect_gl(RESET_X, BTN_Y, RESET_WIDTH, BTN_HEIGHT, 0.3f, 0.3f, 0.3f, 0.4f * dim);
    {
        int rw = font_string_width("RESET", 2);
        font_draw_string("RESET", RESET_X + (RESET_WIDTH - rw) / 2, BTN_Y + 12, 2,
                         1.0f, 0.5f, 0.15f, 0.5f * dim);
    }

    /* Draw bottom SAVE/LOAD/ZOOM/OPTIONS buttons */
    /* SAVE */
    draw_rect_gl(SAVE_X, SAVE_Y, SAVE_WIDTH, SAVE_HEIGHT, 0.3f, 0.3f, 0.3f, 0.4f * dim);
    {
        int svw = font_string_width("SAVE", 2);
        font_draw_string("SAVE", SAVE_X + (SAVE_WIDTH - svw) / 2, SAVE_Y + 12, 2,
                         1.0f, 0.5f, 0.15f, 0.5f * dim);
    }

    /* LOAD (greyed out if no save file exists) */
    if (g_save_exists) {
        int lw = font_string_width("LOAD", 2);
        draw_rect_gl(LOAD_X, LOAD_Y, LOAD_WIDTH, LOAD_HEIGHT, 0.3f, 0.3f, 0.3f, 0.4f * dim);
        font_draw_string("LOAD", LOAD_X + (LOAD_WIDTH - lw) / 2, LOAD_Y + 12, 2,
                         1.0f, 0.5f, 0.15f, 0.5f * dim);
    } else {
        int lw = font_string_width("LOAD", 2);
        draw_rect_gl(LOAD_X, LOAD_Y, LOAD_WIDTH, LOAD_HEIGHT, 0.2f, 0.2f, 0.2f, 0.15f * dim);
        font_draw_string("LOAD", LOAD_X + (LOAD_WIDTH - lw) / 2, LOAD_Y + 12, 2,
                         0.5f, 0.5f, 0.5f, 0.3f * dim);
    }

    /* ZOOM */
    draw_rect_gl(ZOOM_X, ZOOM_Y, ZOOM_WIDTH, ZOOM_HEIGHT, 0.3f, 0.3f, 0.3f, 0.4f * dim);
    {
        int zw = font_string_width("ZOOM", 2);
        font_draw_string("ZOOM", ZOOM_X + (ZOOM_WIDTH - zw) / 2, ZOOM_Y + 12, 2,
                         1.0f, 0.5f, 0.15f, 0.5f * dim);
    }

    /* OPTIONS */
    draw_rect_gl(OPTIONS_X, OPTIONS_Y, OPTIONS_WIDTH, OPTIONS_HEIGHT, 0.3f, 0.3f, 0.3f, 0.4f * dim);
    {
        int ow = font_string_width("OPTIONS", 2);
        font_draw_string("OPTIONS", OPTIONS_X + (OPTIONS_WIDTH - ow) / 2, OPTIONS_Y + 12, 2,
                         1.0f, 0.5f, 0.15f, 0.5f * dim);
    }

    /* Draw notification text at top center */
    if (g_notify_timer > 0) {
        float alpha = (g_notify_timer > 30) ? 0.9f : (g_notify_timer / 30.0f) * 0.9f;
        int text_w = font_string_width(g_notify_text, 3);
        int text_x = (SCREEN_WIDTH - text_w) / 2;
        font_draw_string(g_notify_text, text_x, BTN_Y + 12, 3,
                         1.0f, 1.0f, 1.0f, alpha);
    }

    /* Keyboard key labels (shown when keyboard is active) */
    if (g_keyboard_active) {
        /* D-pad labels: W/A/S/D (black) */
        font_draw_string("W", DPAD_CENTER_X - 8, DPAD_CENTER_Y - 50, 2,
                         0.0f, 0.0f, 0.0f, 0.8f);
        font_draw_string("S", DPAD_CENTER_X - 8, DPAD_CENTER_Y + 30, 2,
                         0.0f, 0.0f, 0.0f, 0.8f);
        font_draw_string("A", DPAD_CENTER_X - 50, DPAD_CENTER_Y - 8, 2,
                         0.0f, 0.0f, 0.0f, 0.8f);
        font_draw_string("D", DPAD_CENTER_X + 40, DPAD_CENTER_Y - 8, 2,
                         0.0f, 0.0f, 0.0f, 0.8f);

        /* Fire button labels (black) */
        if (machine_get_type() == MACHINE_7800) {
            /* 7800: K = fire (right), J = fire2 (left) */
            font_draw_string("K", FIRE_X + FIRE_SIZE/2 - 8, FIRE_Y + FIRE_SIZE/2 - 8, 2,
                             0.0f, 0.0f, 0.0f, 0.8f);
            font_draw_string("J", FIRE2_X + FIRE2_SIZE/2 - 8, FIRE2_Y + FIRE2_SIZE/2 - 8, 2,
                             0.0f, 0.0f, 0.0f, 0.8f);
        } else {
            /* 2600: J = fire */
            font_draw_string("J", FIRE_X + FIRE_SIZE/2 - 8, FIRE_Y + FIRE_SIZE/2 - 8, 2,
                             0.0f, 0.0f, 0.0f, 0.8f);
        }

        /* Top bar button labels (orange, centered below each button) */
        font_draw_string("1", BACK_X + BACK_WIDTH/2 - 4, BTN_Y + BTN_HEIGHT + 4, 2,
                         1.0f, 0.5f, 0.15f, 0.8f * dim);
        font_draw_string("2", PAUSE_X + PAUSE_WIDTH/2 - 4, BTN_Y + BTN_HEIGHT + 4, 2,
                         1.0f, 0.5f, 0.15f, 0.8f * dim);
        font_draw_string("3", SELECT_X + SELECT_WIDTH/2 - 4, BTN_Y + BTN_HEIGHT + 4, 2,
                         1.0f, 0.5f, 0.15f, 0.8f * dim);
        font_draw_string("4", RESET_X + RESET_WIDTH/2 - 4, BTN_Y + BTN_HEIGHT + 4, 2,
                         1.0f, 0.5f, 0.15f, 0.8f * dim);

        /* Bottom bar button labels (orange, centered above each button) */
        font_draw_string("5", SAVE_X + SAVE_WIDTH/2 - 4, SAVE_Y - 18, 2,
                         1.0f, 0.5f, 0.15f, 0.8f * dim);
        font_draw_string("6", LOAD_X + LOAD_WIDTH/2 - 4, LOAD_Y - 18, 2,
                         1.0f, 0.5f, 0.15f, 0.8f * dim);
        font_draw_string("7", ZOOM_X + ZOOM_WIDTH/2 - 4, ZOOM_Y - 18, 2,
                         1.0f, 0.5f, 0.15f, 0.8f * dim);
        font_draw_string("8", OPTIONS_X + OPTIONS_WIDTH/2 - 4, OPTIONS_Y - 18, 2,
                         1.0f, 0.5f, 0.15f, 0.8f * dim);
    }

    glDisable(GL_BLEND);
}

/* Draw touch control overlays (software rendering - deprecated) */
void input_draw_controls(uint32_t *pixels, int pitch)
{
    (void)pixels;
    (void)pitch;
}

/* Check if reset button is pressed */
int input_reset_pressed(void)
{
    return is_target_active(TOUCH_RESET);
}

/* Check if select button is pressed */
int input_select_pressed(void)
{
    return is_target_active(TOUCH_SELECT);
}

/* Check if back button was pressed (returns 1 once, then clears) */
int input_back_pressed(void)
{
    if (g_back_pressed) {
        g_back_pressed = 0;
        return 1;
    }
    return 0;
}

/* Check if pause button was pressed (returns 1 once, then clears) */
int input_pause_pressed(void)
{
    if (g_pause_pressed) {
        g_pause_pressed = 0;
        return 1;
    }
    return 0;
}

/* Check if save button was pressed (returns 1 once, then clears) */
int input_save_pressed(void)
{
    if (g_save_pressed) {
        g_save_pressed = 0;
        return 1;
    }
    return 0;
}

/* Check if load button was pressed (returns 1 once, then clears) */
int input_load_pressed(void)
{
    if (g_load_pressed) {
        g_load_pressed = 0;
        return 1;
    }
    return 0;
}

/* Check if zoom button was pressed (returns 1 once, then clears) */
int input_zoom_pressed(void)
{
    if (g_zoom_pressed) {
        g_zoom_pressed = 0;
        return 1;
    }
    return 0;
}

/* Handle keyboard key down */
void input_handle_key_down(int sdl_key)
{
    g_keyboard_active = 1;

    switch (sdl_key) {
        case SDLK_w:
            if (!g_key_up) {
                g_key_up = 1;
                machine_set_joystick(0, 0, 1);
            }
            break;
        case SDLK_s:
            if (!g_key_down) {
                g_key_down = 1;
                machine_set_joystick(0, 1, 1);
            }
            break;
        case SDLK_a:
            if (!g_key_left) {
                g_key_left = 1;
                machine_set_joystick(0, 2, 1);
            }
            break;
        case SDLK_d:
            if (!g_key_right) {
                g_key_right = 1;
                machine_set_joystick(0, 3, 1);
            }
            break;
        case SDLK_j:
            if (machine_get_type() == MACHINE_7800) {
                if (!g_key_fire2) {
                    g_key_fire2 = 1;
                    machine_set_trigger2(0, 1);
                }
            } else {
                if (!g_key_fire) {
                    g_key_fire = 1;
                    machine_set_trigger(0, 1);
                }
            }
            break;
        case SDLK_k:
            if (machine_get_type() == MACHINE_7800 && !g_key_fire) {
                g_key_fire = 1;
                machine_set_trigger(0, 1);
            }
            break;
        case SDLK_1:
            g_back_pressed = 1;
            break;
        case SDLK_2:
            g_pause_pressed = 1;
            break;
        case SDLK_3:
            machine_set_switch(1, 1);
            break;
        case SDLK_4:
            machine_set_switch(0, 1);
            break;
        case SDLK_5:
            g_save_pressed = 1;
            break;
        case SDLK_6:
            g_load_pressed = 1;
            break;
        case '7':
            g_zoom_pressed = 1;
            break;
        case '8':
            g_options_pressed = 1;
            g_options_popup_visible = 1;
            break;
        default:
            break;
    }
}

/* Handle keyboard key up */
void input_handle_key_up(int sdl_key)
{
    switch (sdl_key) {
        case SDLK_w:
            g_key_up = 0;
            machine_set_joystick(0, 0, 0);
            break;
        case SDLK_s:
            g_key_down = 0;
            machine_set_joystick(0, 1, 0);
            break;
        case SDLK_a:
            g_key_left = 0;
            machine_set_joystick(0, 2, 0);
            break;
        case SDLK_d:
            g_key_right = 0;
            machine_set_joystick(0, 3, 0);
            break;
        case SDLK_j:
            if (machine_get_type() == MACHINE_7800) {
                g_key_fire2 = 0;
                machine_set_trigger2(0, 0);
            } else {
                g_key_fire = 0;
                machine_set_trigger(0, 0);
            }
            break;
        case SDLK_k:
            g_key_fire = 0;
            machine_set_trigger(0, 0);
            break;
        case SDLK_3:
            machine_set_switch(1, 0);
            break;
        case SDLK_4:
            machine_set_switch(0, 0);
            break;
        default:
            break;
    }
}

/* Returns 1 if keyboard was detected */
int input_keyboard_active(void)
{
    return g_keyboard_active;
}

/* Set whether a save file exists for the current ROM */
void input_set_save_exists(int exists)
{
    g_save_exists = exists;
}

/* Show a notification message at the top of the screen */
void input_show_notification(const char *text)
{
    strncpy(g_notify_text, text, sizeof(g_notify_text) - 1);
    g_notify_text[sizeof(g_notify_text) - 1] = '\0';
    g_notify_timer = NOTIFY_FRAMES;
}

/* Tick notification timer (call once per frame) */
void input_tick(void)
{
    if (g_notify_timer > 0) {
        g_notify_timer--;
    }
}

/* ---- OPTIONS popup / confirm dialog ---- */

/* OPTIONS button one-shot */
int input_options_pressed(void)
{
    if (g_options_pressed) {
        g_options_pressed = 0;
        return 1;
    }
    return 0;
}

int input_options_popup_visible(void)
{
    return g_options_popup_visible;
}

void input_close_options_popup(void)
{
    g_options_popup_visible = 0;
}

int input_confirm_visible(void)
{
    return g_confirm_visible;
}

int input_confirm_result(void)
{
    int r = g_confirm_result;
    if (r >= 0) {
        g_confirm_visible = 0;
        g_confirm_result = -1;
    }
    return r;
}

void input_show_confirm(void)
{
    g_confirm_visible = 1;
    g_confirm_result = -1;
}

/* Settings accessors */
int input_get_autosave(void)      { return g_autosave; }
void input_set_autosave(int val)  { g_autosave = val ? 1 : 0; }
int input_get_autosave_ask(void)  { return g_autosave_ask; }
void input_set_autosave_ask(int val) { g_autosave_ask = val ? 1 : 0; }
int input_get_control_dim(void)   { return g_control_dim; }
void input_set_control_dim(int val) { g_control_dim = (val >= 0 && val <= 2) ? val : 0; }

int input_autosave_warn_visible(void) { return g_autosave_warn_visible; }
int input_autosave_warn_result(void)  { return g_autosave_warn_result; }

/* ---- Popup layout constants ---- */
#define IGPOPUP_ROWS    7
#define IGPOPUP_W       460
#define IGPOPUP_PAD     16
#define IGPOPUP_TITLE_H 28
#define IGPOPUP_ROW_H   44
#define IGPOPUP_H       (IGPOPUP_PAD + IGPOPUP_TITLE_H + IGPOPUP_PAD + IGPOPUP_ROWS * IGPOPUP_ROW_H + IGPOPUP_PAD)
#define IGPOPUP_X       ((SCREEN_WIDTH - IGPOPUP_W) / 2)
#define IGPOPUP_Y       ((SCREEN_HEIGHT - IGPOPUP_H) / 2)
#define IGPOPUP_BTN_W   112
#define IGPOPUP_BTN_H   32

/* Confirm dialog layout */
#define CONFIRM_W    420
#define CONFIRM_H    120
#define CONFIRM_X    ((SCREEN_WIDTH - CONFIRM_W) / 2)
#define CONFIRM_Y    ((SCREEN_HEIGHT - CONFIRM_H) / 2)
#define CONFIRM_BTN_W 80
#define CONFIRM_BTN_H 36

/* Auto-save warning dialog layout */
#define ASWARN_W     640
#define ASWARN_H     210
#define ASWARN_X     ((SCREEN_WIDTH - ASWARN_W) / 2)
#define ASWARN_Y     ((SCREEN_HEIGHT - ASWARN_H) / 2)

/* Draw the OPTIONS popup overlay */
void input_draw_popup_gl(void)
{
    int row;
    int row_x, row_y, btn_x, btn_y;
    const char *label;
    float lbl_r, lbl_g, lbl_b, lbl_a;

    if (!g_options_popup_visible && !g_confirm_visible && !g_autosave_warn_visible)
        return;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_TEXTURE_2D);

    if (g_options_popup_visible) {
        /* Dark overlay (full screen black 60%) */
        draw_rect_gl(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0.0f, 0.0f, 0.0f, 0.6f);

        /* Black box with orange 1px border */
        draw_rect_gl(IGPOPUP_X - 1, IGPOPUP_Y - 1, IGPOPUP_W + 2, IGPOPUP_H + 2,
                     1.0f, 0.5f, 0.15f, 1.0f);
        draw_rect_gl(IGPOPUP_X, IGPOPUP_Y, IGPOPUP_W, IGPOPUP_H,
                     0.0f, 0.0f, 0.0f, 0.95f);

        /* Title: "Options" (centered, orange) */
        {
            int tw = font_string_width("Options", 3);
            font_draw_string("Options", IGPOPUP_X + (IGPOPUP_W - tw) / 2,
                             IGPOPUP_Y + IGPOPUP_PAD, 3,
                             1.0f, 0.5f, 0.15f, 1.0f);
        }

        /* Rows */
        for (row = 0; row < IGPOPUP_ROWS; row++) {
            const char *row_label = NULL;
            row_y = IGPOPUP_Y + IGPOPUP_PAD + IGPOPUP_TITLE_H + IGPOPUP_PAD + row * IGPOPUP_ROW_H;
            row_x = IGPOPUP_X + IGPOPUP_PAD;
            btn_x = IGPOPUP_X + IGPOPUP_W - IGPOPUP_PAD - IGPOPUP_BTN_W;
            btn_y = row_y + (IGPOPUP_ROW_H - IGPOPUP_BTN_H) / 2;

            label = NULL;
            lbl_r = 1.0f; lbl_g = 0.5f; lbl_b = 0.15f; lbl_a = 1.0f;

            switch (row) {
                case 0:
                    row_label = "Auto-Save on Close";
                    label = g_autosave ? "ON" : "OFF";
                    break;
                case 1:
                    row_label = "Ask Before Saving";
                    label = g_autosave_ask ? "ON" : "OFF";
                    if (!g_autosave) {
                        lbl_r = 0.4f; lbl_g = 0.4f; lbl_b = 0.4f; lbl_a = 0.5f;
                    }
                    break;
                case 2:
                    row_label = "Control Brightness";
                    switch (g_control_dim) {
                        case 1: label = "DIM"; break;
                        case 2: label = "DIMMER"; break;
                        default: label = "BRIGHT"; break;
                    }
                    break;
                case 3:
                    row_label = "Scanlines";
                    label = video_get_scanlines() ? "ON" : "OFF";
                    break;
                case 4:
                    row_label = "Scanline Brightness";
                    label = video_get_scanline_brightness_label();
                    break;
                case 5:
                    row_label = "Palette (7800)";
                    label = video_get_palette_label();
                    break;
                case 6:
                    row_label = "Bug Report";
                    label = "EMAIL";
                    break;
            }

            /* Row label (white text) */
            if (row_label) {
                font_draw_string(row_label, row_x, row_y + 10, 2,
                                 0.9f, 0.9f, 0.9f, (row == 1 && !g_autosave) ? 0.4f : 0.9f);
            }

            /* Button background */
            if (row == 1 && !g_autosave) {
                draw_rect_gl(btn_x, btn_y, IGPOPUP_BTN_W, IGPOPUP_BTN_H,
                             0.2f, 0.2f, 0.2f, 0.3f);
            } else {
                draw_rect_gl(btn_x, btn_y, IGPOPUP_BTN_W, IGPOPUP_BTN_H,
                             0.3f, 0.3f, 0.3f, 0.6f);
            }

            /* Button label (centered) */
            if (label) {
                int lw = font_string_width(label, 2);
                font_draw_string(label, btn_x + (IGPOPUP_BTN_W - lw) / 2,
                                 btn_y + 8, 2, lbl_r, lbl_g, lbl_b, lbl_a);
            }
        }
    }

    /* Confirm dialog (drawn ON TOP of options overlay if both active) */
    if (g_confirm_visible) {
        int yes_x, no_x, btn_cy;
        const char *q = "Save before closing?";
        int qw;

        if (!g_options_popup_visible) {
            /* Dark overlay only if options popup isn't already drawing one */
            draw_rect_gl(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0.0f, 0.0f, 0.0f, 0.6f);
        }

        /* Box with orange border */
        draw_rect_gl(CONFIRM_X - 1, CONFIRM_Y - 1, CONFIRM_W + 2, CONFIRM_H + 2,
                     1.0f, 0.5f, 0.15f, 1.0f);
        draw_rect_gl(CONFIRM_X, CONFIRM_Y, CONFIRM_W, CONFIRM_H,
                     0.0f, 0.0f, 0.0f, 0.95f);

        /* Question text (centered, orange) */
        qw = font_string_width(q, 2);
        font_draw_string(q, CONFIRM_X + (CONFIRM_W - qw) / 2,
                         CONFIRM_Y + 16, 2,
                         1.0f, 0.5f, 0.15f, 1.0f);

        /* Yes / No buttons */
        btn_cy = CONFIRM_Y + CONFIRM_H - CONFIRM_BTN_H - 16;
        yes_x = CONFIRM_X + CONFIRM_W / 2 - CONFIRM_BTN_W - 20;
        no_x  = CONFIRM_X + CONFIRM_W / 2 + 20;

        /* Yes */
        draw_rect_gl(yes_x, btn_cy, CONFIRM_BTN_W, CONFIRM_BTN_H,
                     1.0f, 0.5f, 0.15f, 0.8f);
        {
            int yw = font_string_width("Yes", 2);
            font_draw_string("Yes", yes_x + (CONFIRM_BTN_W - yw) / 2,
                             btn_cy + 8, 2, 1.0f, 1.0f, 1.0f, 1.0f);
        }

        /* No */
        draw_rect_gl(no_x, btn_cy, CONFIRM_BTN_W, CONFIRM_BTN_H,
                     0.3f, 0.3f, 0.3f, 0.8f);
        {
            int nw = font_string_width("No", 2);
            font_draw_string("No", no_x + (CONFIRM_BTN_W - nw) / 2,
                             btn_cy + 8, 2, 1.0f, 0.5f, 0.15f, 1.0f);
        }
    }

    /* Auto-save warning dialog (drawn ON TOP of everything) */
    if (g_autosave_warn_visible) {
        int yes_x, no_x, btn_cy;
        int tw;
        static const char *warn_lines[] = {
            "If you already have a save file,",
            "this will overwrite it",
            "automatically when exiting a game",
            "to return to the ROM select",
            "screen. Are you sure you want to",
            NULL
        };
        const char *last_line;
        int i, text_y;

        if (g_autosave_warn_action == 0)
            last_line = "enable auto-save?";
        else
            last_line = "disable Ask Before Saving?";

        if (!g_options_popup_visible && !g_confirm_visible) {
            draw_rect_gl(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0.0f, 0.0f, 0.0f, 0.6f);
        }

        /* Box with orange border */
        draw_rect_gl(ASWARN_X - 1, ASWARN_Y - 1, ASWARN_W + 2, ASWARN_H + 2,
                     1.0f, 0.5f, 0.15f, 1.0f);
        draw_rect_gl(ASWARN_X, ASWARN_Y, ASWARN_W, ASWARN_H,
                     0.0f, 0.0f, 0.0f, 0.95f);

        /* Warning text (centered, orange, 6 lines) */
        text_y = ASWARN_Y + 16;
        for (i = 0; warn_lines[i] != NULL; i++) {
            tw = font_string_width(warn_lines[i], 2);
            font_draw_string(warn_lines[i], ASWARN_X + (ASWARN_W - tw) / 2,
                             text_y, 2, 1.0f, 0.5f, 0.15f, 1.0f);
            text_y += 18;
        }
        /* Last line (dynamic) */
        tw = font_string_width(last_line, 2);
        font_draw_string(last_line, ASWARN_X + (ASWARN_W - tw) / 2,
                         text_y, 2, 1.0f, 0.5f, 0.15f, 1.0f);

        /* Yes / No buttons */
        btn_cy = ASWARN_Y + ASWARN_H - CONFIRM_BTN_H - 16;
        yes_x = ASWARN_X + ASWARN_W / 2 - CONFIRM_BTN_W - 20;
        no_x  = ASWARN_X + ASWARN_W / 2 + 20;

        /* Yes */
        draw_rect_gl(yes_x, btn_cy, CONFIRM_BTN_W, CONFIRM_BTN_H,
                     1.0f, 0.5f, 0.15f, 0.8f);
        {
            int yw = font_string_width("Yes", 2);
            font_draw_string("Yes", yes_x + (CONFIRM_BTN_W - yw) / 2,
                             btn_cy + 8, 2, 1.0f, 1.0f, 1.0f, 1.0f);
        }

        /* No */
        draw_rect_gl(no_x, btn_cy, CONFIRM_BTN_W, CONFIRM_BTN_H,
                     0.3f, 0.3f, 0.3f, 0.8f);
        {
            int nw = font_string_width("No", 2);
            font_draw_string("No", no_x + (CONFIRM_BTN_W - nw) / 2,
                             btn_cy + 8, 2, 1.0f, 0.5f, 0.15f, 1.0f);
        }
    }

    glDisable(GL_BLEND);
}

/* Handle touch events when popup/confirm is visible */
static void input_popup_handle_touch(int x, int y)
{
    /* Auto-save warning dialog takes highest priority */
    if (g_autosave_warn_visible) {
        int btn_cy = ASWARN_Y + ASWARN_H - CONFIRM_BTN_H - 16;
        int yes_x = ASWARN_X + ASWARN_W / 2 - CONFIRM_BTN_W - 20;
        int no_x  = ASWARN_X + ASWARN_W / 2 + 20;

        if (point_in_rect(x, y, yes_x, btn_cy, CONFIRM_BTN_W, CONFIRM_BTN_H)) {
            /* Apply the pending action */
            if (g_autosave_warn_action == 0) {
                g_autosave = 1;
            } else {
                g_autosave_ask = 0;
            }
            g_autosave_warn_result = 1;
            g_autosave_warn_visible = 0;
            filepicker_save_settings();
        } else if (point_in_rect(x, y, no_x, btn_cy, CONFIRM_BTN_W, CONFIRM_BTN_H)) {
            g_autosave_warn_result = 0;
            g_autosave_warn_visible = 0;
        }
        return;
    }

    /* Confirm dialog takes priority */
    if (g_confirm_visible) {
        int btn_cy = CONFIRM_Y + CONFIRM_H - CONFIRM_BTN_H - 16;
        int yes_x = CONFIRM_X + CONFIRM_W / 2 - CONFIRM_BTN_W - 20;
        int no_x  = CONFIRM_X + CONFIRM_W / 2 + 20;

        if (point_in_rect(x, y, yes_x, btn_cy, CONFIRM_BTN_W, CONFIRM_BTN_H)) {
            g_confirm_result = 1;
        } else if (point_in_rect(x, y, no_x, btn_cy, CONFIRM_BTN_W, CONFIRM_BTN_H)) {
            g_confirm_result = 0;
        }
        return;
    }

    /* Options popup */
    if (g_options_popup_visible) {
        int row;
        int btn_x = IGPOPUP_X + IGPOPUP_W - IGPOPUP_PAD - IGPOPUP_BTN_W;

        /* Check if touch is inside popup box */
        if (!point_in_rect(x, y, IGPOPUP_X, IGPOPUP_Y, IGPOPUP_W, IGPOPUP_H)) {
            /* Tap outside popup -> close and save settings */
            g_options_popup_visible = 0;
            filepicker_save_settings();
            return;
        }

        /* Hit-test row buttons */
        for (row = 0; row < IGPOPUP_ROWS; row++) {
            int row_y = IGPOPUP_Y + IGPOPUP_PAD + IGPOPUP_TITLE_H + IGPOPUP_PAD + row * IGPOPUP_ROW_H;
            int btn_y = row_y + (IGPOPUP_ROW_H - IGPOPUP_BTN_H) / 2;

            if (!point_in_rect(x, y, btn_x, btn_y, IGPOPUP_BTN_W, IGPOPUP_BTN_H))
                continue;

            switch (row) {
                case 0: /* Auto-Save toggle */
                    if (!g_autosave && !g_autosave_ask) {
                        /* Turning ON with Ask OFF — show warning */
                        g_autosave_warn_action = 0;
                        g_autosave_warn_result = -1;
                        g_autosave_warn_visible = 1;
                    } else {
                        g_autosave = !g_autosave;
                    }
                    break;
                case 1: /* Ask Before Saving (only if autosave ON) */
                    if (g_autosave) {
                        if (g_autosave_ask) {
                            /* Turning OFF with autosave ON — show warning */
                            g_autosave_warn_action = 1;
                            g_autosave_warn_result = -1;
                            g_autosave_warn_visible = 1;
                        } else {
                            g_autosave_ask = 1;
                        }
                    }
                    break;
                case 2: /* Control Brightness cycle */
                    g_control_dim = (g_control_dim + 1) % 3;
                    break;
                case 3: /* Scanlines toggle */
                    video_set_scanlines(!video_get_scanlines());
                    break;
                case 4: /* Scanline Brightness cycle */
                    video_set_scanline_brightness((video_get_scanline_brightness() + 1) % 3);
                    break;
                case 5: /* Palette cycle */
                    video_set_maria_palette((video_get_maria_palette() + 1) % 3);
                    break;
                case 6: /* Bug Report email */
                    g_options_popup_visible = 0;
                    filepicker_save_settings();
                    {
                        typedef int (*ServiceCallFunc)(const char *, const char *);
                        ServiceCallFunc fn = (ServiceCallFunc)dlsym(RTLD_DEFAULT, "PDL_ServiceCall");
                        if (fn) fn("palm://com.palm.applicationManager/open",
                                   "{\"target\":\"mailto:alanmorford@gmail.com?subject=EMU7800%20bug%20report\"}");
                    }
                    break;
            }
            break;  /* Only process one row */
        }
    }
}
