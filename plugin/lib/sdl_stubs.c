/*
 * SDL stub library for cross-compilation
 * These empty functions satisfy the linker
 * Real implementations come from device libraries at runtime
 */

#include <stdint.h>
#include <stddef.h>

typedef uint32_t Uint32;
typedef uint16_t Uint16;
typedef uint8_t Uint8;
typedef int32_t Sint32;
typedef int16_t Sint16;

/* SDL stubs */
int SDL_Init(Uint32 flags) { return 0; }
void SDL_Quit(void) {}
void *SDL_SetVideoMode(int w, int h, int bpp, Uint32 flags) { return NULL; }
int SDL_Flip(void *screen) { return 0; }
int SDL_LockSurface(void *surface) { return 0; }
void SDL_UnlockSurface(void *surface) {}
int SDL_ShowCursor(int toggle) { return 0; }
int SDL_PollEvent(void *event) { return 0; }
Uint32 SDL_GetTicks(void) { return 0; }
void SDL_Delay(Uint32 ms) {}
int SDL_OpenAudio(void *desired, void *obtained) { return 0; }
void SDL_CloseAudio(void) {}
void SDL_PauseAudio(int pause_on) {}
void SDL_LockAudio(void) {}
void SDL_UnlockAudio(void) {}
int SDL_FillRect(void *dst, void *dstrect, Uint32 color) { (void)dst; (void)dstrect; (void)color; return 0; }
Uint32 SDL_MapRGB(void *fmt, Uint8 r, Uint8 g, Uint8 b) { (void)fmt; (void)r; (void)g; (void)b; return 0; }
const char *SDL_GetError(void) { return "stub"; }
void SDL_Log(const char *fmt, ...) {}
int SDL_GL_SetAttribute(int attr, int value) { return 0; }
void SDL_GL_SwapBuffers(void) {}
