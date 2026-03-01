/*
 * SDL.h
 *
 * SDL 1.2 Stub Header for cross-compilation
 * Provides declarations for SDL functions used by plugin
 *
 * On device, link against actual libSDL.so
 */

#ifndef SDL_H
#define SDL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Init flags */
#define SDL_INIT_VIDEO      0x00000020
#define SDL_INIT_AUDIO      0x00000010
#define SDL_INIT_TIMER      0x00000001

/* Surface flags */
#define SDL_SWSURFACE       0x00000000
#define SDL_HWSURFACE       0x00000001
#define SDL_DOUBLEBUF       0x40000000
#define SDL_FULLSCREEN      0x80000000
#define SDL_OPENGL          0x00000002

/* OpenGL attributes */
#define SDL_GL_RED_SIZE                 0
#define SDL_GL_GREEN_SIZE               1
#define SDL_GL_BLUE_SIZE                2
#define SDL_GL_ALPHA_SIZE               3
#define SDL_GL_BUFFER_SIZE              4
#define SDL_GL_DOUBLEBUFFER             5
#define SDL_GL_DEPTH_SIZE               6
#define SDL_GL_STENCIL_SIZE             7
#define SDL_GL_SWAP_CONTROL             16
#define SDL_GL_CONTEXT_MAJOR_VERSION    17
#define SDL_GL_CONTEXT_MINOR_VERSION    18

/* SDL_MUSTLOCK macro */
#define SDL_MUSTLOCK(s) (((s)->flags & SDL_HWSURFACE) != 0)

/* Cursor state */
#define SDL_DISABLE         0
#define SDL_ENABLE          1

/* Mouse button */
#define SDL_BUTTON(x)       (1 << ((x) - 1))

/* SDL types */
typedef uint8_t  Uint8;
typedef uint16_t Uint16;
typedef uint32_t Uint32;
typedef int8_t   Sint8;
typedef int16_t  Sint16;
typedef int32_t  Sint32;

/* Surface structure */
typedef struct SDL_Surface {
    Uint32 flags;
    void *format;
    int w, h;
    Uint16 pitch;
    void *pixels;
    void *userdata;
    int locked;
    void *map;
    unsigned int format_version;
    int refcount;
} SDL_Surface;

/* Rect structure */
typedef struct SDL_Rect {
    Sint16 x, y;
    Uint16 w, h;
} SDL_Rect;

/* Event types */
#define SDL_QUIT            12
#define SDL_ACTIVEEVENT     1
#define SDL_KEYDOWN         2
#define SDL_KEYUP           3
#define SDL_MOUSEMOTION     4
#define SDL_MOUSEBUTTONDOWN 5
#define SDL_MOUSEBUTTONUP   6

/* Active state flags */
#define SDL_APPACTIVE       0x04

/* Key symbols (SDL 1.2 keysyms - ASCII values) */
#define SDLK_1              49
#define SDLK_2              50
#define SDLK_3              51
#define SDLK_4              52
#define SDLK_5              53
#define SDLK_6              54
#define SDLK_7              55
#define SDLK_8              56
#define SDLK_9              57
#define SDLK_a              97
#define SDLK_d              100
#define SDLK_j              106
#define SDLK_k              107
#define SDLK_n              110
#define SDLK_s              115
#define SDLK_w              119
#define SDLK_y              121

/* Keyboard event */
typedef struct SDL_KeyboardEvent {
    Uint8 type;
    Uint8 which;
    Uint8 state;
    struct {
        Uint8 scancode;
        int sym;
        int mod;
        Uint16 unicode;
    } keysym;
} SDL_KeyboardEvent;

/* Mouse motion event */
typedef struct SDL_MouseMotionEvent {
    Uint8 type;
    Uint8 which;
    Uint8 state;
    Uint16 x, y;
    Sint16 xrel, yrel;
} SDL_MouseMotionEvent;

/* Mouse button event */
typedef struct SDL_MouseButtonEvent {
    Uint8 type;
    Uint8 which;
    Uint8 button;
    Uint8 state;
    Uint16 x, y;
} SDL_MouseButtonEvent;

/* Active event */
typedef struct SDL_ActiveEvent {
    Uint8 type;
    Uint8 gain;
    Uint8 state;
} SDL_ActiveEvent;

/* Quit event */
typedef struct SDL_QuitEvent {
    Uint8 type;
} SDL_QuitEvent;

/* Event union */
typedef union SDL_Event {
    Uint8 type;
    SDL_ActiveEvent active;
    SDL_KeyboardEvent key;
    SDL_MouseMotionEvent motion;
    SDL_MouseButtonEvent button;
    SDL_QuitEvent quit;
} SDL_Event;

/* Audio format */
#define AUDIO_S16SYS        0x8010

/* Audio callback */
typedef void (*SDL_AudioCallback)(void *userdata, Uint8 *stream, int len);

/* Audio spec */
typedef struct SDL_AudioSpec {
    int freq;
    Uint16 format;
    Uint8 channels;
    Uint8 silence;
    Uint16 samples;
    Uint16 padding;
    Uint32 size;
    SDL_AudioCallback callback;
    void *userdata;
} SDL_AudioSpec;

/* Init/Quit */
int SDL_Init(Uint32 flags);
void SDL_Quit(void);

/* Video */
SDL_Surface *SDL_SetVideoMode(int width, int height, int bpp, Uint32 flags);
int SDL_Flip(SDL_Surface *screen);
int SDL_LockSurface(SDL_Surface *surface);
void SDL_UnlockSurface(SDL_Surface *surface);
int SDL_ShowCursor(int toggle);

/* OpenGL */
int SDL_GL_SetAttribute(int attr, int value);
void SDL_GL_SwapBuffers(void);

/* Events */
int SDL_PollEvent(SDL_Event *event);

/* Timer */
Uint32 SDL_GetTicks(void);
void SDL_Delay(Uint32 ms);

/* Audio */
int SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained);
void SDL_CloseAudio(void);
void SDL_PauseAudio(int pause_on);
void SDL_LockAudio(void);
void SDL_UnlockAudio(void);

/* Error handling */
const char *SDL_GetError(void);
void SDL_Log(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* SDL_H */
