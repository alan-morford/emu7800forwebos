/*
 * controller.h
 *
 * Wired USB game controller support (direct evdev reads).
 *
 * webOS's SDL has no joydev, so SDL_INIT_JOYSTICK never finds a pad --
 * controllers only ever show up as /dev/input/eventN. This module opens
 * that node directly and decodes button/axis events itself. It requires
 * the app's postinst to have bind-mounted /dev/input into the PDK jail
 * (see plugin/packaging/postinst); without that this module simply finds
 * nothing to open, which is a normal, silent, expected state.
 *
 * Copyright (c) 2026 EMU7800
 */

#ifndef CONTROLLER_H
#define CONTROLLER_H

/* Open/close the evdev subsystem. Safe to call if no pad is present. */
void controller_init(void);
void controller_shutdown(void);

/* Scan for / service the pad. Call once per main-loop iteration,
 * regardless of app state (cheap no-op with no pad attached). */
void controller_poll(void);

int controller_connected(void);

/* Level state -- safe to read every frame, reflects state as of the last
 * controller_poll() call. */
void controller_dpad(int *up, int *down, int *left, int *right);
int  controller_fire(void);            /* B / Y  -> Fire 1 */
int  controller_fire2(void);           /* A / X  -> Fire 2 */
int  controller_select_switch(void);   /* LT     -> console Select switch */
int  controller_reset_switch(void);    /* RT     -> console Reset switch */

/* Edge state -- true for the single frame the button/direction transitioned
 * from released to pressed (or, for nav up/down, also true on repeat while
 * held). Valid until the next controller_poll() call. */
int controller_start_edge(void);       /* Start  -> Pause */
int controller_back_edge(void);        /* Select -> Back */
int controller_nav_up_edge(void);
int controller_nav_down_edge(void);
int controller_a_edge(void);
int controller_b_edge(void);
int controller_save_edge(void);        /* LB     -> Save State */
int controller_load_edge(void);        /* RB     -> Load State */

/* Analog paddle (left stick). Only meaningful when controller_has_analog_paddle()
 * is true -- a d-pad-only pad has no analog axis to give a paddle value. */
int controller_has_analog_paddle(void);
int controller_paddle_value(void);     /* 0-255 */

#endif /* CONTROLLER_H */
