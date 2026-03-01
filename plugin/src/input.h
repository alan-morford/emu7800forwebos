/*
 * input.h
 *
 * Multitouch Input Handler Header
 * Tracks up to 5 simultaneous finger touches mapped to virtual buttons.
 *
 * Copyright (c) 2024 EMU7800
 */

#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>

/* Initialize input subsystem */
void input_init(void);

/* Handle touch events (finger_id from SDL event.button.which / event.motion.which) */
void input_handle_touch_down(int finger_id, int x, int y);
void input_handle_touch_up(int finger_id, int x, int y);
void input_handle_touch_move(int finger_id, int x, int y);

/* Draw touch control overlays (software rendering - deprecated) */
void input_draw_controls(uint32_t *pixels, int pitch);

/* Draw touch control overlays using OpenGL */
void input_draw_controls_gl(void);

/* Check if reset button is pressed */
int input_reset_pressed(void);

/* Check if select button is pressed */
int input_select_pressed(void);

/* Check if back button was pressed (returns 1 once, then clears) */
int input_back_pressed(void);

/* Check if pause button was pressed (returns 1 once, then clears) */
int input_pause_pressed(void);

/* Check if save button was pressed (returns 1 once, then clears) */
int input_save_pressed(void);

/* Check if load button was pressed (returns 1 once, then clears) */
int input_load_pressed(void);

/* Check if zoom button was pressed (returns 1 once, then clears) */
int input_zoom_pressed(void);

/* Handle keyboard events */
void input_handle_key_down(int sdl_key);
void input_handle_key_up(int sdl_key);

/* Returns 1 if keyboard was detected (labels should be shown) */
int input_keyboard_active(void);

/* Set keyboard active state (e.g. transferred from filepicker detection) */
void input_set_keyboard_active(int active);

/* Set whether a save file exists for the current ROM (affects LOAD button appearance) */
void input_set_save_exists(int exists);

/* Show a notification message at the top of the screen (auto-fades after ~2 seconds) */
void input_show_notification(const char *text);

/* Tick notification timer (call once per frame) */
void input_tick(void);

/* OPTIONS popup */
int input_options_pressed(void);
int input_options_popup_visible(void);
void input_close_options_popup(void);
void input_draw_popup_gl(void);

/* Auto-save confirmation dialog */
int input_confirm_visible(void);
int input_confirm_result(void);     /* -1=pending, 0=No, 1=Yes */
void input_show_confirm(void);

/* Auto-save warning dialog */
int input_autosave_warn_visible(void);
int input_autosave_warn_result(void);  /* -1=pending, 0=No, 1=Yes */

/* Settings accessors (persisted via filepicker) */
int input_get_autosave(void);
void input_set_autosave(int val);
int input_get_autosave_ask(void);
void input_set_autosave_ask(int val);
int input_get_control_dim(void);
void input_set_control_dim(int val);

#endif /* INPUT_H */
