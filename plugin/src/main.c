/*
 * main.c
 *
 * EMU7800 PDK Application Entry Point
 * Standalone PDK app for webOS
 *
 * Emulation runs directly in the main loop, driven by vsync.
 * Touch events update lock-free input state.
 * App starts in file picker, launches emulator on ROM selection.
 *
 * Copyright (c) 2024 EMU7800
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <pthread.h>
#include <SDL.h>
#include <GLES/gl.h>
#include "machine.h"
#include "tia.h"
#include "video.h"
#include "audio.h"
#include "input.h"
#include "font.h"
#include "filepicker.h"
#include "savestate.h"
#include "device.h"
#include "sw_render.h"

/* App states */
#define APP_STATE_FILEPICKER 0
#define APP_STATE_EMULATOR   1

/* Global state */
static volatile int g_running = 0;
static volatile int g_emulator_paused = 0;
static int g_paused_for_popup = 0;
static SDL_Surface *g_screen = NULL;
static int g_app_state = APP_STATE_FILEPICKER;

/* Performance instrumentation — accumulated per reporting interval */
static uint64_t g_last_perf_log_time = 0;
static uint64_t g_poll_time_total = 0;
static uint64_t g_poll_time_max = 0;
static uint64_t g_upload_time_total = 0;
static uint64_t g_upload_time_max = 0;
static uint64_t g_swap_time_total = 0;
static uint64_t g_swap_time_max = 0;
static uint64_t g_frame_time_max = 0;
static int g_slow_frames = 0;
static int g_poll_event_count = 0;
static int g_perf_frame_count = 0;
static int g_touch_dedup_count = 0;

/* Touch-active tracking: >0 when any finger is touching the screen */
static volatile int g_touch_active = 0;

/* Touch dedup: ignore duplicate MOUSEMOTION for same finger within 8ms */
#define TOUCH_DEDUP_WINDOW_US 8000
typedef struct {
    int x, y;
    uint64_t time_us;
} TouchDedup;
static TouchDedup g_touch_dedup[8];  /* Per finger ID (0-7) */

/* Current ROM path for save state */
static char g_current_rom_path[512] = {0};

/*
 * Pre3 emulator thread — runs machine_run_frame() + audio_update() independently
 * of the SW render loop, because SDL_SWSURFACE flip has no vsync.  On the
 * TouchPad the inline path is fine (SDL_GL_SwapBuffers blocks on vsync).
 */
static pthread_t         g_emu_thread;
static volatile int      g_emu_thread_running = 0;
static volatile int      g_emu_frame_active   = 0;

/* Frame handoff (Pre3): set by emulator thread, cleared by render thread */
static volatile int      g_frame_ready    = 0;
static volatile int      g_frames_produced = 0;
static volatile int      g_frames_rendered = 0;
static volatile int      g_frames_skipped  = 0;


/*
 * Get current time in microseconds using gettimeofday.
 * More portable than clock_gettime on older systems like webOS.
 */
static uint64_t get_time_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

void log_msg(const char *msg)
{
    static FILE *logf = NULL;
    static int log_bytes = 0;
    if (!logf) {
        logf = fopen("/media/internal/emu7800.log", "w");
        if (!logf) return;
    }
    if (log_bytes > 2000000) return;
    log_bytes += fprintf(logf, "%s\n", msg);
    fflush(logf);
}

/* Forward declarations */
static int init_sdl(void);
static void shutdown_sdl(void);
static void main_loop(void);
static uint64_t process_events(void);
static void launch_selected_rom(void);
static void return_to_filepicker(void);
static void emu_thread_start(void);
static void emu_thread_stop(void);
static void emu_thread_wait_idle(void);

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    log_msg("EMU7800 starting... [build: emu7800-v1.8.1]");

    /* Detect device and initialize PDL (all PDL calls via dlsym in device.c) */
    device_init();

    /* Initialize SDL */
    log_msg("Calling init_sdl...");
    if (init_sdl() != 0) {
        device_pdl_quit();
        return 1;
    }
    log_msg("init_sdl done");

    /* Disable screen timeout */
    device_pdl_screen_timeout(0);

    /* Initialize font system (requires GL) */
    if (device_has_gl()) {
        log_msg("font_init...");
        font_init();
        log_msg("font_init OK");
    }

    /* Initialize machine */
    machine_init();

    /* Initialize file picker and scan for ROMs */
    filepicker_init();
    {
        const char *start_dir = filepicker_get_default_romdir();
        filepicker_scan(start_dir ? start_dir : "/media/internal/");
    }

    /* Start in file picker state */
    g_app_state = APP_STATE_FILEPICKER;
    g_running = 1;

    /* Enter main loop (handles events, emulation, and rendering) */
    main_loop();

    /* Ensure Pre3 emulator thread is stopped before tearing down machine */
    if (g_emu_thread_running) {
        emu_thread_stop();
    }

    /* Cleanup */
    filepicker_shutdown();
    if (device_has_gl()) {
        font_shutdown();
    }
    video_shutdown();
    audio_shutdown();
    machine_shutdown();
    shutdown_sdl();
    device_pdl_quit();

    return 0;
}

