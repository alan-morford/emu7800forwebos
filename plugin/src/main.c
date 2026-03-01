/*
 * main.c
 *
 * EMU7800 PDK Application Entry Point
 * Standalone PDK app for webOS
 *
 * Uses dedicated emulator thread for consistent timing.
 * Touch events update lock-free input state.
 * App starts in file picker, launches emulator on ROM selection.
 *
 * Copyright (c) 2024 EMU7800
 */

#define _GNU_SOURCE  /* for RTLD_DEFAULT */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <dlfcn.h>
#include <SDL.h>
#include <GLES/gl.h>
#include "PDL.h"
#include "machine.h"
#include "tia.h"
#include "video.h"
#include "audio.h"
#include "input.h"
#include "font.h"
#include "filepicker.h"
#include "savestate.h"

/* Screen dimensions for HP TouchPad */
#define SCREEN_WIDTH  1024
#define SCREEN_HEIGHT 768

/* App states */
#define APP_STATE_FILEPICKER 0
#define APP_STATE_EMULATOR   1

/* Global state - volatile for thread visibility */
static volatile int g_running = 0;
static volatile int g_emulator_running = 0;
static volatile int g_emulator_paused = 0;
static int g_paused_for_popup = 0;
static SDL_Surface *g_screen = NULL;
static pthread_t g_emu_thread;
static int g_emu_thread_created = 0;
static int g_app_state = APP_STATE_FILEPICKER;

/* Frame ready flag - set by emulator thread, cleared by render */
static volatile int g_frame_ready = 0;

/* Frame delivery diagnostics */
static volatile int g_frames_produced = 0;  /* Frames completed by emulator */
static volatile int g_frames_rendered = 0;  /* Frames displayed by renderer */
static volatile int g_frames_skipped = 0;   /* Frames dropped (renderer too slow) */

/* Performance instrumentation — accumulated per reporting interval */
static uint64_t g_last_perf_log_time = 0;
static uint64_t g_poll_time_total = 0;     /* Accumulated PollEvent time per interval */
static uint64_t g_poll_time_max = 0;       /* Worst single PollEvent call */
static uint64_t g_upload_time_total = 0;   /* Accumulated upload (convert+texsub+draw) time */
static uint64_t g_upload_time_max = 0;     /* Worst single upload call */
static uint64_t g_swap_time_total = 0;     /* Accumulated SwapBuffers time per interval */
static uint64_t g_swap_time_max = 0;       /* Worst single SwapBuffers call */
static uint64_t g_frame_time_max = 0;      /* Worst main-loop iteration */
static int g_slow_frames = 0;             /* Iterations > 16ms */
static int g_poll_event_count = 0;        /* Total events polled per interval */
static int g_perf_frame_count = 0;        /* Frames rendered in this interval */
static int g_touch_dedup_count = 0;       /* Motion events deduplicated */

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
    (void)msg;
}

/* Forward declarations */
static int init_sdl(void);
static void shutdown_sdl(void);
static void main_loop(void);
static uint64_t process_events(void);
static void *emulator_thread(void *data);
static void launch_selected_rom(void);
static void return_to_filepicker(void);

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    log_msg("EMU7800 starting... [build: emu7800-v1.4.0-20260219]");

    /* Initialize PDL */
    log_msg("Calling PDL_Init...");
    PDL_Init(0);

    /* Enable aggressive multi-finger tracking (only available on real device) */
    {
        typedef PDL_Err (*SetTouchAggressionFunc)(int);
        SetTouchAggressionFunc fn = (SetTouchAggressionFunc)dlsym(RTLD_DEFAULT, "PDL_SetTouchAggression");
        if (fn) {
            fn(1);
            log_msg("PDL_SetTouchAggression(1) OK");
        } else {
            log_msg("PDL_SetTouchAggression not available");
        }
    }
    log_msg("PDL_Init done");

    /* Initialize SDL */
    log_msg("Calling init_sdl...");
    if (init_sdl() != 0) {
        PDL_Quit();
        return 1;
    }
    log_msg("init_sdl done");

    /* Disable screen timeout */
    PDL_ScreenTimeoutEnable(PDL_FALSE);

    /* Initialize font system */
    log_msg("font_init...");
    font_init();
    log_msg("font_init OK");

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

    /* Enter main loop (handles events and rendering) */
    main_loop();

    /* Signal emulator thread to stop */
    g_running = 0;
    g_emulator_running = 0;
    if (g_emu_thread_created) {
        log_msg("Waiting for emulator thread...");
        pthread_join(g_emu_thread, NULL);
        {
            char msg[128];
            snprintf(msg, sizeof(msg),
                "FINAL FRAMESTATS: produced=%d rendered=%d skipped=%d",
                g_frames_produced, g_frames_rendered, g_frames_skipped);
            log_msg(msg);
        }
    }

    /* Cleanup */
    filepicker_shutdown();
    font_shutdown();
    audio_shutdown();
    video_shutdown();
    machine_shutdown();
    shutdown_sdl();
    PDL_Quit();

    return 0;
}

