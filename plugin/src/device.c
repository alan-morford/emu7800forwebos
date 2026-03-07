/*
 * device.c
 *
 * Runtime device detection and PDL abstraction.
 * All PDL calls go through dlsym — safe on both webOS 2.x and 3.x.
 *
 * Detection logic:
 *   - If PDL_GetHardwareID returns 601 → TouchPad (GL via SDL)
 *   - Everything else (501, missing PDL, etc.) → Pre3/non-TouchPad (SW only)
 *
 * The TouchPad is the ONLY device where SDL_OPENGL is known to work.
 * All other devices get the safe software rendering path.
 *
 * Copyright (c) 2024 EMU7800
 */

#define _GNU_SOURCE  /* for RTLD_DEFAULT */
#include <stdio.h>
#include <dlfcn.h>
#include "device.h"

extern void log_msg(const char *msg);

/* Device state */
static int g_device_type   = DEVICE_PRE3;  /* safe default */
static int g_screen_width  = 480;
static int g_screen_height = 800;
static int g_has_gl        = 0;            /* safe default: no GL */

/* PDL function pointers (resolved via dlsym) */
typedef int  (*PDL_InitFunc)(unsigned int);
typedef int  (*PDL_QuitFunc)(void);
typedef int  (*PDL_ScreenTimeoutFunc)(int);
typedef int  (*PDL_GetHWIDFunc)(void);
typedef int  (*PDL_SetTouchFunc)(int);

static PDL_QuitFunc          s_pdl_quit = NULL;
static PDL_ScreenTimeoutFunc s_pdl_timeout = NULL;

void device_init(void)
{
    PDL_InitFunc pdl_init;
    PDL_GetHWIDFunc pdl_gethwid;
    PDL_SetTouchFunc pdl_touch;
    int hwid;
    char msg[128];

    /* Resolve PDL_Init */
    pdl_init = (PDL_InitFunc)dlsym(RTLD_DEFAULT, "PDL_Init");
    if (pdl_init) {
        int ret = pdl_init(0);
        snprintf(msg, sizeof(msg), "DEVICE: PDL_Init(0) returned %d", ret);
        log_msg(msg);
    } else {
        log_msg("DEVICE: PDL_Init not found via dlsym (webOS 2.x or no PDL)");
    }

    /* Cache PDL_Quit and PDL_ScreenTimeoutEnable for later use */
    s_pdl_quit = (PDL_QuitFunc)dlsym(RTLD_DEFAULT, "PDL_Quit");
    s_pdl_timeout = (PDL_ScreenTimeoutFunc)dlsym(RTLD_DEFAULT, "PDL_ScreenTimeoutEnable");

    /* Enable aggressive multi-finger tracking (TouchPad only) */
    pdl_touch = (PDL_SetTouchFunc)dlsym(RTLD_DEFAULT, "PDL_SetTouchAggression");
    if (pdl_touch) {
        pdl_touch(1);
        log_msg("DEVICE: PDL_SetTouchAggression(1) OK");
    }

    /* Detect device via PDL_GetHardwareID */
    pdl_gethwid = (PDL_GetHWIDFunc)dlsym(RTLD_DEFAULT, "PDL_GetHardwareID");
    if (pdl_gethwid) {
        hwid = pdl_gethwid();
        snprintf(msg, sizeof(msg), "DEVICE: PDL_GetHardwareID=%d", hwid);
        log_msg(msg);

        if (hwid == 601) {
            /* HP TouchPad — the ONLY device with confirmed SDL+GL support */
            g_device_type   = DEVICE_TOUCHPAD;
            g_screen_width  = 1024;
            g_screen_height = 768;
            g_has_gl        = 1;
            log_msg("DEVICE: HP TouchPad (1024x768, GL)");
        } else if (hwid == 501) {
            /* Logical landscape dimensions (rotated from 480x800 portrait) */
            g_device_type   = DEVICE_PRE3;
            g_screen_width  = 800;
            g_screen_height = 480;
            g_has_gl        = 0;
            log_msg("DEVICE: HP Pre3 (800x480 logical landscape, SW)");
        } else {
            /* Unknown device — use safe Pre3-like defaults */
            snprintf(msg, sizeof(msg),
                     "DEVICE: Unknown hwid=%d, using Pre3 defaults", hwid);
            log_msg(msg);
            g_device_type   = DEVICE_PRE3;
            g_screen_width  = 800;
            g_screen_height = 480;
            g_has_gl        = 0;
        }
    } else {
        /* No GetHardwareID → older webOS → safe defaults */
        log_msg("DEVICE: PDL_GetHardwareID not found, using Pre3 defaults (800x480 logical, SW)");
        g_device_type   = DEVICE_PRE3;
        g_screen_width  = 800;
        g_screen_height = 480;
        g_has_gl        = 0;
    }
}

int device_screen_width(void)  { return g_screen_width; }
int device_screen_height(void) { return g_screen_height; }
int device_type(void)          { return g_device_type; }
int device_is_small(void)      { return g_device_type == DEVICE_PRE3; }
int device_has_gl(void)        { return g_has_gl; }

void device_pdl_quit(void)
{
    if (s_pdl_quit) {
        s_pdl_quit();
        log_msg("DEVICE: PDL_Quit called");
    }
}

void device_pdl_screen_timeout(int enable)
{
    if (s_pdl_timeout) {
        s_pdl_timeout(enable);
    }
}