static int init_sdl(void)
{
    char errmsg[256];
    int sw = device_screen_width();
    int sh = device_screen_height();

    /* Initialize SDL with video and audio */
    log_msg("SDL_Init VIDEO|AUDIO...");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        log_msg("SDL_Init FAILED");
        return -1;
    }
    log_msg("SDL_Init OK");

    if (device_has_gl()) {
        /*
         * TouchPad path: OpenGL ES via SDL
         * - Set GL attributes BEFORE SDL_SetVideoMode
         * - Use SDL_OPENGL flag
         */
        char vmode_msg[128];

        log_msg("Setting SDL GL attributes for GLES 1.1...");
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 5);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 6);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 5);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_SWAP_CONTROL, 1);  /* VSYNC enabled */

        snprintf(vmode_msg, sizeof(vmode_msg),
                 "SDL_SetVideoMode %dx%d OPENGL|FULLSCREEN...", sw, sh);
        log_msg(vmode_msg);
        g_screen = SDL_SetVideoMode(sw, sh, 0,
                                    SDL_OPENGL | SDL_FULLSCREEN);
        if (g_screen == NULL) {
            snprintf(errmsg, sizeof(errmsg), "SDL_SetVideoMode GL FAILED: %s",
                     SDL_GetError());
            log_msg(errmsg);
            SDL_Quit();
            return -1;
        }
        log_msg("SDL_SetVideoMode OK (GL)");

        /* Set up basic OpenGL state */
        log_msg("Setting up OpenGL state...");
        glViewport(0, 0, sw, sh);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrthof(0, sw, sh, 0, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        glDisable(GL_DEPTH_TEST);
        glDisable(GL_LIGHTING);
        glEnable(GL_TEXTURE_2D);

        /* Clear both buffers */
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        SDL_GL_SwapBuffers();
        glClear(GL_COLOR_BUFFER_BIT);
        SDL_GL_SwapBuffers();
        log_msg("OpenGL state initialized");

        /* Initialize video subsystem (GL textures) */
        log_msg("video_init...");
        if (video_init(g_screen) != 0) {
            log_msg("video_init FAILED");
            SDL_Quit();
            return -1;
        }
        log_msg("video_init OK");
    } else {
        /*
         * Pre3 path: Software surface (no OpenGL)
         * webOS 2.x SDL does not support SDL_OPENGL flag.
         * Physical surface is portrait (480x800); sw_render rotates to
         * logical landscape (800x480).
         */
        int phys_w = 480, phys_h = 800;
        char vmode_msg[128];

        snprintf(vmode_msg, sizeof(vmode_msg),
                 "SDL_SetVideoMode %dx%d SWSURFACE...", phys_w, phys_h);
        log_msg(vmode_msg);
        g_screen = SDL_SetVideoMode(phys_w, phys_h, 16, SDL_SWSURFACE);
        if (g_screen == NULL) {
            snprintf(errmsg, sizeof(errmsg), "SDL_SetVideoMode SW FAILED: %s",
                     SDL_GetError());
            log_msg(errmsg);
            SDL_Quit();
            return -1;
        }
        {
            char info[128];
            snprintf(info, sizeof(info),
                     "SDL_SetVideoMode OK (SW): %dx%d bpp=%d",
                     g_screen->w, g_screen->h,
                     g_screen->format->BitsPerPixel);
            log_msg(info);
        }

        /* Initialize software renderer (landscape rotation) */
        sw_init(g_screen);
        sw_clear(0, 0, 0);
        sw_flip();
        log_msg("Pre3: sw_init OK");

        /* Initialize video (palette LUTs only, no GL textures) */
        video_init(g_screen);
        log_msg("Pre3: video_init OK (palettes only)");
    }

    SDL_ShowCursor(SDL_DISABLE);

    /* Initialize audio subsystem */
    log_msg("audio_init...");
    if (audio_init() != 0) {
        log_msg("audio_init FAILED");
        if (device_has_gl()) video_shutdown();
        SDL_Quit();
        return -1;
    }
    log_msg("audio_init OK");

    /* Initialize input subsystem */
    input_init();
    log_msg("All init complete");

    return 0;
}

