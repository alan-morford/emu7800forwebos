/*
 * device.h
 *
 * Runtime device detection for HP TouchPad and HP Pre3.
 * Handles all PDL interaction via dlsym (safe on webOS 2.x and 3.x).
 *
 * Call device_init() FIRST, before SDL_Init or any other init.
 * It will call PDL_Init internally if available.
 */

#ifndef DEVICE_H
#define DEVICE_H

#define DEVICE_TOUCHPAD  0
#define DEVICE_PRE3      1

void device_init(void);                  /* Call before SDL_Init */
int  device_screen_width(void);          /* 1024 or 800 (logical) */
int  device_screen_height(void);         /* 768 or 480 (logical) */
int  device_type(void);                  /* DEVICE_TOUCHPAD or DEVICE_PRE3 */
int  device_is_small(void);              /* 1 if Pre3 */
int  device_has_gl(void);                /* 1 if OpenGL ES via SDL is supported */

/* PDL wrappers — safe no-ops when PDL is not available */
void device_pdl_quit(void);
void device_pdl_screen_timeout(int enable);

#endif /* DEVICE_H */
