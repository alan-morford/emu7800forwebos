/*
 * device.c
 *
 * Runtime device detection and PDL abstraction.
 * All PDL calls go through dlsym — safe on both webOS 2.x and 3.x.
 *
 * Detection logic (PDL_GetHardwareID, confirmed on real hardware):
 *   - 601 (topaz)  -> HP TouchPad        1024x768, GL
 *   - 701 (opal)   -> HP TouchPad Go     1024x768, GL  (prototype 7" tablet)
 *   - 501 (manta)  -> HP Pre3            800x480 logical landscape, SW
 *   - anything else: fall back on PDL_GetPDKVersion + PDL_GetScreenMetrics.
 *     A webOS 3.x device reporting a 1024x768-or-larger screen is a
 *     TouchPad-class tablet and gets the GL path; everything else (webOS 2.x,
 *     small screen, missing PDL) gets the safe software rendering path.
 *
 * Copyright (c) 2024 EMU7800
 */

#define _GNU_SOURCE  /* for RTLD_DEFAULT */
#include <stdio.h>
#include <dlfcn.h>
#include "device.h"

extern void log_msg(const char *msg);

/* PDL_HardwareID values, confirmed by probing the actual hardware */
#define HWID_PRE3         501   /* mantaray  */
#define HWID_TOUCHPAD     601   /* topaz     */
#define HWID_TOUCHPAD_GO  701   /* opal      */

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
typedef int  (*PDL_GetMetricsFunc)(int *, int *);
typedef int  (*PDL_GetPDKVerFunc)(void);

static PDL_QuitFunc          s_pdl_quit = NULL;
static PDL_ScreenTimeoutFunc s_pdl_timeout = NULL;

static void select_touchpad(const char *name)
{
    char msg[128];
    g_device_type   = DEVICE_TOUCHPAD;
    g_screen_width  = 1024;
    g_screen_height = 768;
    g_has_gl        = 1;
    snprintf(msg, sizeof(msg), "DEVICE: %s (1024x768, GL)", name);
    log_msg(msg);
}

static void select_pre3(const char *name)
{
    char msg[128];
    /* Logical landscape dimensions (rotated from 480x800 portrait) */
    g_device_type   = DEVICE_PRE3;
    g_screen_width  = 800;
    g_screen_height = 480;
    g_has_gl        = 0;
    snprintf(msg, sizeof(msg), "DEVICE: %s (800x480 logical landscape, SW)", name);
    log_msg(msg);
}

void device_init(void)
{
    PDL_InitFunc pdl_init;
    PDL_GetHWIDFunc pdl_gethwid;
    PDL_SetTouchFunc pdl_touch;
    PDL_GetMetricsFunc pdl_metrics;
    PDL_GetPDKVerFunc pdl_pdkver;
    int hwid;
    int pdk_ver = 0;
    int scr_w = 0, scr_h = 0;
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

    /* PDK version + screen metrics: used to classify devices whose hardware ID
     * we do not recognize, so a new webOS 3.x tablet is not misfiled as a Pre3. */
    pdl_pdkver = (PDL_GetPDKVerFunc)dlsym(RTLD_DEFAULT, "PDL_GetPDKVersion");
    if (pdl_pdkver) {
        pdk_ver = pdl_pdkver();
    }
    pdl_metrics = (PDL_GetMetricsFunc)dlsym(RTLD_DEFAULT, "PDL_GetScreenMetrics");
    if (pdl_metrics && pdl_metrics(&scr_w, &scr_h) != 0) {
        scr_w = scr_h = 0;   /* call failed — treat as unknown */
    }
    snprintf(msg, sizeof(msg), "DEVICE: PDK version=%d, screen metrics=%dx%d",
             pdk_ver, scr_w, scr_h);
    log_msg(msg);

    /* Detect device via PDL_GetHardwareID */
    pdl_gethwid = (PDL_GetHWIDFunc)dlsym(RTLD_DEFAULT, "PDL_GetHardwareID");
    if (pdl_gethwid) {
        hwid = pdl_gethwid();
        snprintf(msg, sizeof(msg), "DEVICE: PDL_GetHardwareID=%d", hwid);
        log_msg(msg);

        if (hwid == HWID_TOUCHPAD) {
            select_touchpad("HP TouchPad");
        } else if (hwid == HWID_TOUCHPAD_GO) {
            /* Prototype 7" TouchPad Go: same panel, same webOS 3.x GL stack */
            select_touchpad("HP TouchPad Go");
        } else if (hwid == HWID_PRE3) {
            select_pre3("HP Pre3");
        } else if (pdk_ver >= 300 && scr_w >= 1024 && scr_h >= 768) {
            /* Unknown ID, but a webOS 3.x device with a TouchPad-sized screen —
             * the GL path is the right one for that class of hardware. */
            snprintf(msg, sizeof(msg),
                     "DEVICE: Unknown hwid=%d, webOS 3.x tablet-class", hwid);
            log_msg(msg);
            select_touchpad("Unknown TouchPad-class device");
        } else {
            snprintf(msg, sizeof(msg),
                     "DEVICE: Unknown hwid=%d, using Pre3 defaults", hwid);
            log_msg(msg);
            select_pre3("Unknown small device");
        }
    } else {
        /* No GetHardwareID -> older webOS -> safe defaults */
        log_msg("DEVICE: PDL_GetHardwareID not found, using Pre3 defaults");
        select_pre3("Unknown (no PDL_GetHardwareID)");
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