static void shutdown_sdl(void)
{
    SDL_Quit();
}

/* --- Pre3 emulator thread ------------------------------------------------ */

static void *emu_thread_func(void *arg)
{
    uint64_t next_frame_time;
    uint64_t now;
    const uint64_t target_frame_us = 16683ULL; /* ~59.94 fps NTSC */
    (void)arg;

    now = get_time_us();
    next_frame_time = now + target_frame_us;

    while (g_emu_thread_running) {
        if (g_emulator_paused) {
            usleep(50000);
            now = get_time_us();
            next_frame_time = now + target_frame_us;
            g_frame_ready = 0;
            continue;
        }

        /* Wait until next frame time: coarse sleep then spin for precision */
        now = get_time_us();
        if (now < next_frame_time) {
            uint64_t wait_time = next_frame_time - now;
            if (wait_time > 1000)
                usleep((uint32_t)(wait_time - 500));
            else if (wait_time > 100)
                usleep(100);
            while (get_time_us() < next_frame_time) { /* spin */ }
        }

        /* Advance deadline; if we're more than one frame late, reset (no catch-up) */
        now = get_time_us();
        if (now > next_frame_time + target_frame_us)
            next_frame_time = now + target_frame_us;
        else
            next_frame_time += target_frame_us;

        /* If the renderer hasn't consumed the last frame, count the drop */
        if (g_frame_ready)
            g_frames_skipped++;

        if (machine_is_loaded()) {
            g_emu_frame_active = 1;
            machine_run_frame();
            audio_update();
            g_emu_frame_active = 0;
            __sync_synchronize(); /* ARM DMB: stores visible before flag */
            g_frame_ready = 1;
            g_frames_produced++;
        }
    }
    g_emu_frame_active = 0;
    return NULL;
}

static void emu_thread_start(void)
{
    g_frame_ready = 0;
    g_emu_thread_running = 1;
    pthread_create(&g_emu_thread, NULL, emu_thread_func, NULL);
    log_msg("Pre3: emulator thread started");
}

static void emu_thread_stop(void)
{
    g_emu_thread_running = 0;
    pthread_join(g_emu_thread, NULL);
    log_msg("Pre3: emulator thread stopped");
}

/* Block until the emulator thread finishes its current frame (max 50 ms).
 * Call after setting g_emulator_paused=1, before touching machine state. */
static void emu_thread_wait_idle(void)
{
    int i;
    for (i = 0; i < 50 && g_emu_frame_active; i++)
        usleep(1000);
}

/* --- ROM launch / return ------------------------------------------------- */

/* Launch the selected ROM from file picker */
static void launch_selected_rom(void)
{
    const char *path = filepicker_get_selected_path();
    int type = filepicker_get_selected_type();
    int load_result;
    char msg[256];

    if (!path) {
        log_msg("launch_selected_rom: no path");
        return;
    }

    /* Final file-existence check — catches all code paths */
    if (access(path, F_OK) != 0) {
        log_msg("launch_selected_rom: file not found");
        filepicker_show_notfound();
        return;
    }

    snprintf(msg, sizeof(msg), "Loading ROM: %s (type=%d)", path, type);
    log_msg(msg);

    /* Store ROM path for save state */
    strncpy(g_current_rom_path, path, sizeof(g_current_rom_path) - 1);
    g_current_rom_path[sizeof(g_current_rom_path) - 1] = '\0';

    /* Re-init machine for clean state */
    machine_shutdown();
    machine_init();

    load_result = machine_load_rom(path, type);
    snprintf(msg, sizeof(msg), "machine_load_rom returned: %d", load_result);
    log_msg(msg);

    if (load_result != 0) {
        log_msg("Failed to load ROM!");
        return;
    }

    log_msg("ROM loaded successfully, starting emulator");

    /* Load save state if user chose "Yes" from save popup */
    if (filepicker_should_load_save()) {
        log_msg("Loading save state (user requested)");
        savestate_load(path);
    }

    /* Store for resume */
    filepicker_set_last_rom(path, type);

    /* Reset input state */
    input_init();

    /* Transfer keyboard detection from filepicker to emulator */
    if (filepicker_keyboard_detected()) {
        input_set_keyboard_active(1);
    }

    /* Check if save file exists for this ROM */
    input_set_save_exists(savestate_exists(path));

    /* Resume audio */
    audio_resume();

    /* Pre3: start the emulator thread (SW path has no vsync for pacing) */
    if (!device_has_gl()) {
        emu_thread_start();
    }

    g_emulator_paused = 0;
    g_app_state = APP_STATE_EMULATOR;
}