/*
 * Emulator thread - runs CPU, TIA, and audio at consistent 60 FPS.
 *
 * Uses simple frame pacing: run one frame, wait until next frame time.
 * No catch-up behavior - if we fall behind, we just maintain steady pace.
 */
static void *emulator_thread(void *data)
{
    uint64_t next_frame_time;
    uint64_t now;
    const uint64_t target_frame_us = 16683ULL; /* ~59.94 FPS (NTSC) */
    (void)data;

    now = get_time_us();
    next_frame_time = now + target_frame_us;

    while (g_running && g_emulator_running) {
        /* Skip if paused (app minimized) */
        if (g_emulator_paused) {
            usleep(50000);
            now = get_time_us();
            next_frame_time = now + target_frame_us;
            g_frame_ready = 0;
            continue;
        }

        /* Wait until it's time for the next frame */
        now = get_time_us();
        if (now < next_frame_time) {
            uint64_t wait_time = next_frame_time - now;
            if (wait_time > 1000) {
                usleep(wait_time - 500);
            } else if (wait_time > 100) {
                usleep(100);
            }
            /* Spin-wait for final precision */
            while (get_time_us() < next_frame_time) {
                /* busy wait */
            }
        }

        /* Schedule next frame - if we're late, schedule from now (no catch-up) */
        now = get_time_us();
        if (now > next_frame_time + target_frame_us) {
            /* We're more than one frame behind - reset timing, no catch-up */
            next_frame_time = now + target_frame_us;
        } else {
            next_frame_time += target_frame_us;
        }

        /* Non-blocking: if renderer hasn't consumed the previous frame,
         * drop it and keep going.  The TIA/Maria double-buffer ensures the
         * emulator can safely overwrite — memcpy in the renderer takes ~50us
         * while a frame takes ~16ms, so two full frames cannot complete
         * during a single memcpy.  This keeps audio fed even when GL stalls. */
        if (g_frame_ready) {
            g_frames_skipped++;
        }

        /* Run one frame of emulation */
        if (machine_is_loaded()) {
            machine_run_frame();

            /* Feed audio buffer */
            audio_update();

            /* ARM DMB: ensure buffer swap visible before flag */
            __sync_synchronize();
            g_frame_ready = 1;
            g_frames_produced++;
        }
    }

    return NULL;
}

