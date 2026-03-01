/*
 * filepicker.h
 *
 * File Picker UI Header
 * Lists ROM files and allows selection via touch
 *
 * Copyright (c) 2024 EMU7800
 */

#ifndef FILEPICKER_H
#define FILEPICKER_H

/* Initialize file picker */
void filepicker_init(void);

/* Shutdown file picker */
void filepicker_shutdown(void);

/* Scan directory for ROM files (.a26, .a78, .bin) */
void filepicker_scan(const char *directory);

/* Re-scan current directory preserving scroll position */
void filepicker_rescan(void);

/* Draw the file picker UI (clears, draws, swaps buffers) */
void filepicker_draw(void);

/* Touch event handlers - return 1 from touch_up if a file was selected */
void filepicker_touch_down(int x, int y);
void filepicker_touch_move(int x, int y);
int filepicker_touch_up(int x, int y);

/* Get selected file path (valid after touch_up returns 1) */
const char *filepicker_get_selected_path(void);

/* Get selected machine type (MACHINE_2600 or MACHINE_7800) */
int filepicker_get_selected_type(void);

/* Store last played ROM for resume feature */
void filepicker_set_last_rom(const char *path, int type);

/* Check if a last ROM is available for resume */
int filepicker_has_last_rom(void);

/* Check if save state should be loaded after ROM launch (consumes flag) */
int filepicker_should_load_save(void);

/* Get the current directory being browsed */
const char *filepicker_get_current_dir(void);

/* Get the saved default ROM directory (NULL if not set or dir doesn't exist) */
const char *filepicker_get_default_romdir(void);

/* Save current settings to persistence file (called from in-game options popup) */
void filepicker_save_settings(void);

/* Handle keyboard input in filepicker state.
 * Returns 1 if a ROM was selected (caller should launch). */
int filepicker_key_down(int sym);

/* Returns 1 if a keyboard was detected (persisted across sessions) */
int filepicker_keyboard_detected(void);

#endif /* FILEPICKER_H */