/* Return to file picker from emulator */
static void return_to_filepicker(void)
{
    log_msg("Returning to file picker");

    /* Stop emulator thread before touching machine state (Pre3 only) */
    if (!device_has_gl() && g_emu_thread_running) {
        emu_thread_stop();
    }

    /* Pause audio */
    audio_pause();

    /* Re-scan the directory we launched from (preserves scroll position) */
    filepicker_rescan();

    g_app_state = APP_STATE_FILEPICKER;
}

/*
 * Main loop - handles SDL events, emulation, and rendering.
 *
 * Emulation runs directly in this loop, one frame per vsync.
 * Vsync (SDL_GL_SWAP_CONTROL=1) provides hardware-accurate 60Hz pacing.
 * Audio is fed immediately after each emulated frame, in the same thread,
 * eliminating timing jitter between the CPU/TIA and audio domains.
 *
 * In FILEPICKER state: draws file picker, handles touch for selection.
 * In EMULATOR state: emulates one frame, renders, handles input.
 */
static void main_loop(void)
{
    uint64_t iter_start;

    g_last_perf_log_time = get_time_us();

    while (g_running) {
        iter_start = get_time_us();

        /* Process SDL events (returns elapsed microseconds) */
        {
            uint64_t poll_us = process_events();
            g_poll_time_total += poll_us;
            if (poll_us > g_poll_time_max)
                g_poll_time_max = poll_us;
        }

        if (g_app_state == APP_STATE_FILEPICKER) {
            /* Draw file picker and sleep a bit to avoid burning CPU */
            filepicker_draw();
            usleep(16000); /* ~60fps */
        } else if (g_app_state == APP_STATE_EMULATOR) {
            /* Check for BACK button */
            if (input_back_pressed()) {
                if (input_get_autosave() && g_current_rom_path[0]) {
                    if (input_get_autosave_ask()) {
                        /* Show confirm dialog, pause emu */
                        g_emulator_paused = 1;
                        audio_pause();
                        g_paused_for_popup = 1;
                        input_show_confirm();
                    } else {
                        /* Auto-save silently, then return */
                        g_emulator_paused = 1;
                        audio_pause();
                        usleep(50000);
                        savestate_save(g_current_rom_path);
                        return_to_filepicker();
                        continue;
                    }
                } else {
                    return_to_filepicker();
                    continue;
                }
            }

            /* Handle confirm result */
            {
                int r = input_confirm_result();
                if (r == 1) {       /* Yes */
                    usleep(50000);
                    savestate_save(g_current_rom_path);
                    g_paused_for_popup = 0;
                    return_to_filepicker();
                    continue;
                } else if (r == 0) { /* No */
                    g_paused_for_popup = 0;
                    return_to_filepicker();
                    continue;
                }
            }

            /* Check for OPTIONS button */
            if (input_options_pressed()) {
                g_emulator_paused = 1;
                audio_pause();
                g_paused_for_popup = 1;
            }

            /* Auto-unpause when popup closes */
            if (g_paused_for_popup && !input_options_popup_visible() && !input_confirm_visible() && !input_autosave_warn_visible()) {
                g_paused_for_popup = 0;
                g_emulator_paused = 0;
                audio_resume();
            }

            /* Check for PAUSE button (not while popup is up) */
            if (input_pause_pressed() && !g_paused_for_popup) {
                if (g_emulator_paused) {
                    log_msg("Unpausing emulator");
                    g_emulator_paused = 0;
                    audio_resume();
                } else {
                    log_msg("Pausing emulator");
                    g_emulator_paused = 1;
                    audio_pause();
                }
            }

            /* Check for SAVE button */
            if (input_save_pressed() && g_current_rom_path[0]) {
                log_msg("Save state requested");
                g_emulator_paused = 1;
                audio_pause();
                if (!device_has_gl()) emu_thread_wait_idle();
                if (savestate_save(g_current_rom_path) == 0) {
                    input_show_notification("Saved");
                    input_set_save_exists(1);
                }
                g_emulator_paused = 0;
                audio_resume();
            }

            /* Check for LOAD button */
            if (input_load_pressed() && g_current_rom_path[0]) {
                if (savestate_exists(g_current_rom_path)) {
                    log_msg("Load state requested");
                    g_emulator_paused = 1;
                    audio_pause();
                    if (!device_has_gl()) emu_thread_wait_idle();
                    if (savestate_load(g_current_rom_path) == 0) {
                        input_show_notification("Loaded");
                    }
                    g_emulator_paused = 0;
                    audio_resume();
                }
            }

            /* Check for ZOOM button */
            if (input_zoom_pressed()) {
                video_cycle_zoom();
                input_show_notification(video_get_zoom_label());
            }

            /*
             * Run one frame of emulation and feed audio — in the same thread,
             * in the same timing domain as the vsync swap below.
             * This ensures audio is generated at a predictable rate with no
             * cross-thread jitter between CPU/TIA and the audio ring buffer.
             */
            if (!g_emulator_paused && machine_is_loaded()) {
                if (device_has_gl()) {
                    /* TouchPad: run inline, paced by SDL_GL_SwapBuffers vsync */
                    machine_run_frame();
                    audio_update();
                }
                /* Pre3: emulator thread handles machine_run_frame/audio_update */
            }

            if (device_has_gl()) {
                int popup_vis = input_options_popup_visible() || input_confirm_visible() || input_autosave_warn_visible();

                if (machine_is_loaded()) {
                    uint64_t upload_t0, upload_us, swap_t0, swap_us;

                    /* Phase 1: convert + upload + draw (CPU + GPU work) */
                    upload_t0 = get_time_us();
                    video_render_upload();
                    upload_us = get_time_us() - upload_t0;

                    g_upload_time_total += upload_us;
                    if (upload_us > g_upload_time_max)
                        g_upload_time_max = upload_us;

                    /* Touch-aware swap: 1ms delay lets compositor finish GPU
                     * work before we request a buffer swap, reducing the
                     * chance of a multi-frame stall (45-80ms spikes). */
                    if (g_touch_active > 0) {
                        usleep(1000);
                    }

                    /* Phase 2: swap buffers (blocks on VSYNC — this is our
                     * 60Hz clock; emulation above runs once per swap) */
                    swap_t0 = get_time_us();
                    video_render_swap();
                    swap_us = get_time_us() - swap_t0;

                    g_swap_time_total += swap_us;
                    if (swap_us > g_swap_time_max)
                        g_swap_time_max = swap_us;

                    input_tick();
                    g_perf_frame_count++;

                    /* Throttle when popup visible and paused */
                    if (popup_vis && g_emulator_paused)
                        usleep(33000);  /* ~30fps while popup shown */
                }
            } else {
                /* Pre3: software rendering path */
                if (machine_is_loaded()) {
                    int popup_vis = input_options_popup_visible() || input_confirm_visible() || input_autosave_warn_visible();

                    if (g_frame_ready || popup_vis) {
                        __sync_synchronize(); /* ARM DMB: see all emulator stores */

                        if (!video_sw_is_fullscreen())
                            sw_clear(0, 0, 0);
                        video_render_frame_sw();
                        input_draw_controls_sw();
                        input_draw_popup_sw();

                        if (g_touch_active > 0)
                            usleep(1000);

                        sw_flip();

                        input_tick();

                        if (g_frame_ready) {
                            g_frame_ready = 0;
                            g_frames_rendered++;
                        }
                        g_perf_frame_count++;

                        if (popup_vis && g_emulator_paused)
                            usleep(33000);
                    } else {
                        usleep(500); /* nothing ready — yield briefly */
                    }
                }
            }

            /* Track worst iteration time and slow frames */
            {
                uint64_t iter_us = get_time_us() - iter_start;
                if (iter_us > g_frame_time_max)
                    g_frame_time_max = iter_us;
                if (iter_us > 16000)
                    g_slow_frames++;
            }

            /* Log performance summary every ~5 seconds */
            {
                uint64_t now = get_time_us();
                if (now - g_last_perf_log_time >= 5000000ULL && g_perf_frame_count > 0) {
                    char msg[512];
                    int cb_fr, cb_lr, cb_fc, cb_lc;
                    int perf_vbo = tia_get_vblank_off_scanline();
                    int perf_active = tia_get_active_height();
                    int perf_disp_off = 0;
                    int perf_disp_h;
                    if (perf_vbo >= 0 && perf_vbo < 31) {
                        perf_disp_off = 31 - perf_vbo;
                        if (perf_disp_off >= perf_active) perf_disp_off = 0;
                    }
                    perf_disp_h = perf_active - perf_disp_off;
                    if (perf_disp_off > 0 && perf_disp_h > 210) perf_disp_h = 210;

                    snprintf(msg, sizeof(msg),
                        "PERF: poll_avg=%lluus poll_max=%lluus "
                        "upload_avg=%lluus upload_max=%lluus "
                        "swap_avg=%lluus swap_max=%lluus "
                        "frame_max=%lluus slow=%d events=%d dedup=%d frames=%d "
                        "active_h=%d vbo_sl=%d disp_off=%d disp_h=%d",
                        (unsigned long long)(g_poll_time_total / g_perf_frame_count),
                        (unsigned long long)g_poll_time_max,
                        (unsigned long long)(g_upload_time_total / g_perf_frame_count),
                        (unsigned long long)g_upload_time_max,
                        (unsigned long long)(g_swap_time_total / g_perf_frame_count),
                        (unsigned long long)g_swap_time_max,
                        (unsigned long long)g_frame_time_max,
                        g_slow_frames, g_poll_event_count,
                        g_touch_dedup_count, g_perf_frame_count,
                        perf_active, perf_vbo, perf_disp_off, perf_disp_h);
                    log_msg(msg);

                    if (machine_get_type() != MACHINE_7800 &&
                        tia_scan_content_bounds(&cb_fr, &cb_lr, &cb_fc, &cb_lc) == 0) {
                        snprintf(msg, sizeof(msg),
                            "CONTENT: rows=[%d..%d] cols=[%d..%d] "
                            "content_h=%d content_w=%d "
                            "disp_row=[%d..%d] top_gap=%d bot_gap=%d left_gap=%d",
                            cb_fr, cb_lr, cb_fc, cb_lc,
                            cb_lr - cb_fr + 1, cb_lc - cb_fc + 1,
                            cb_fr - perf_disp_off, cb_lr - perf_disp_off,
                            cb_fr - perf_disp_off,
                            perf_disp_h - 1 - (cb_lr - perf_disp_off),
                            cb_fc);
                        log_msg(msg);
                    }

                    /* Reset accumulators */
                    g_poll_time_total = 0;
                    g_poll_time_max = 0;
                    g_upload_time_total = 0;
                    g_upload_time_max = 0;
                    g_swap_time_total = 0;
                    g_swap_time_max = 0;
                    g_frame_time_max = 0;
                    g_slow_frames = 0;
                    g_poll_event_count = 0;
                    g_touch_dedup_count = 0;
                    g_perf_frame_count = 0;
                    g_last_perf_log_time = now;
                }
            }
        }
    }

}

