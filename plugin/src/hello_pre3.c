/*
 * hello_pre3.c
 *
 * Diagnostic "hello world" for HP Pre3 (webOS 2.x)
 * Tests PDL availability, SDL video modes, and touch input.
 * Logs everything to /media/internal/emu7800_hello.log
 *
 * Build:  cd plugin && make hello
 * Deploy: cp hello.arm ../emu7800.arm && cd .. && palm-package .
 *
 * NO PDL link dependency — all PDL calls via dlsym.
 * NO OpenGL dependency — pure SDL software rendering.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <SDL.h>

#define LOG_PATH "/media/internal/emu7800_hello.log"

static FILE *g_logf = NULL;

static void hlog(const char *msg)
{
    if (!g_logf) {
        g_logf = fopen(LOG_PATH, "w");
    }
    if (g_logf) {
        fprintf(g_logf, "%s\n", msg);
        fflush(g_logf);
    }
    fprintf(stderr, "%s\n", msg);
}

/*
 * Try SDL_SetVideoMode with given params.
 * Returns the surface on success, NULL on failure.
 * Logs the result.
 */
static SDL_Surface *try_video_mode(int w, int h, int bpp, Uint32 flags,
                                   const char *desc)
{
    SDL_Surface *s;
    char buf[256];

    snprintf(buf, sizeof(buf), "  Trying SDL_SetVideoMode(%d, %d, %d, 0x%X) [%s]...",
             w, h, bpp, (unsigned)flags, desc);
    hlog(buf);

    s = SDL_SetVideoMode(w, h, bpp, flags);
    if (s) {
        snprintf(buf, sizeof(buf), "    OK: %dx%d bpp=%d pitch=%d flags=0x%X",
                 s->w, s->h, s->format->BitsPerPixel, s->pitch,
                 (unsigned)s->flags);
        hlog(buf);
    } else {
        snprintf(buf, sizeof(buf), "    FAILED: %s", SDL_GetError());
        hlog(buf);
    }
    return s;
}