static int init_sdl(void)
{
    char errmsg[256];

    /* Initialize SDL with video and audio */
    log_msg("SDL_Init VIDEO|AUDIO...");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        log_msg("SDL_Init FAILED");
        return -1;
    }
    log_msg("SDL_Init OK");

    /*
     * webOS PDK OpenGL ES setup:
     * - Set GL attributes BEFORE SDL_SetVideoMode
     * - Use SDL_OPENGL flag
     */
    log_msg("Setting SDL GL attributes for GLES 1.1...");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 5);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 6);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 5);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_SWAP_CONTROL, 1);  /* VSYNC enabled */

    /* Create OpenGL surface */
    log_msg("SDL_SetVideoMode 1024x768 OPENGL|FULLSCREEN...");
    g_screen = SDL_SetVideoMode(SCREEN_WIDTH, SCREEN_HEIGHT, 0,
                                SDL_OPENGL | SDL_FULLSCREEN);
    if (g_screen == NULL) {
        snprintf(errmsg, sizeof(errmsg), "SDL_SetVideoMode FAILED: %s", SDL_GetError());
        log_msg(errmsg);
        SDL_Quit();
        return -1;
    }
    log_msg("SDL_SetVideoMode OK");

    /* Set up basic OpenGL state */
    log_msg("Setting up OpenGL state...");
    glViewport(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(0, SCREEN_WIDTH, SCREEN_HEIGHT, 0, -1, 1);
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

    SDL_ShowCursor(SDL_DISABLE);

    /* Initialize video subsystem */
    log_msg("video_init...");
    if (video_init(g_screen) != 0) {
        log_msg("video_init FAILED");
        SDL_Quit();
        return -1;
    }
    log_msg("video_init OK");

    /* Initialize audio subsystem */
    log_msg("audio_init...");
    if (audio_init() != 0) {
        log_msg("audio_init FAILED");
        video_shutdown();
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

    /* Resume audio (paused when returning to filepicker) */
    audio_resume();

    /* Start emulator */
    g_emulator_running = 1;
    g_emulator_paused = 0;
    g_frame_ready = 0;

    if (!g_emu_thread_created) {
        if (pthread_create(&g_emu_thread, NULL, emulator_thread, NULL) == 0) {
            g_emu_thread_created = 1;
            log_msg("Emulator thread created");
        } else {
            log_msg("Failed to create emulator thread!");
            return;
        }
    }

    g_app_state = APP_STATE_EMULATOR;
}

/* Return to file picker from emulator */
static void return_to_filepicker(void)
{
    log_msg("Returning to file picker");

    /* Stop emulator thread */
    g_emulator_running = 0;
    if (g_emu_thread_created) {
        pthread_join(g_emu_thread, NULL);
        g_emu_thread_created = 0;
        {
            char msg[128];
            snprintf(msg, sizeof(msg),
                "FINAL FRAMESTATS: produced=%d rendered=%d skipped=%d",
                g_frames_produced, g_frames_rendered, g_frames_skipped);
            log_msg(msg);
        }
    }

    /* Pause audio */
    audio_pause();

    /* Clear frame state */
    g_frame_ready = 0;

    /* Re-scan the directory we launched from (preserves scroll position) */
    filepicker_rescan();

    g_app_state = APP_STATE_FILEPICKER;
}

/*
 * Main loop - handles SDL events and rendering.
 *
 * In FILEPICKER state: draws file picker, handles touch for selection.
 * In EMULATOR state: renders emulator frames, handles input.
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
                usleep(50000); /* Let emulator thread settle */
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
                    usleep(50000); /* Let emulator thread settle */
                    if (savestate_load(g_current_rom_path) == 0) {
                        g_frame_ready = 0; /* Force fresh frame */
                        input_show_notification("Loaded");
                    }
                    g_emulator_paused = 0;
                    audio_resume();
                }
                /* No save file: do nothing, no text */
            }

            /* Check for ZOOM button */
            if (input_zoom_pressed()) {
                video_cycle_zoom();
                input_show_notification(video_get_zoom_label());
            }

            {
                int popup_vis = input_options_popup_visible() || input_confirm_visible() || input_autosave_warn_visible();

                if (g_emulator_running && machine_is_loaded()) {
                    if (g_frame_ready || popup_vis) {
                        uint64_t upload_t0, upload_us, swap_t0, swap_us;

                        /* ARM DMB: ensure we see all prior stores */
                        __sync_synchronize();

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

                        /* Phase 2: swap buffers (may block on VSYNC) */
                        swap_t0 = get_time_us();
                        video_render_swap();
                        swap_us = get_time_us() - swap_t0;

                        g_swap_time_total += swap_us;
                        if (swap_us > g_swap_time_max)
                            g_swap_time_max = swap_us;

                        input_tick();

                        if (g_frame_ready) {
                            g_frame_ready = 0;
                            g_frames_rendered++;
                        }
                        g_perf_frame_count++;

                        /* Throttle when popup visible and paused */
                        if (popup_vis && g_emulator_paused)
                            usleep(33000);  /* ~30fps while popup shown */
                    } else {
                        usleep(500);
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
                    /* Mirror the CRT display window logic from video.c */
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

                    /* Content bounds: bounding box of non-zero pixels in display buffer.
                     * Rows/cols are absolute (buffer coordinates); subtract disp_off
                     * for display-relative position. */
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
 *
 * On webOS, each finger gets a unique ID in event.button.which /
 * event.motion.which. We pass this to the input layer for per-finger
 * tracking. The filepicker only needs single-touch, so it ignores IDs.
 *
 * Motion events are dispatched immediately per-finger (no coalescing)
 * so each finger's D-pad position is tracked independently.
 * For the filepicker, we still coalesce to the last motion position.
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
                if (g_app_state == APP_STATE_FILEPICKER) {
                    filepicker_touch_down(event.button.x, event.button.y);
                } else {
                    input_handle_touch_down(event.button.which,
                                            event.button.x, event.button.y);
                }
                events_processed++;
                break;

            case SDL_MOUSEBUTTONUP:
                if (g_touch_active > 0) g_touch_active--;
                if (g_app_state == APP_STATE_FILEPICKER) {
                    if (filepicker_touch_up(event.button.x, event.button.y)) {
                        launch_selected_rom();
                    }
                } else {
                    input_handle_touch_up(event.button.which,
                                          event.button.x, event.button.y);
                }
                events_processed++;
                break;

            case SDL_MOUSEMOTION:
                if (g_app_state == APP_STATE_FILEPICKER) {
                    picker_had_motion = 1;
                    picker_motion_x = event.motion.x;
                    picker_motion_y = event.motion.y;
                } else {
                    /* Touch dedup: skip identical position within time window */
                    int finger = event.motion.which & 7;  /* clamp to 0-7 */
                    uint64_t now = get_time_us();
                    if (event.motion.x == g_touch_dedup[finger].x &&
                        event.motion.y == g_touch_dedup[finger].y &&
                        now - g_touch_dedup[finger].time_us < TOUCH_DEDUP_WINDOW_US) {
                        g_touch_dedup_count++;
                    } else {
                        g_touch_dedup[finger].x = event.motion.x;
                        g_touch_dedup[finger].y = event.motion.y;
                        g_touch_dedup[finger].time_us = now;
                        input_handle_touch_move(event.motion.which,
                                                event.motion.x, event.motion.y);
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
                        glClear(GL_COLOR_BUFFER_BIT);
                        SDL_GL_SwapBuffers();
                        glClear(GL_COLOR_BUFFER_BIT);
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

/* Start emulator */
void main_start_emulator(void)
{
    g_emulator_running = 1;
    g_emulator_paused = 0;
    audio_resume();
}

/* Stop emulator */
void main_stop_emulator(void)
{
    g_emulator_running = 0;
    audio_pause();
}

/* Check if emulator is running */
int main_is_running(void)
{
    return g_emulator_running;
}