/*
 * Process SDL events.
 * Routes touch events to file picker or input based on app state.
 */
static uint64_t process_events(void)
{
    SDL_Event event;
    int events_processed = 0;
    int picker_had_motion = 0;
    int picker_motion_x = 0, picker_motion_y = 0;
    uint64_t poll_start = get_time_us();

    while (events_processed < 32) {
        int has_event;
        int tx, ty;  /* transformed touch coords */

        /* Time-based throttle: don't let PollEvent starve the renderer */
        if (get_time_us() - poll_start > 2000) break;  /* 2ms max */

        has_event = SDL_PollEvent(&event);
        if (!has_event) break;

        switch (event.type) {
            case SDL_QUIT:
                g_running = 0;
                events_processed++;
                break;

            case SDL_MOUSEBUTTONDOWN:
                g_touch_active++;
                /* Pre3: rotate portrait touch to landscape coords */
                if (!device_has_gl()) {
                    tx = event.button.y;
                    ty = 479 - event.button.x;
                } else {
                    tx = event.button.x;
                    ty = event.button.y;
                }
                if (g_app_state == APP_STATE_FILEPICKER) {
                    filepicker_touch_down(tx, ty);
                } else {
                    input_handle_touch_down(event.button.which, tx, ty);
                }
                events_processed++;
                break;

            case SDL_MOUSEBUTTONUP:
                if (g_touch_active > 0) g_touch_active--;
                if (!device_has_gl()) {
                    tx = event.button.y;
                    ty = 479 - event.button.x;
                } else {
                    tx = event.button.x;
                    ty = event.button.y;
                }
                if (g_app_state == APP_STATE_FILEPICKER) {
                    if (filepicker_touch_up(tx, ty)) {
                        launch_selected_rom();
                    }
                } else {
                    input_handle_touch_up(event.button.which, tx, ty);
                }
                events_processed++;
                break;

            case SDL_MOUSEMOTION:
                if (!device_has_gl()) {
                    tx = event.motion.y;
                    ty = 479 - event.motion.x;
                } else {
                    tx = event.motion.x;
                    ty = event.motion.y;
                }
                if (g_app_state == APP_STATE_FILEPICKER) {
                    picker_had_motion = 1;
                    picker_motion_x = tx;
                    picker_motion_y = ty;
                } else {
                    /* Touch dedup: skip identical position within time window */
                    int finger = event.motion.which & 7;  /* clamp to 0-7 */
                    uint64_t now = get_time_us();
                    if (tx == g_touch_dedup[finger].x &&
                        ty == g_touch_dedup[finger].y &&
                        now - g_touch_dedup[finger].time_us < TOUCH_DEDUP_WINDOW_US) {
                        g_touch_dedup_count++;
                    } else {
                        g_touch_dedup[finger].x = tx;
                        g_touch_dedup[finger].y = ty;
                        g_touch_dedup[finger].time_us = now;
                        input_handle_touch_move(event.motion.which, tx, ty);
                    }
                }
                events_processed++;
                break;

            case SDL_KEYDOWN:
                if (g_app_state == APP_STATE_FILEPICKER) {
                    if (filepicker_key_down(event.key.keysym.sym)) {
                        launch_selected_rom();
                    }
                } else if (g_app_state == APP_STATE_EMULATOR) {
                    input_handle_key_down(event.key.keysym.sym);
                }
                events_processed++;
                break;

            case SDL_KEYUP:
                if (g_app_state == APP_STATE_EMULATOR) {
                    input_handle_key_up(event.key.keysym.sym);
                }
                events_processed++;
                break;

            case SDL_ACTIVEEVENT:
                if (event.active.state & SDL_APPACTIVE) {
                    if (!event.active.gain) {
                        log_msg("Focus lost - pausing");
                        g_emulator_paused = 1;
                        audio_pause();
                    } else {
                        log_msg("Focus gained - resuming");
                        if (device_has_gl()) {
                            glClear(GL_COLOR_BUFFER_BIT);
                            SDL_GL_SwapBuffers();
                            glClear(GL_COLOR_BUFFER_BIT);
                        }
                        if (g_app_state == APP_STATE_EMULATOR) {
                            audio_resume();
                        }
                        g_emulator_paused = 0;
                    }
                }
                events_processed++;
                break;

            default:
                break;
        }
    }

    g_poll_event_count += events_processed;

    /* Coalesced motion for filepicker only */
    if (picker_had_motion) {
        filepicker_touch_move(picker_motion_x, picker_motion_y);
    }

    return get_time_us() - poll_start;
}

/* Start emulator (called from bridge.c JS handler) */
void main_start_emulator(void)
{
    g_emulator_paused = 0;
    audio_resume();
}

/* Stop emulator (called from bridge.c JS handler) */
void main_stop_emulator(void)
{
    g_emulator_paused = 1;
    audio_pause();
}

/* Check if emulator is running (called from bridge.c JS handler) */
int main_is_running(void)
{
    return g_app_state == APP_STATE_EMULATOR && !g_emulator_paused;
}