int main(int argc, char *argv[])
{
    SDL_Surface *screen = NULL;
    char buf[256];
    int running = 1;
    int frame_count = 0;

    (void)argc;
    (void)argv;

    hlog("========================================");
    hlog("EMU7800 Pre3 Diagnostic v1.0");
    hlog("========================================");

    /* ---- Test 1: PDL_Init via dlsym ---- */
    hlog("");
    hlog("[Test 1] PDL_Init via dlsym");
    {
        typedef int (*PDL_InitFunc)(unsigned int);
        PDL_InitFunc pdl_init = (PDL_InitFunc)dlsym(RTLD_DEFAULT, "PDL_Init");
        if (pdl_init) {
            int ret = pdl_init(0);
            snprintf(buf, sizeof(buf), "  PDL_Init(0) returned %d", ret);
            hlog(buf);
        } else {
            hlog("  PDL_Init NOT found via dlsym (webOS 2.x or no PDL)");
        }
    }

    /* ---- Test 2: PDL_GetHardwareID via dlsym ---- */
    hlog("");
    hlog("[Test 2] PDL_GetHardwareID via dlsym");
    {
        typedef int (*GetHWIDFunc)(void);
        GetHWIDFunc fn = (GetHWIDFunc)dlsym(RTLD_DEFAULT, "PDL_GetHardwareID");
        if (fn) {
            int hwid = fn();
            snprintf(buf, sizeof(buf), "  PDL_GetHardwareID() = %d", hwid);
            hlog(buf);
            if (hwid == 501)      hlog("  -> HP Pre3");
            else if (hwid == 601) hlog("  -> HP TouchPad");
            else                  hlog("  -> Unknown device");
        } else {
            hlog("  PDL_GetHardwareID NOT found via dlsym");
        }
    }

    /* ---- Test 3: PDL_GetPDKVersion via dlsym ---- */
    hlog("");
    hlog("[Test 3] PDL_GetPDKVersion via dlsym");
    {
        typedef int (*GetVersionFunc)(void);
        GetVersionFunc fn = (GetVersionFunc)dlsym(RTLD_DEFAULT, "PDL_GetPDKVersion");
        if (fn) {
            int ver = fn();
            snprintf(buf, sizeof(buf), "  PDL_GetPDKVersion() = %d", ver);
            hlog(buf);
        } else {
            hlog("  PDL_GetPDKVersion NOT found via dlsym");
        }
    }

    /* ---- Test 4: SDL_Init ---- */
    hlog("");
    hlog("[Test 4] SDL_Init");
    hlog("  Calling SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)...");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        snprintf(buf, sizeof(buf), "  SDL_Init FAILED: %s", SDL_GetError());
        hlog(buf);
        hlog("FATAL: Cannot continue without SDL");
        if (g_logf) fclose(g_logf);
        return 1;
    }
    hlog("  SDL_Init OK");

    /* ---- Test 5: SDL_SetVideoMode (multiple attempts) ---- */
    hlog("");
    hlog("[Test 5] SDL_SetVideoMode attempts");

    /* Try portrait SWSURFACE first (Pre3 native orientation) */
    if (!screen)
        screen = try_video_mode(480, 800, 16, SDL_SWSURFACE, "portrait SW");

    /* Try landscape SWSURFACE */
    if (!screen)
        screen = try_video_mode(800, 480, 16, SDL_SWSURFACE, "landscape SW");

    /* Try portrait FULLSCREEN */
    if (!screen)
        screen = try_video_mode(480, 800, 16, SDL_FULLSCREEN, "portrait FS");

    /* Try landscape FULLSCREEN */
    if (!screen)
        screen = try_video_mode(800, 480, 16, SDL_FULLSCREEN, "landscape FS");

    /* Try auto-detect (0x0) */
    if (!screen)
        screen = try_video_mode(0, 0, 0, SDL_SWSURFACE, "auto SW");

    /* Try auto-detect fullscreen */
    if (!screen)
        screen = try_video_mode(0, 0, 0, SDL_FULLSCREEN, "auto FS");

    if (!screen) {
        hlog("FATAL: All SDL_SetVideoMode attempts failed");
        SDL_Quit();
        if (g_logf) fclose(g_logf);
        return 1;
    }

    snprintf(buf, sizeof(buf), "  Using surface: %dx%d bpp=%d",
             screen->w, screen->h, screen->format->BitsPerPixel);
    hlog(buf);

    /* ---- Test 6: Draw test pattern ---- */
    hlog("");
    hlog("[Test 6] Drawing test pattern");
    {
        SDL_Rect r;
        Uint32 red, green, blue, white, yellow;

        red    = SDL_MapRGB(screen->format, 255, 0, 0);
        green  = SDL_MapRGB(screen->format, 0, 255, 0);
        blue   = SDL_MapRGB(screen->format, 0, 0, 255);
        white  = SDL_MapRGB(screen->format, 255, 255, 255);
        yellow = SDL_MapRGB(screen->format, 255, 255, 0);

        /* Black background */
        SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0, 0, 0));

        /* Color bars */
        r.x = 10; r.y = 10; r.w = 100; r.h = 100;
        SDL_FillRect(screen, &r, red);

        r.x = 120; r.y = 10; r.w = 100; r.h = 100;
        SDL_FillRect(screen, &r, green);

        r.x = 230; r.y = 10; r.w = 100; r.h = 100;
        SDL_FillRect(screen, &r, blue);

        r.x = 340; r.y = 10; r.w = 100; r.h = 100;
        SDL_FillRect(screen, &r, white);

        /* Large yellow rectangle in center */
        r.x = screen->w / 4;
        r.y = screen->h / 4;
        r.w = screen->w / 2;
        r.h = screen->h / 2;
        SDL_FillRect(screen, &r, yellow);

        /* Border indicator: thin white lines on edges to verify fullscreen coverage */
        r.x = 0; r.y = 0; r.w = screen->w; r.h = 2;
        SDL_FillRect(screen, &r, white);  /* top */
        r.x = 0; r.y = screen->h - 2; r.w = screen->w; r.h = 2;
        SDL_FillRect(screen, &r, white);  /* bottom */
        r.x = 0; r.y = 0; r.w = 2; r.h = screen->h;
        SDL_FillRect(screen, &r, white);  /* left */
        r.x = screen->w - 2; r.y = 0; r.w = 2; r.h = screen->h;
        SDL_FillRect(screen, &r, white);  /* right */

        SDL_Flip(screen);
        hlog("  Test pattern drawn and flipped");
    }

    /* ---- Test 7: Event loop ---- */
    hlog("");
    hlog("[Test 7] Entering event loop (touch to test, card/back to exit)");
    SDL_ShowCursor(SDL_DISABLE);

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_QUIT:
                    hlog("  SDL_QUIT received");
                    running = 0;
                    break;

                case SDL_MOUSEBUTTONDOWN:
                    snprintf(buf, sizeof(buf),
                             "  TOUCH DOWN: x=%d y=%d which=%d button=%d",
                             event.button.x, event.button.y,
                             event.button.which, event.button.button);
                    hlog(buf);
                    break;

                case SDL_MOUSEBUTTONUP:
                    snprintf(buf, sizeof(buf),
                             "  TOUCH UP:   x=%d y=%d which=%d",
                             event.button.x, event.button.y,
                             event.button.which);
                    hlog(buf);
                    break;

                case SDL_MOUSEMOTION:
                    /* Only log every 10th motion to avoid log spam */
                    frame_count++;
                    if ((frame_count % 10) == 0) {
                        snprintf(buf, sizeof(buf),
                                 "  MOTION:     x=%d y=%d which=%d",
                                 event.motion.x, event.motion.y,
                                 event.motion.which);
                        hlog(buf);
                    }
                    break;

                case SDL_KEYDOWN:
                    snprintf(buf, sizeof(buf), "  KEY DOWN: sym=%d",
                             event.key.keysym.sym);
                    hlog(buf);
                    /* ESC or Back gesture exits */
                    if (event.key.keysym.sym == 27) { /* ESC */
                        running = 0;
                    }
                    break;

                case SDL_ACTIVEEVENT:
                    snprintf(buf, sizeof(buf),
                             "  ACTIVE: gain=%d state=%d",
                             event.active.gain, event.active.state);
                    hlog(buf);
                    if ((event.active.state & SDL_APPACTIVE) && !event.active.gain) {
                        hlog("  App minimized/carded");
                        running = 0;
                    }
                    break;

                default:
                    snprintf(buf, sizeof(buf), "  EVENT: type=%d", event.type);
                    hlog(buf);
                    break;
            }
        }
        SDL_Delay(50);
    }

    /* ---- Cleanup ---- */
    hlog("");
    hlog("Exiting cleanly");

    /* PDL_Quit via dlsym */
    {
        typedef void (*PDL_QuitFunc)(void);
        PDL_QuitFunc pdl_quit = (PDL_QuitFunc)dlsym(RTLD_DEFAULT, "PDL_Quit");
        if (pdl_quit) {
            pdl_quit();
            hlog("PDL_Quit called");
        }
    }

    SDL_Quit();
    hlog("SDL_Quit done");
    hlog("========================================");
    hlog("Diagnostic complete. Check this log on device.");
    hlog("========================================");

    if (g_logf) fclose(g_logf);
    return 0;
}
