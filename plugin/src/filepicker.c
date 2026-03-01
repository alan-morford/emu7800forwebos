/*
 * filepicker.c
 *
 * File Picker UI
 * Scans directory for files and subdirectories, displays scrollable list,
 * handles touch for navigation and ROM selection.
 *
 * Copyright (c) 2024 EMU7800
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dlfcn.h>
#include <GLES/gl.h>
#include <SDL.h>
#include "filepicker.h"
#include "font.h"
#include "machine.h"
#include "video.h"
#include "maria.h"
#include "input.h"
#include "updater.h"
#include "asteroids_img.h"
#include "logo_img.h"
#include "gear_img.h"
/* External logging */
extern void log_msg(const char *msg);

/* GL constant for RGB565 - may not be in webOS headers */
#ifndef GL_UNSIGNED_SHORT_5_6_5
#define GL_UNSIGNED_SHORT_5_6_5 0x8363
#endif


/* Screen dimensions */
#define SCREEN_WIDTH  1024
#define SCREEN_HEIGHT 768

/* UI layout */
#define MARGIN_X      16
#define TITLE_Y       16
#define LIST_TOP      120
#define LIST_BOTTOM   (SCREEN_HEIGHT - 8)
#define ITEM_HEIGHT   48
#define SCROLLBAR_W   8
#define SCROLLBAR_X   (SCREEN_WIDTH - MARGIN_X)

/* Resume button (top-left) */
#define RESUME_X      MARGIN_X
#define RESUME_Y      16
#define RESUME_H      40

/* Asteroids launch image (right-aligned, bottom-aligned with list) */
#define AST_BTN_W     180
#define AST_BTN_H     180
#define AST_BTN_X     (SCREEN_WIDTH - MARGIN_X - AST_BTN_W)
#define AST_BTN_Y     (LIST_BOTTOM - AST_BTN_H)

/* Bundled ROM path (installed alongside the binary) */
#define BUNDLED_ASTEROIDS_ROM "/media/cryptofs/apps/usr/palm/applications/com.emu7800.touchpad/Asteroids.a78"

/* Touch thresholds */
#define TAP_THRESHOLD 10

/* Max files */
#define MAX_FILES 256
#define MAX_PATH_LEN 512
#define MAX_NAME_LEN 128

/* Persistence file for last played ROM */
#define LASTROM_FILE "/media/internal/.emu7800_lastrom"

/* Recently played list */
#define RECENT_MAX  9
#define RECENT_FILE "/media/internal/.emu7800_recent"

/* Popup layout */
#define POPUP_W       600
#define POPUP_PAD     16
#define POPUP_TITLE_H 16
#define POPUP_GAP     12
#define POPUP_ITEM_H  40
#define POPUP_BTN_H   36

/* Entry type constants */
#define ENTRY_DIR     -2
#define ENTRY_FILE    -1  /* non-ROM file (hidden) */
#define ENTRY_SAV     -3  /* .sav file (visible, not launchable) */

/* File entry */
typedef struct {
    char path[MAX_PATH_LEN];
    char name[MAX_NAME_LEN];
    int type; /* ENTRY_DIR, MACHINE_2600, MACHINE_7800, or ENTRY_FILE */
    int is_dir;
} FileEntry;

/* State */
static FileEntry g_files[MAX_FILES];
static int g_file_count = 0;
static float g_scroll_offset = 0.0f;
static int g_selected_index = -1;
static char g_current_dir[MAX_PATH_LEN];

/* Last played ROM (for resume) */
static char g_last_rom_path[MAX_PATH_LEN];
static int g_last_rom_type = 0;
static int g_has_last_rom = 0;
static int g_resume_selected = 0;

/* Asteroids button selection flag */
static int g_art_selected = 0;

/* Asteroids image GL texture */
static GLuint g_ast_texture = 0;

/* Gear icon GL texture */
static GLuint g_gear_texture = 0;

/* Animated logo background */
static GLuint g_logo_texture = 0;
static int    g_logo_frame = 0;
static Uint32 g_logo_last_tick = 0;

/* Recently played list */
static char g_recent_paths[RECENT_MAX][MAX_PATH_LEN];
static int  g_recent_types[RECENT_MAX];
static int  g_recent_count = 0;
static int  g_recent_popup_visible = 0;

/* About popup */
static int  g_about_popup_visible = 0;

/* Save popup (shown when loading a ROM that has a .sav file) */
static int  g_save_popup_visible = 0;
static char g_save_popup_path[MAX_PATH_LEN];
static int  g_save_popup_type = 0;
static int  g_save_popup_load_save = 0;

/* Delete confirmation popup */
static int  g_delete_confirm_visible = 0;

/* Settings file (lives in app install dir — removed on uninstall) */
#define SETTINGS_FILE "/media/cryptofs/apps/usr/palm/applications/com.emu7800.touchpad/.emu7800_settings"

/* Default ROM directory popup state */
static char g_default_romdir[MAX_PATH_LEN];
static int  g_romdir_prompt_never = 0;
static int  g_dirask_popup_visible = 0;
static int  g_dirpicker_popup_visible = 0;

/* Directory picker listing (directories only) */
#define DIRPICKER_MAX 128
typedef struct {
    char path[MAX_PATH_LEN];
    char name[MAX_NAME_LEN];
} DirEntry;
static DirEntry g_dirpicker_dirs[DIRPICKER_MAX];
static int  g_dirpicker_count = 0;
static char g_dirpicker_current[MAX_PATH_LEN];
static float g_dirpicker_scroll = 0.0f;

/* Settings popup state */
static int  g_settings_popup_visible = 0;

/* Update popup state */
static int  g_update_popup_visible = 0;
static int  g_update_auto_shown = 0;   /* 1 after auto-popup has fired */
static int  g_update_later = 0;        /* 1 after user tapped LATER */

/* Auto-save warning dialog state (filepicker) */
static int  g_fp_aswarn_visible = 0;
static int  g_fp_aswarn_action = 0;  /* 0=enabling autosave, 1=disabling ask */

/* Directory picker touch tracking */
static int  g_dirpicker_touch_active = 0;
static int  g_dirpicker_touch_start_y = 0;
static float g_dirpicker_scroll_at_start = 0.0f;
static int  g_dirpicker_touch_moved = 0;

/* Keyboard detected (set on first keypress, cleared on touch) */
static int g_keyboard_detected = 0;

/* Touch tracking */
static int g_touch_active = 0;
static int g_touch_start_x = 0;
static int g_touch_start_y = 0;
static int g_touch_last_y = 0;
static int g_touch_moved = 0;
static float g_scroll_at_touch_start = 0.0f;

/* Compare: directories first, then alphabetical */
static int compare_files(const void *a, const void *b)
{
    const FileEntry *fa = (const FileEntry *)a;
    const FileEntry *fb = (const FileEntry *)b;
    /* Directories sort before files */
    if (fa->is_dir != fb->is_dir)
        return fb->is_dir - fa->is_dir;
    return strcasecmp(fa->name, fb->name);
}

/* Check if filename has a recognized extension */
static int get_rom_type(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return ENTRY_FILE;
    if (strcasecmp(dot, ".a26") == 0) return MACHINE_2600;
    if (strcasecmp(dot, ".a78") == 0) return MACHINE_7800;
    if (strcasecmp(dot, ".bin") == 0) return MACHINE_2600; /* default bin to 2600 */
    if (strcasecmp(dot, ".sav") == 0) return ENTRY_SAV;
    return ENTRY_FILE;
}

/* Load last ROM from persistence file */
static void load_last_rom(void)
{
    FILE *f = fopen(LASTROM_FILE, "r");
    char line[MAX_PATH_LEN];
    if (!f) return;

    /* Line 1: path */
    if (!fgets(line, sizeof(line), f)) { fclose(f); return; }
    /* Strip newline */
    {
        int len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
    }
    strncpy(g_last_rom_path, line, MAX_PATH_LEN - 1);
    g_last_rom_path[MAX_PATH_LEN - 1] = '\0';

    /* Line 2: type */
    if (!fgets(line, sizeof(line), f)) { fclose(f); return; }
    g_last_rom_type = atoi(line);

    g_has_last_rom = 1;
    fclose(f);
}

/* Save last ROM to persistence file */
static void save_last_rom(void)
{
    FILE *f = fopen(LASTROM_FILE, "w");
    if (!f) return;
    fprintf(f, "%s\n%d\n", g_last_rom_path, g_last_rom_type);
    fclose(f);
}

/* Save recent list to file */
static void save_recent_list(void)
{
    FILE *f = fopen(RECENT_FILE, "w");
    int i;
    if (!f) return;
    for (i = 0; i < g_recent_count; i++) {
        fprintf(f, "%s\n%d\n", g_recent_paths[i], g_recent_types[i]);
    }
    fclose(f);
}

/* Load recent list from file */
static void load_recent_list(void)
{
    FILE *f = fopen(RECENT_FILE, "r");
    char line[MAX_PATH_LEN];
    int len;
    if (!f) return;
    g_recent_count = 0;
    while (g_recent_count < RECENT_MAX && fgets(line, sizeof(line), f)) {
        len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (line[0] == '\0') continue;
        strncpy(g_recent_paths[g_recent_count], line, MAX_PATH_LEN - 1);
        g_recent_paths[g_recent_count][MAX_PATH_LEN - 1] = '\0';
        if (!fgets(line, sizeof(line), f)) break;
        g_recent_types[g_recent_count] = atoi(line);
        g_recent_count++;
    }
    fclose(f);
}

/* Load settings from persistence file */
static void load_settings(void)
{
    FILE *f = fopen(SETTINGS_FILE, "r");
    char line[MAX_PATH_LEN];
    int len;
    if (!f) return;

    while (fgets(line, sizeof(line), f)) {
        len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';

        if (strncmp(line, "romdir_prompt=never", 19) == 0) {
            g_romdir_prompt_never = 1;
        } else if (strncmp(line, "romdir=", 7) == 0) {
            strncpy(g_default_romdir, line + 7, MAX_PATH_LEN - 1);
            g_default_romdir[MAX_PATH_LEN - 1] = '\0';
        } else if (strncmp(line, "scanlines=", 10) == 0) {
            video_set_scanlines(atoi(line + 10));
        } else if (strncmp(line, "palette=", 8) == 0) {
            video_set_maria_palette(atoi(line + 8));
        } else if (strncmp(line, "brightness=", 11) == 0) {
            video_set_scanline_brightness(atoi(line + 11));
        } else if (strncmp(line, "autosave=", 9) == 0) {
            input_set_autosave(atoi(line + 9));
        } else if (strncmp(line, "autosave_ask=", 13) == 0) {
            input_set_autosave_ask(atoi(line + 13));
        } else if (strncmp(line, "control_dim=", 12) == 0) {
            input_set_control_dim(atoi(line + 12));
        } else if (strncmp(line, "keyboard=", 9) == 0) {
            g_keyboard_detected = atoi(line + 9);
        }
    }
    fclose(f);
}

/* Save settings to persistence file */
static void save_settings(void)
{
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f) return;
    if (g_romdir_prompt_never) {
        fprintf(f, "romdir_prompt=never\n");
    }
    if (g_default_romdir[0] != '\0') {
        fprintf(f, "romdir=%s\n", g_default_romdir);
    }
    fprintf(f, "scanlines=%d\n", video_get_scanlines());
    fprintf(f, "brightness=%d\n", video_get_scanline_brightness());
    fprintf(f, "palette=%d\n", video_get_maria_palette());
    fprintf(f, "autosave=%d\n", input_get_autosave());
    fprintf(f, "autosave_ask=%d\n", input_get_autosave_ask());
    fprintf(f, "control_dim=%d\n", input_get_control_dim());
    if (g_keyboard_detected) {
        fprintf(f, "keyboard=1\n");
    }
    fclose(f);
}

/* Public wrapper for in-game options popup */
void filepicker_save_settings(void)
{
    save_settings();
}

/* Compare directories for qsort (case-insensitive, ".." pinned first) */
static int compare_dirs(const void *a, const void *b)
{
    const DirEntry *da = (const DirEntry *)a;
    const DirEntry *db = (const DirEntry *)b;
    if (strcmp(da->name, "..") == 0) return -1;
    if (strcmp(db->name, "..") == 0) return 1;
    return strcasecmp(da->name, db->name);
}

/* Scan directory for subdirectories only (for directory picker) */
static void dirpicker_scan(const char *directory)
{
    DIR *dir;
    struct dirent *ent;
    struct stat st;
    char fullpath[MAX_PATH_LEN];
    int dirlen;

    g_dirpicker_count = 0;
    g_dirpicker_scroll = 0.0f;

    /* Store current directory */
    strncpy(g_dirpicker_current, directory, MAX_PATH_LEN - 1);
    g_dirpicker_current[MAX_PATH_LEN - 1] = '\0';

    /* Ensure trailing slash */
    dirlen = strlen(g_dirpicker_current);
    if (dirlen > 0 && g_dirpicker_current[dirlen - 1] != '/') {
        if (dirlen < MAX_PATH_LEN - 1) {
            g_dirpicker_current[dirlen] = '/';
            g_dirpicker_current[dirlen + 1] = '\0';
        }
    }

    dir = opendir(g_dirpicker_current);
    if (!dir) return;

    /* Add ".." entry unless at /media/internal/ */
    if (strcmp(g_dirpicker_current, "/media/internal/") != 0) {
        DirEntry *d = &g_dirpicker_dirs[g_dirpicker_count];
        strncpy(d->name, "..", MAX_NAME_LEN - 1);
        d->name[MAX_NAME_LEN - 1] = '\0';
        /* Build parent path */
        strncpy(d->path, g_dirpicker_current, MAX_PATH_LEN - 1);
        d->path[MAX_PATH_LEN - 1] = '\0';
        {
            int len = strlen(d->path);
            char *slash;
            if (len > 1 && d->path[len - 1] == '/')
                d->path[len - 1] = '\0';
            slash = strrchr(d->path, '/');
            if (slash && slash != d->path)
                *(slash + 1) = '\0';
            else if (slash)
                *(slash + 1) = '\0';
        }
        g_dirpicker_count++;
    }

    while ((ent = readdir(dir)) != NULL && g_dirpicker_count < DIRPICKER_MAX) {
        /* Skip . and .. */
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        /* Skip hidden entries */
        if (ent->d_name[0] == '.')
            continue;

        snprintf(fullpath, MAX_PATH_LEN, "%s%s", g_dirpicker_current, ent->d_name);

        /* Only include directories */
        if (stat(fullpath, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;

        {
            DirEntry *d = &g_dirpicker_dirs[g_dirpicker_count];
            strncpy(d->path, fullpath, MAX_PATH_LEN - 1);
            d->path[MAX_PATH_LEN - 1] = '\0';
            strncpy(d->name, ent->d_name, MAX_NAME_LEN - 1);
            d->name[MAX_NAME_LEN - 1] = '\0';
            g_dirpicker_count++;
        }
    }

    closedir(dir);

    /* Sort: ".." first, then alphabetical */
    if (g_dirpicker_count > 1) {
        qsort(g_dirpicker_dirs, g_dirpicker_count, sizeof(DirEntry), compare_dirs);
    }
}

/* Add a ROM to the recent list (front, dedup, cap at RECENT_MAX) */
static void add_to_recent(const char *path, int type)
{
    int i, found = -1;
    /* Check if already in list */
    for (i = 0; i < g_recent_count; i++) {
        if (strcmp(g_recent_paths[i], path) == 0) {
            found = i;
            break;
        }
    }
    if (found > 0) {
        /* Move existing entry to front */
        char tmp_path[MAX_PATH_LEN];
        int tmp_type = g_recent_types[found];
        strncpy(tmp_path, g_recent_paths[found], MAX_PATH_LEN);
        for (i = found; i > 0; i--) {
            strncpy(g_recent_paths[i], g_recent_paths[i - 1], MAX_PATH_LEN);
            g_recent_types[i] = g_recent_types[i - 1];
        }
        strncpy(g_recent_paths[0], tmp_path, MAX_PATH_LEN);
        g_recent_types[0] = tmp_type;
    } else if (found != 0) {
        /* Not in list - insert at front, shift others down */
        int count = g_recent_count < RECENT_MAX ? g_recent_count : RECENT_MAX - 1;
        for (i = count; i > 0; i--) {
            strncpy(g_recent_paths[i], g_recent_paths[i - 1], MAX_PATH_LEN);
            g_recent_types[i] = g_recent_types[i - 1];
        }
        strncpy(g_recent_paths[0], path, MAX_PATH_LEN - 1);
        g_recent_paths[0][MAX_PATH_LEN - 1] = '\0';
        g_recent_types[0] = type;
        if (g_recent_count < RECENT_MAX) g_recent_count++;
    }
    /* Update type (in case it changed) */
    g_recent_types[0] = type;
    save_recent_list();
}

/* Initialize */
void filepicker_init(void)
{
    g_file_count = 0;
    g_scroll_offset = 0.0f;
    g_selected_index = -1;
    g_touch_active = 0;
    g_current_dir[0] = '\0';

    /* Load persisted last ROM if not already set */
    if (!g_has_last_rom) {
        load_last_rom();
    }

    /* Load recently played list */
    load_recent_list();

    /* Load settings (default ROM directory, prompt preference) */
    load_settings();
    if (!g_romdir_prompt_never && g_default_romdir[0] == '\0') {
        g_dirask_popup_visible = 1;
    }

    /* Seed recent list from last ROM if upgrading from older version */
    if (g_has_last_rom && g_recent_count == 0) {
        add_to_recent(g_last_rom_path, g_last_rom_type);
    }

    /* Start background update check */
    updater_check_start();

    /* Create Asteroids image texture */
    if (g_ast_texture == 0) {
        glGenTextures(1, &g_ast_texture);
        glBindTexture(GL_TEXTURE_2D, g_ast_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                     ASTEROIDS_IMG_W, ASTEROIDS_IMG_H, 0,
                     GL_RGB, GL_UNSIGNED_SHORT_5_6_5,
                     asteroids_img_data);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    /* Create gear icon texture (RGBA for transparency) */
    if (g_gear_texture == 0) {
        glGenTextures(1, &g_gear_texture);
        glBindTexture(GL_TEXTURE_2D, g_gear_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                     GEAR_TEX_SIZE, GEAR_TEX_SIZE, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE,
                     gear_img_data);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    /* Create animated logo texture */
    if (g_logo_texture == 0) {
        glGenTextures(1, &g_logo_texture);
        glBindTexture(GL_TEXTURE_2D, g_logo_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                     LOGO_TEX_SIZE, LOGO_TEX_SIZE, 0,
                     GL_RGB, GL_UNSIGNED_SHORT_5_6_5,
                     logo_frame_data[0]);
        glBindTexture(GL_TEXTURE_2D, 0);
        g_logo_frame = 0;
        g_logo_last_tick = 0;
    }
}

/* Shutdown */
void filepicker_shutdown(void)
{
    updater_shutdown();
    if (g_ast_texture) {
        glDeleteTextures(1, &g_ast_texture);
        g_ast_texture = 0;
    }
    if (g_logo_texture) {
        glDeleteTextures(1, &g_logo_texture);
        g_logo_texture = 0;
    }
    if (g_gear_texture) {
        glDeleteTextures(1, &g_gear_texture);
        g_gear_texture = 0;
    }
    g_file_count = 0;
}

/* Scan directory for all files and subdirectories */
void filepicker_scan(const char *directory)
{
    DIR *dir;
    struct dirent *ent;
    struct stat st;
    int dirlen;

    g_file_count = 0;
    g_scroll_offset = 0.0f;
    g_selected_index = -1;

    /* Store current directory */
    strncpy(g_current_dir, directory, MAX_PATH_LEN - 1);
    g_current_dir[MAX_PATH_LEN - 1] = '\0';

    /* Ensure trailing slash */
    dirlen = strlen(g_current_dir);
    if (dirlen > 0 && g_current_dir[dirlen - 1] != '/') {
        if (dirlen < MAX_PATH_LEN - 1) {
            g_current_dir[dirlen] = '/';
            g_current_dir[dirlen + 1] = '\0';
        }
    }

    dir = opendir(g_current_dir);
    if (!dir) return;

    /* Add ".." entry if not at root and not at /media/internal/ */
    if (strcmp(g_current_dir, "/") != 0 &&
        strcmp(g_current_dir, "/media/internal/") != 0) {
        FileEntry *f = &g_files[g_file_count];
        snprintf(f->name, MAX_NAME_LEN, "..");
        /* Build parent path */
        strncpy(f->path, g_current_dir, MAX_PATH_LEN - 1);
        f->path[MAX_PATH_LEN - 1] = '\0';
        /* Strip trailing slash, then strip last component */
        {
            int len = strlen(f->path);
            char *slash;
            if (len > 1 && f->path[len - 1] == '/')
                f->path[len - 1] = '\0';
            slash = strrchr(f->path, '/');
            if (slash && slash != f->path)
                *(slash + 1) = '\0';
            else if (slash)
                *(slash + 1) = '\0';
        }
        f->type = ENTRY_DIR;
        f->is_dir = 1;
        g_file_count++;
    }

    while ((ent = readdir(dir)) != NULL && g_file_count < MAX_FILES) {
        FileEntry *f;
        char fullpath[MAX_PATH_LEN];

        /* Skip . and .. */
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        /* Skip hidden entries (starting with .) */
        if (ent->d_name[0] == '.')
            continue;

        /* Skip .ini files */
        {
            const char *dot = strrchr(ent->d_name, '.');
            if (dot && strcasecmp(dot, ".ini") == 0)
                continue;
        }

        snprintf(fullpath, MAX_PATH_LEN, "%s%s", g_current_dir, ent->d_name);

        f = &g_files[g_file_count];
        strncpy(f->path, fullpath, MAX_PATH_LEN - 1);
        f->path[MAX_PATH_LEN - 1] = '\0';
        strncpy(f->name, ent->d_name, MAX_NAME_LEN - 1);
        f->name[MAX_NAME_LEN - 1] = '\0';

        /* Check if it's a directory */
        if (stat(fullpath, &st) == 0 && S_ISDIR(st.st_mode)) {
            f->type = ENTRY_DIR;
            f->is_dir = 1;
        } else {
            f->type = get_rom_type(ent->d_name);
            f->is_dir = 0;
            if (f->type == ENTRY_FILE) continue;  /* skip unrecognized files */
        }

        g_file_count++;
    }

    closedir(dir);

    /* Sort: ".." stays at index 0, sort the rest */
    {
        int start = 0;
        /* Skip ".." entry at front */
        if (g_file_count > 0 && strcmp(g_files[0].name, "..") == 0)
            start = 1;
        if (g_file_count - start > 1)
            qsort(&g_files[start], g_file_count - start, sizeof(FileEntry), compare_files);
    }
}

/* Re-scan current directory preserving scroll position (for returning from game) */
void filepicker_rescan(void)
{
    float saved_scroll = g_scroll_offset;
    if (g_current_dir[0] != '\0') {
        filepicker_scan(g_current_dir);
        g_scroll_offset = saved_scroll;
    }
}

/* Draw a filled rectangle (no texture) */
static void draw_rect(float x, float y, float w, float h,
                      float r, float g, float b, float a)
{
    GLfloat verts[] = {
        x,     y,
        x + w, y,
        x,     y + h,
        x + w, y + h
    };

    glColor4f(r, g, b, a);
    glVertexPointer(2, GL_FLOAT, 0, verts);
    glEnableClientState(GL_VERTEX_ARRAY);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableClientState(GL_VERTEX_ARRAY);
}

/* Draw a 1px outline rectangle */
static void draw_rect_outline(float x, float y, float w, float h,
                              float r, float g, float b, float a)
{
    draw_rect(x, y, w, 1, r, g, b, a);           /* top */
    draw_rect(x, y + h - 1, w, 1, r, g, b, a);   /* bottom */
    draw_rect(x, y, 1, h, r, g, b, a);            /* left */
    draw_rect(x + w - 1, y, 1, h, r, g, b, a);   /* right */
}

/* Update popup dimensions */
#define UPDATE_POPUP_W   600
#define UPDATE_POPUP_MAX_H 600
#define UPDATE_LINE_H    20   /* line height for note text at scale 2 */
#define UPDATE_MAX_LINES 20

/* Word-wrap note text into lines; returns line count */
static int wrap_note_lines(const char *note, int max_w,
                           char lines[][64], int max_lines)
{
    int count = 0;
    const char *p = note;

    while (*p && count < max_lines) {
        const char *line_start = p;
        const char *last_break = NULL;
        int w = 0;
        int char_w = 8 * 2;  /* scale 2 font width */

        /* Scan forward, tracking last word boundary */
        while (*p && *p != '\n') {
            if (*p == ' ') last_break = p;
            w += char_w;
            if (w > max_w && last_break) {
                /* Wrap at last space */
                int len = (int)(last_break - line_start);
                if (len > 63) len = 63;
                memcpy(lines[count], line_start, len);
                lines[count][len] = '\0';
                count++;
                p = last_break + 1;
                line_start = p;
                last_break = NULL;
                w = 0;
                if (count >= max_lines) return count;
                continue;
            }
            p++;
        }
        /* Remainder of this line (or up to \n) */
        {
            int len = (int)(p - line_start);
            if (len > 63) len = 63;
            memcpy(lines[count], line_start, len);
            lines[count][len] = '\0';
            count++;
        }
        if (*p == '\n') p++;
    }
    return count;
}

/* Compute dynamic popup height for update popup */
static int update_popup_height(int note_lines)
{
    /* POPUP_PAD + title(16) + gap(8) + note_lines*LINE_H + gap(12) + RESUME_H + POPUP_PAD */
    int h = POPUP_PAD + 16 + 8 + note_lines * UPDATE_LINE_H + 12 + RESUME_H + POPUP_PAD;
    if (h < 120) h = 120;
    if (h > UPDATE_POPUP_MAX_H) h = UPDATE_POPUP_MAX_H;
    return h;
}

/* Draw the update available popup */
static void draw_update_popup(void)
{
    int popup_x, popup_y, popup_h;
    int tw, text_y;
    int gap, total_w, bx, by;
    int btn_w;
    char title[80];
    char note_lines[UPDATE_MAX_LINES][64];
    int note_count = 0;
    int i;

    if (!g_update_popup_visible) return;

    /* Word-wrap note text */
    {
        const char *note = updater_get_note();
        if (note && note[0]) {
            int max_w = UPDATE_POPUP_W - 2 * POPUP_PAD;
            note_count = wrap_note_lines(note, max_w, note_lines, UPDATE_MAX_LINES);
        }
    }

    popup_h = update_popup_height(note_count);
    popup_x = (SCREEN_WIDTH - UPDATE_POPUP_W) / 2;
    popup_y = (SCREEN_HEIGHT - popup_h) / 2;

    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);

    /* Semi-transparent overlay */
    draw_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0.0f, 0.0f, 0.0f, 0.6f);

    /* Black filled popup box */
    draw_rect(popup_x, popup_y, UPDATE_POPUP_W, popup_h,
              0.0f, 0.0f, 0.0f, 1.0f);

    /* Orange outline */
    draw_rect_outline(popup_x, popup_y, UPDATE_POPUP_W, popup_h,
                      1.0f, 0.5f, 0.15f, 0.8f);

    /* Title: "Update Available: v1.6.0" */
    snprintf(title, sizeof(title), "Update Available: v%s", updater_get_version());
    text_y = popup_y + POPUP_PAD;
    tw = font_string_width(title, 2);
    font_draw_string(title, popup_x + (UPDATE_POPUP_W - tw) / 2, text_y, 2,
                     1.0f, 0.5f, 0.15f, 1.0f);

    /* Version note lines (grey, left-aligned with padding) */
    {
        int note_y = text_y + 16 + 8;
        for (i = 0; i < note_count; i++) {
            font_draw_string(note_lines[i],
                             popup_x + POPUP_PAD, note_y + i * UPDATE_LINE_H, 2,
                             0.6f, 0.6f, 0.6f, 1.0f);
        }
    }

    /* Two buttons: UPDATE (orange) and LATER (grey) */
    {
        int w1 = font_string_width("UPDATE", 2) + 16;
        int w2 = font_string_width("LATER", 2) + 16;
        btn_w = w1 > w2 ? w1 : w2;
        gap = 16;
        total_w = btn_w * 2 + gap;
        bx = popup_x + (UPDATE_POPUP_W - total_w) / 2;
        by = popup_y + popup_h - POPUP_PAD - RESUME_H;

        glDisable(GL_TEXTURE_2D);

        /* UPDATE button (grey bg, orange text) */
        draw_rect(bx, by, btn_w, RESUME_H, 0.3f, 0.3f, 0.3f, 0.6f);
        tw = font_string_width("UPDATE", 2);
        font_draw_string("UPDATE", bx + (btn_w - tw) / 2, by + 12, 2,
                         1.0f, 0.5f, 0.15f, 1.0f);
        bx += btn_w + gap;

        /* LATER button (grey bg, orange text) */
        draw_rect(bx, by, btn_w, RESUME_H, 0.3f, 0.3f, 0.3f, 0.6f);
        tw = font_string_width("LATER", 2);
        font_draw_string("LATER", bx + (btn_w - tw) / 2, by + 12, 2,
                         1.0f, 0.5f, 0.15f, 1.0f);
    }
}

/* Directory ask popup dimensions */
#define DIRASK_W  600
#define DIRASK_H  120

/* Draw the "Set a default ROM directory?" popup */
static void draw_dirask_popup(void)
{
    int popup_x, popup_y;
    int tw, text_y;
    int total_w, gap, bx, by;

    if (!g_dirask_popup_visible) return;

    popup_x = (SCREEN_WIDTH - DIRASK_W) / 2;
    popup_y = (SCREEN_HEIGHT - DIRASK_H) / 2;

    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);

    /* Semi-transparent overlay */
    draw_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0.0f, 0.0f, 0.0f, 0.6f);

    /* Black filled popup box */
    draw_rect(popup_x, popup_y, DIRASK_W, DIRASK_H, 0.0f, 0.0f, 0.0f, 1.0f);

    /* Orange outline */
    draw_rect_outline(popup_x, popup_y, DIRASK_W, DIRASK_H,
                      1.0f, 0.5f, 0.15f, 0.8f);

    /* Two lines of text */
    text_y = popup_y + POPUP_PAD;
    tw = font_string_width("Set a default ROM directory now?", 2);
    font_draw_string("Set a default ROM directory now?",
                     popup_x + (DIRASK_W - tw) / 2, text_y, 2,
                     1.0f, 0.5f, 0.15f, 0.8f);
    text_y += 20;
    tw = font_string_width("You can do this later in Settings.", 2);
    font_draw_string("You can do this later in Settings.",
                     popup_x + (DIRASK_W - tw) / 2, text_y, 2,
                     0.6f, 0.6f, 0.6f, 0.8f);

    /* Two buttons: Yes | Later */
    {
        int w1 = font_string_width("Yes", 2) + 16;
        int w2 = font_string_width("Later", 2) + 16;
        int btn_w = w1 > w2 ? w1 : w2;
        gap = 16;
        total_w = btn_w * 2 + gap;
        bx = popup_x + (DIRASK_W - total_w) / 2;
        by = popup_y + DIRASK_H - POPUP_PAD - RESUME_H;

        glDisable(GL_TEXTURE_2D);

        /* Yes */
        draw_rect(bx, by, btn_w, RESUME_H, 0.3f, 0.3f, 0.3f, 0.6f);
        tw = font_string_width("Yes", 2);
        font_draw_string("Yes", bx + (btn_w - tw) / 2, by + 12, 2, 1.0f, 0.5f, 0.15f, 1.0f);
        bx += btn_w + gap;

        /* Later */
        draw_rect(bx, by, btn_w, RESUME_H, 0.3f, 0.3f, 0.3f, 0.6f);
        tw = font_string_width("Later", 2);
        font_draw_string("Later", bx + (btn_w - tw) / 2, by + 12, 2, 1.0f, 0.5f, 0.15f, 1.0f);
    }
}

/* Directory picker popup dimensions */
#define DIRPICKER_W        600
#define DIRPICKER_VISIBLE  8
#define DIRPICKER_ITEM_H   40
#define DIRPICKER_LIST_H   (DIRPICKER_VISIBLE * DIRPICKER_ITEM_H)
#define DIRPICKER_TITLE_H  28  /* scale 3 title height */
#define DIRPICKER_H        (POPUP_PAD + DIRPICKER_TITLE_H + 8 + DIRPICKER_LIST_H + 12 + RESUME_H + POPUP_PAD)

/* Draw the directory picker popup */
static void draw_dirpicker_popup(void)
{
    int popup_x, popup_y;
    int tw, text_y, list_y, i;
    int set_w, bx, by;
    int visible_height, total_height;
    float max_scroll;
    int max_text_w;

    if (!g_dirpicker_popup_visible) return;

    popup_x = (SCREEN_WIDTH - DIRPICKER_W) / 2;
    popup_y = (SCREEN_HEIGHT - DIRPICKER_H) / 2;

    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);

    /* Semi-transparent overlay */
    draw_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0.0f, 0.0f, 0.0f, 0.6f);

    /* Black filled popup box */
    draw_rect(popup_x, popup_y, DIRPICKER_W, DIRPICKER_H, 0.0f, 0.0f, 0.0f, 1.0f);

    /* Orange outline */
    draw_rect_outline(popup_x, popup_y, DIRPICKER_W, DIRPICKER_H,
                      1.0f, 0.5f, 0.15f, 0.8f);

    /* Title */
    text_y = popup_y + POPUP_PAD;
    tw = font_string_width("Choose ROM Directory", 3);
    font_draw_string("Choose ROM Directory",
                     popup_x + (DIRPICKER_W - tw) / 2, text_y, 3,
                     1.0f, 0.5f, 0.15f, 1.0f);

    max_text_w = DIRPICKER_W - 2 * POPUP_PAD;

    /* Directory list area */
    list_y = text_y + DIRPICKER_TITLE_H + 8;

    /* Clamp scroll */
    visible_height = DIRPICKER_LIST_H;
    total_height = g_dirpicker_count * DIRPICKER_ITEM_H;
    max_scroll = total_height - visible_height;
    if (max_scroll < 0) max_scroll = 0;
    if (g_dirpicker_scroll < 0) g_dirpicker_scroll = 0;
    if (g_dirpicker_scroll > max_scroll) g_dirpicker_scroll = max_scroll;

    if (g_dirpicker_count == 0) {
        /* No subdirectories message */
        tw = font_string_width("No subdirectories", 2);
        font_draw_string("No subdirectories",
                         popup_x + (DIRPICKER_W - tw) / 2,
                         list_y + DIRPICKER_LIST_H / 2 - 8, 2,
                         0.5f, 0.5f, 0.5f, 0.8f);
    } else {
        for (i = 0; i < g_dirpicker_count; i++) {
            float iy = list_y + i * DIRPICKER_ITEM_H - g_dirpicker_scroll;
            char disp[MAX_NAME_LEN + 4];

            /* Skip items outside visible list area */
            if (iy + DIRPICKER_ITEM_H <= list_y || iy >= list_y + DIRPICKER_LIST_H)
                continue;

            if (strcmp(g_dirpicker_dirs[i].name, "..") == 0) {
                strncpy(disp, "../", sizeof(disp) - 1);
            } else {
                snprintf(disp, sizeof(disp), "%s/", g_dirpicker_dirs[i].name);
            }
            disp[sizeof(disp) - 1] = '\0';

            /* Truncate if too wide */
            tw = font_string_width(disp, 2);
            if (tw > max_text_w) {
                int dots_w = font_string_width("...", 2);
                int max_chars = (max_text_w - dots_w) / (8 * 2);
                if (max_chars < 0) max_chars = 0;
                disp[max_chars] = '\0';
                strcat(disp, "...");
            }

            if (strcmp(g_dirpicker_dirs[i].name, "..") == 0) {
                font_draw_string(disp, popup_x + POPUP_PAD,
                                 (int)(iy + (DIRPICKER_ITEM_H - 16) / 2), 2,
                                 0.6f, 0.6f, 0.6f, 1.0f);
            } else {
                font_draw_string(disp, popup_x + POPUP_PAD,
                                 (int)(iy + (DIRPICKER_ITEM_H - 16) / 2), 2,
                                 1.0f, 0.5f, 0.15f, 0.8f);
            }
        }
    }

    /* Scrollbar (only when content overflows) */
    if (total_height > visible_height) {
        int sb_w = 6;
        int sb_x = popup_x + DIRPICKER_W - POPUP_PAD - sb_w;
        float thumb_frac = (float)visible_height / total_height;
        int thumb_h = (int)(DIRPICKER_LIST_H * thumb_frac);
        int scroll_range = DIRPICKER_LIST_H - thumb_h;
        int thumb_y;
        if (thumb_h < 20) thumb_h = 20;
        scroll_range = DIRPICKER_LIST_H - thumb_h;
        thumb_y = list_y + (int)(scroll_range * (g_dirpicker_scroll / max_scroll));

        glDisable(GL_TEXTURE_2D);
        /* Track */
        draw_rect(sb_x, list_y, sb_w, DIRPICKER_LIST_H,
                  0.3f, 0.3f, 0.3f, 0.4f);
        /* Thumb */
        draw_rect(sb_x, thumb_y, sb_w, thumb_h,
                  1.0f, 0.5f, 0.15f, 0.6f);
    }

    /* Centered Set button */
    set_w = font_string_width("Set", 2) + 16;
    bx = popup_x + (DIRPICKER_W - set_w) / 2;
    by = popup_y + DIRPICKER_H - POPUP_PAD - RESUME_H;

    glDisable(GL_TEXTURE_2D);

    draw_rect(bx, by, set_w, RESUME_H, 0.3f, 0.3f, 0.3f, 0.6f);
    tw = font_string_width("Set", 2);
    font_draw_string("Set", bx + (set_w - tw) / 2, by + 12, 2, 1.0f, 0.5f, 0.15f, 1.0f);
}

/* Draw the recently played popup overlay */
static void draw_recent_popup(void)
{
    int popup_h, popup_x, popup_y;
    int items_y, i;
    int btn_w, btn_x, btn_y;
    int title_w, max_text_w;

    if (!g_recent_popup_visible || g_recent_count == 0) return;

    {
        int recent_title_h = 28;  /* scale 3 title height */
        popup_h = POPUP_PAD + recent_title_h + POPUP_GAP
                + g_recent_count * POPUP_ITEM_H
                + POPUP_GAP + POPUP_BTN_H + POPUP_PAD;
        popup_x = (SCREEN_WIDTH - POPUP_W) / 2;
        popup_y = (SCREEN_HEIGHT - popup_h) / 2;
    }

    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);

    /* Semi-transparent overlay */
    draw_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0.0f, 0.0f, 0.0f, 0.6f);

    /* Black filled popup box */
    draw_rect(popup_x, popup_y, POPUP_W, popup_h, 0.0f, 0.0f, 0.0f, 1.0f);

    /* Orange outline */
    draw_rect_outline(popup_x, popup_y, POPUP_W, popup_h,
                      1.0f, 0.5f, 0.15f, 0.8f);

    /* Title "Recently Played" centered, scale 3 */
    title_w = font_string_width("Recently Played", 3);
    font_draw_string("Recently Played",
                     popup_x + (POPUP_W - title_w) / 2,
                     popup_y + POPUP_PAD, 3,
                     1.0f, 0.5f, 0.15f, 1.0f);

    /* Item list */
    items_y = popup_y + POPUP_PAD + 28 + POPUP_GAP;
    max_text_w = POPUP_W - 2 * POPUP_PAD;

    for (i = 0; i < g_recent_count; i++) {
        int iy = items_y + i * POPUP_ITEM_H;
        char basename[MAX_NAME_LEN];
        char numbered[MAX_NAME_LEN + 4];
        const char *slash, *dot;
        int tw;

        /* Extract basename without extension */
        slash = strrchr(g_recent_paths[i], '/');
        strncpy(basename, slash ? slash + 1 : g_recent_paths[i], MAX_NAME_LEN - 1);
        basename[MAX_NAME_LEN - 1] = '\0';
        dot = strrchr(basename, '.');
        if (dot) basename[dot - basename] = '\0';

        /* Prefix with number */
        snprintf(numbered, sizeof(numbered), "%d. %s", i + 1, basename);

        /* Truncate with "..." if too wide */
        tw = font_string_width(numbered, 2);
        if (tw > max_text_w) {
            int dots_w = font_string_width("...", 2);
            int max_chars = (max_text_w - dots_w) / (8 * 2);
            if (max_chars < 0) max_chars = 0;
            numbered[max_chars] = '\0';
            strncat(numbered, "...", sizeof(numbered) - 1 - max_chars);
        }

        font_draw_string(numbered,
                         popup_x + POPUP_PAD,
                         iy + (POPUP_ITEM_H - 16) / 2, 2,
                         0.9f, 0.9f, 0.9f, 0.9f);
    }

    /* "Clear List" button centered at bottom */
    btn_w = font_string_width("Clear List", 2) + 16;
    btn_x = popup_x + (POPUP_W - btn_w) / 2;
    btn_y = popup_y + popup_h - POPUP_PAD - POPUP_BTN_H;

    glDisable(GL_TEXTURE_2D);
    draw_rect(btn_x, btn_y, btn_w, POPUP_BTN_H,
              0.3f, 0.3f, 0.3f, 0.6f);
    {
        int clr_tw = font_string_width("Clear List", 2);
        font_draw_string("Clear List",
                         btn_x + (btn_w - clr_tw) / 2,
                         btn_y + (POPUP_BTN_H - 16) / 2, 2,
                         1.0f, 0.5f, 0.15f, 1.0f);
    }
}

/* Settings popup layout */
#define SETTINGS_POPUP_W     500
#define SETTINGS_ROW_H       44
#define SETTINGS_ROWS        9
#define SETTINGS_TITLE_H     28  /* scale 3 title height (matches Options popup) */
#define SETTINGS_POPUP_H     (POPUP_PAD + SETTINGS_TITLE_H + POPUP_PAD + SETTINGS_ROWS * SETTINGS_ROW_H + POPUP_PAD)

/* Auto-save warning popup (filepicker) */
#define FP_ASWARN_W   640
#define FP_ASWARN_H   210
#define FP_ASWARN_BTN_W 80
#define FP_ASWARN_BTN_H 36

/* Draw the settings popup overlay */
static void draw_settings_popup(void)
{
    int popup_x, popup_y;
    int tw, text_y, row_y;
    int btn_w, btn_x;

    if (!g_settings_popup_visible) return;

    popup_x = (SCREEN_WIDTH - SETTINGS_POPUP_W) / 2;
    popup_y = (SCREEN_HEIGHT - SETTINGS_POPUP_H) / 2;

    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);

    /* Semi-transparent overlay */
    draw_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0.0f, 0.0f, 0.0f, 0.6f);

    /* Black filled popup box */
    draw_rect(popup_x, popup_y, SETTINGS_POPUP_W, SETTINGS_POPUP_H, 0.0f, 0.0f, 0.0f, 1.0f);

    /* Orange outline */
    draw_rect_outline(popup_x, popup_y, SETTINGS_POPUP_W, SETTINGS_POPUP_H,
                      1.0f, 0.5f, 0.15f, 0.8f);

    /* Title "Settings" centered */
    text_y = popup_y + POPUP_PAD;
    tw = font_string_width("Settings", 3);
    font_draw_string("Settings",
                     popup_x + (SETTINGS_POPUP_W - tw) / 2, text_y, 3,
                     1.0f, 0.5f, 0.15f, 1.0f);

    /* Row area starts after title */
    row_y = text_y + SETTINGS_TITLE_H + POPUP_PAD;

    /* Compute uniform button width (widest of all labels + padding) */
    {
        int w1 = font_string_width("MEDIUM", 2);
        int w2 = font_string_width("EMAIL", 2);
        int w3 = font_string_width("BRIGHT", 2);
        btn_w = w1 > w2 ? w1 : w2;
        if (w3 > btn_w) btn_w = w3;
        btn_w += 16;
    }

    /* Row 0: Scanlines — label left, toggle button right */
    {
        const char *state = video_get_scanlines() ? "ON" : "OFF";
        btn_x = popup_x + SETTINGS_POPUP_W - POPUP_PAD - btn_w;

        font_draw_string("Scanlines", popup_x + POPUP_PAD,
                         row_y + (SETTINGS_ROW_H - 16) / 2, 2,
                         0.9f, 0.9f, 0.9f, 0.9f);

        glDisable(GL_TEXTURE_2D);
        draw_rect(btn_x, row_y + (SETTINGS_ROW_H - RESUME_H) / 2,
                  btn_w, RESUME_H, 0.3f, 0.3f, 0.3f, 0.6f);
        tw = font_string_width(state, 2);
        font_draw_string(state, btn_x + (btn_w - tw) / 2,
                         row_y + (SETTINGS_ROW_H - RESUME_H) / 2 + 12, 2,
                         1.0f, 0.5f, 0.15f, 1.0f);
    }
    row_y += SETTINGS_ROW_H;

    /* Row 1: Scanline Brightness — label left, cycle button right */
    {
        const char *br_label = video_get_scanline_brightness_label();
        btn_x = popup_x + SETTINGS_POPUP_W - POPUP_PAD - btn_w;

        font_draw_string("Scanline Brightness", popup_x + POPUP_PAD,
                         row_y + (SETTINGS_ROW_H - 16) / 2, 2,
                         0.9f, 0.9f, 0.9f, 0.9f);

        glDisable(GL_TEXTURE_2D);
        draw_rect(btn_x, row_y + (SETTINGS_ROW_H - RESUME_H) / 2,
                  btn_w, RESUME_H, 0.3f, 0.3f, 0.3f, 0.6f);
        tw = font_string_width(br_label, 2);
        font_draw_string(br_label, btn_x + (btn_w - tw) / 2,
                         row_y + (SETTINGS_ROW_H - RESUME_H) / 2 + 12, 2,
                         1.0f, 0.5f, 0.15f, 1.0f);
    }
    row_y += SETTINGS_ROW_H;

    /* Row 2: Palette (7800) — label left, cycle button right */
    {
        const char *pal_label = video_get_palette_label();
        btn_x = popup_x + SETTINGS_POPUP_W - POPUP_PAD - btn_w;

        font_draw_string("Palette (7800)", popup_x + POPUP_PAD,
                         row_y + (SETTINGS_ROW_H - 16) / 2, 2,
                         0.9f, 0.9f, 0.9f, 0.9f);

        glDisable(GL_TEXTURE_2D);
        draw_rect(btn_x, row_y + (SETTINGS_ROW_H - RESUME_H) / 2,
                  btn_w, RESUME_H, 0.3f, 0.3f, 0.3f, 0.6f);
        tw = font_string_width(pal_label, 2);
        font_draw_string(pal_label, btn_x + (btn_w - tw) / 2,
                         row_y + (SETTINGS_ROW_H - RESUME_H) / 2 + 12, 2,
                         1.0f, 0.5f, 0.15f, 1.0f);
    }
    row_y += SETTINGS_ROW_H;

    /* Row 3: Control Brightness — label left, cycle button right */
    {
        const char *dim_label;
        switch (input_get_control_dim()) {
            case 1: dim_label = "DIM"; break;
            case 2: dim_label = "DIMMER"; break;
            default: dim_label = "BRIGHT"; break;
        }
        btn_x = popup_x + SETTINGS_POPUP_W - POPUP_PAD - btn_w;

        font_draw_string("Control Brightness", popup_x + POPUP_PAD,
                         row_y + (SETTINGS_ROW_H - 16) / 2, 2,
                         0.9f, 0.9f, 0.9f, 0.9f);

        glDisable(GL_TEXTURE_2D);
        draw_rect(btn_x, row_y + (SETTINGS_ROW_H - RESUME_H) / 2,
                  btn_w, RESUME_H, 0.3f, 0.3f, 0.3f, 0.6f);
        tw = font_string_width(dim_label, 2);
        font_draw_string(dim_label, btn_x + (btn_w - tw) / 2,
                         row_y + (SETTINGS_ROW_H - RESUME_H) / 2 + 12, 2,
                         1.0f, 0.5f, 0.15f, 1.0f);
    }
    row_y += SETTINGS_ROW_H;

    /* Row 4: Auto-Save on Close — label left, toggle button right */
    {
        const char *as_label = input_get_autosave() ? "ON" : "OFF";
        btn_x = popup_x + SETTINGS_POPUP_W - POPUP_PAD - btn_w;

        font_draw_string("Auto-Save on Close", popup_x + POPUP_PAD,
                         row_y + (SETTINGS_ROW_H - 16) / 2, 2,
                         0.9f, 0.9f, 0.9f, 0.9f);

        glDisable(GL_TEXTURE_2D);
        draw_rect(btn_x, row_y + (SETTINGS_ROW_H - RESUME_H) / 2,
                  btn_w, RESUME_H, 0.3f, 0.3f, 0.3f, 0.6f);
        tw = font_string_width(as_label, 2);
        font_draw_string(as_label, btn_x + (btn_w - tw) / 2,
                         row_y + (SETTINGS_ROW_H - RESUME_H) / 2 + 12, 2,
                         1.0f, 0.5f, 0.15f, 1.0f);
    }
    row_y += SETTINGS_ROW_H;

    /* Row 5: Ask Before Saving — label left, toggle button right (greyed when autosave OFF) */
    {
        const char *ask_label = input_get_autosave_ask() ? "ON" : "OFF";
        int greyed = !input_get_autosave();
        float lbl_a = greyed ? 0.4f : 0.9f;
        float btn_bg_r = greyed ? 0.2f : 0.3f;
        float btn_bg_g = greyed ? 0.2f : 0.3f;
        float btn_bg_b = greyed ? 0.2f : 0.3f;
        float btn_bg_a = greyed ? 0.3f : 0.6f;
        btn_x = popup_x + SETTINGS_POPUP_W - POPUP_PAD - btn_w;

        font_draw_string("Ask Before Saving", popup_x + POPUP_PAD,
                         row_y + (SETTINGS_ROW_H - 16) / 2, 2,
                         0.9f, 0.9f, 0.9f, lbl_a);

        glDisable(GL_TEXTURE_2D);
        draw_rect(btn_x, row_y + (SETTINGS_ROW_H - RESUME_H) / 2,
                  btn_w, RESUME_H, btn_bg_r, btn_bg_g, btn_bg_b, btn_bg_a);
        tw = font_string_width(ask_label, 2);
        font_draw_string(ask_label, btn_x + (btn_w - tw) / 2,
                         row_y + (SETTINGS_ROW_H - RESUME_H) / 2 + 12, 2,
                         greyed ? 0.4f : 1.0f,
                         greyed ? 0.4f : 0.5f,
                         greyed ? 0.4f : 0.15f,
                         greyed ? 0.5f : 1.0f);
    }
    row_y += SETTINGS_ROW_H;

    /* Row 6: Change ROM Directory — label left, SET button right */
    {
        btn_x = popup_x + SETTINGS_POPUP_W - POPUP_PAD - btn_w;

        font_draw_string("Change ROM Directory", popup_x + POPUP_PAD,
                         row_y + (SETTINGS_ROW_H - 16) / 2, 2,
                         0.9f, 0.9f, 0.9f, 0.9f);

        glDisable(GL_TEXTURE_2D);
        draw_rect(btn_x, row_y + (SETTINGS_ROW_H - RESUME_H) / 2,
                  btn_w, RESUME_H, 0.3f, 0.3f, 0.3f, 0.6f);
        tw = font_string_width("SET", 2);
        font_draw_string("SET", btn_x + (btn_w - tw) / 2,
                         row_y + (SETTINGS_ROW_H - RESUME_H) / 2 + 12, 2,
                         1.0f, 0.5f, 0.15f, 1.0f);
    }
    row_y += SETTINGS_ROW_H;

    /* Row 7: Bug Report — label left, EMAIL button right */
    {
        btn_x = popup_x + SETTINGS_POPUP_W - POPUP_PAD - btn_w;

        font_draw_string("Bug Report", popup_x + POPUP_PAD,
                         row_y + (SETTINGS_ROW_H - 16) / 2, 2,
                         0.9f, 0.9f, 0.9f, 0.9f);

        glDisable(GL_TEXTURE_2D);
        draw_rect(btn_x, row_y + (SETTINGS_ROW_H - RESUME_H) / 2,
                  btn_w, RESUME_H, 0.3f, 0.3f, 0.3f, 0.6f);
        tw = font_string_width("EMAIL", 2);
        font_draw_string("EMAIL", btn_x + (btn_w - tw) / 2,
                         row_y + (SETTINGS_ROW_H - RESUME_H) / 2 + 12, 2,
                         1.0f, 0.5f, 0.15f, 1.0f);
    }
    row_y += SETTINGS_ROW_H;

    /* Row 8: About EMU7800 — label left, INFO button right */
    {
        btn_x = popup_x + SETTINGS_POPUP_W - POPUP_PAD - btn_w;

        font_draw_string("About EMU7800", popup_x + POPUP_PAD,
                         row_y + (SETTINGS_ROW_H - 16) / 2, 2,
                         0.9f, 0.9f, 0.9f, 0.9f);

        glDisable(GL_TEXTURE_2D);
        draw_rect(btn_x, row_y + (SETTINGS_ROW_H - RESUME_H) / 2,
                  btn_w, RESUME_H, 0.3f, 0.3f, 0.3f, 0.6f);
        tw = font_string_width("INFO", 2);
        font_draw_string("INFO", btn_x + (btn_w - tw) / 2,
                         row_y + (SETTINGS_ROW_H - RESUME_H) / 2 + 12, 2,
                         1.0f, 0.5f, 0.15f, 1.0f);
    }
}

/* About popup dimensions (content is fixed) */
#define ABOUT_W         800
#define ABOUT_TITLE_H   28  /* scale 3 title height */
#define ABOUT_LINES     8
#define ABOUT_GAPS      3
#define ABOUT_CONTENT_H (ABOUT_LINES * 20 + ABOUT_GAPS * 8)
#define ABOUT_H         (POPUP_PAD + ABOUT_TITLE_H + POPUP_PAD + ABOUT_CONTENT_H + POPUP_PAD)

/* Draw the about popup overlay */
static void draw_about_popup(void)
{
    static const char *lines[] = {
        "EMU7800 is a 100% Claude.ai vibe-coded port",
        "by Alan Morford.",
        "",
        "EMU7800 was written by Mike Murphy.",
        "",
        "This port includes code from Stella 7.0 by",
        "Bradford W. Mott, Stephen Anthony and the",
        "Stella Team.",
        "",
        "Both apps and this port are licensed under",
        "GPL 2.0.",
        NULL
    };
    int popup_x, popup_y;
    int text_y, i;

    if (!g_about_popup_visible) return;

    popup_x = (SCREEN_WIDTH - ABOUT_W) / 2;
    popup_y = (SCREEN_HEIGHT - ABOUT_H) / 2;

    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);

    /* Semi-transparent overlay */
    draw_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0.0f, 0.0f, 0.0f, 0.6f);

    /* Black filled popup box */
    draw_rect(popup_x, popup_y, ABOUT_W, ABOUT_H, 0.0f, 0.0f, 0.0f, 1.0f);

    /* Orange outline */
    draw_rect_outline(popup_x, popup_y, ABOUT_W, ABOUT_H,
                      1.0f, 0.5f, 0.15f, 0.8f);

    /* Title */
    text_y = popup_y + POPUP_PAD;
    {
        int tw = font_string_width("About", 3);
        font_draw_string("About",
                         popup_x + (ABOUT_W - tw) / 2, text_y, 3,
                         1.0f, 0.5f, 0.15f, 1.0f);
    }

    /* Body text (white) */
    text_y += ABOUT_TITLE_H + POPUP_PAD;
    for (i = 0; lines[i] != NULL; i++) {
        if (lines[i][0] == '\0') {
            text_y += 8;  /* paragraph gap */
        } else {
            font_draw_string(lines[i],
                             popup_x + (ABOUT_W - font_string_width(lines[i], 2)) / 2,
                             text_y, 2,
                             0.9f, 0.9f, 0.9f, 0.9f);
            text_y += 20;
        }
    }
}

/* Save popup dimensions */
#define SAVE_POPUP_W  600
#define SAVE_POPUP_H  128  /* PAD(16) + text(40) + gap(16) + btn(40) + PAD(16) */

/* Draw the save popup overlay */
static void draw_save_popup(void)
{
    int popup_x, popup_y;
    int text_y;
    int total_w, gap, bx, by;
    int tw;

    if (!g_save_popup_visible) return;

    popup_x = (SCREEN_WIDTH - SAVE_POPUP_W) / 2;
    popup_y = (SCREEN_HEIGHT - SAVE_POPUP_H) / 2;

    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);

    /* Semi-transparent overlay */
    draw_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0.0f, 0.0f, 0.0f, 0.6f);

    /* Black filled popup box */
    draw_rect(popup_x, popup_y, SAVE_POPUP_W, SAVE_POPUP_H, 0.0f, 0.0f, 0.0f, 1.0f);

    /* Orange outline */
    draw_rect_outline(popup_x, popup_y, SAVE_POPUP_W, SAVE_POPUP_H,
                      1.0f, 0.5f, 0.15f, 0.8f);

    /* Text: two centered lines */
    text_y = popup_y + POPUP_PAD;
    tw = font_string_width("Would you like to continue", 2);
    font_draw_string("Would you like to continue",
                     popup_x + (SAVE_POPUP_W - tw) / 2, text_y, 2,
                     1.0f, 0.5f, 0.15f, 0.8f);
    text_y += 20;
    tw = font_string_width("from your Save?", 2);
    font_draw_string("from your Save?",
                     popup_x + (SAVE_POPUP_W - tw) / 2, text_y, 2,
                     1.0f, 0.5f, 0.15f, 0.8f);

    /* Three equal-width buttons centered below text */
    {
        int w1 = font_string_width("Yes", 2) + 16;
        int w2 = font_string_width("No", 2) + 16;
        int w3 = font_string_width("Delete", 2) + 16;
        int btn_w = w1 > w2 ? w1 : w2;
        if (w3 > btn_w) btn_w = w3;
        gap = 16;
        total_w = btn_w * 3 + gap * 2;
        bx = popup_x + (SAVE_POPUP_W - total_w) / 2;
        by = popup_y + POPUP_PAD + 40 + 16;

        glDisable(GL_TEXTURE_2D);

        /* Yes button */
        draw_rect(bx, by, btn_w, RESUME_H, 0.3f, 0.3f, 0.3f, 0.6f);
        tw = font_string_width("Yes", 2);
        font_draw_string("Yes", bx + (btn_w - tw) / 2, by + 12, 2, 1.0f, 0.5f, 0.15f, 1.0f);
        if (g_keyboard_detected) {
            tw = font_string_width("Y", 2);
            font_draw_string("Y", bx + (btn_w - tw) / 2, by + 1, 2, 1.0f, 1.0f, 1.0f, 0.9f);
        }
        bx += btn_w + gap;

        /* No button */
        draw_rect(bx, by, btn_w, RESUME_H, 0.3f, 0.3f, 0.3f, 0.6f);
        tw = font_string_width("No", 2);
        font_draw_string("No", bx + (btn_w - tw) / 2, by + 12, 2, 1.0f, 0.5f, 0.15f, 1.0f);
        if (g_keyboard_detected) {
            tw = font_string_width("N", 2);
            font_draw_string("N", bx + (btn_w - tw) / 2, by + 1, 2, 1.0f, 1.0f, 1.0f, 0.9f);
        }
        bx += btn_w + gap;

        /* Delete button */
        draw_rect(bx, by, btn_w, RESUME_H, 0.3f, 0.3f, 0.3f, 0.6f);
        tw = font_string_width("Delete", 2);
        font_draw_string("Delete", bx + (btn_w - tw) / 2, by + 12, 2, 0.8f, 0.1f, 0.1f, 1.0f);
        if (g_keyboard_detected) {
            tw = font_string_width("D", 2);
            font_draw_string("D", bx + (btn_w - tw) / 2, by + 1, 2, 1.0f, 1.0f, 1.0f, 0.9f);
        }
    }
}

/* Delete confirmation popup (same dimensions as save popup) */
static void draw_delete_confirm_popup(void)
{
    int popup_x, popup_y;
    int text_y, tw;
    int total_w, gap, bx, by;

    if (!g_delete_confirm_visible) return;

    popup_x = (SCREEN_WIDTH - SAVE_POPUP_W) / 2;
    popup_y = (SCREEN_HEIGHT - SAVE_POPUP_H) / 2;

    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);

    /* Semi-transparent overlay */
    draw_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0.0f, 0.0f, 0.0f, 0.6f);

    /* Black filled popup box */
    draw_rect(popup_x, popup_y, SAVE_POPUP_W, SAVE_POPUP_H, 0.0f, 0.0f, 0.0f, 1.0f);

    /* Orange outline */
    draw_rect_outline(popup_x, popup_y, SAVE_POPUP_W, SAVE_POPUP_H,
                      1.0f, 0.5f, 0.15f, 0.8f);

    /* Text: two centered lines */
    text_y = popup_y + POPUP_PAD;
    tw = font_string_width("Are you sure you want to", 2);
    font_draw_string("Are you sure you want to",
                     popup_x + (SAVE_POPUP_W - tw) / 2, text_y, 2,
                     1.0f, 0.5f, 0.15f, 0.8f);
    text_y += 20;
    tw = font_string_width("delete the save file?", 2);
    font_draw_string("delete the save file?",
                     popup_x + (SAVE_POPUP_W - tw) / 2, text_y, 2,
                     1.0f, 0.5f, 0.15f, 0.8f);

    /* Two equal-width buttons centered below text */
    {
        int w1 = font_string_width("Yes", 2) + 16;
        int w2 = font_string_width("No", 2) + 16;
        int btn_w = w1 > w2 ? w1 : w2;
        gap = 16;
        total_w = btn_w * 2 + gap;
        bx = popup_x + (SAVE_POPUP_W - total_w) / 2;
        by = popup_y + POPUP_PAD + 40 + 16;

        glDisable(GL_TEXTURE_2D);

        /* Yes button */
        draw_rect(bx, by, btn_w, RESUME_H, 0.3f, 0.3f, 0.3f, 0.6f);
        tw = font_string_width("Yes", 2);
        font_draw_string("Yes", bx + (btn_w - tw) / 2, by + 12, 2, 1.0f, 0.5f, 0.15f, 1.0f);
        bx += btn_w + gap;

        /* No button */
        draw_rect(bx, by, btn_w, RESUME_H, 0.3f, 0.3f, 0.3f, 0.6f);
        tw = font_string_width("No", 2);
        font_draw_string("No", bx + (btn_w - tw) / 2, by + 12, 2, 1.0f, 0.5f, 0.15f, 1.0f);
    }
}

/* Build .sav path from ROM path */
static void build_sav_path(const char *rom_path, char *sav_path, int sav_path_size)
{
    const char *dot = strrchr(rom_path, '.');
    int base_len = dot ? (int)(dot - rom_path) : (int)strlen(rom_path);
    if (base_len + 5 >= sav_path_size)
        base_len = sav_path_size - 5;
    memcpy(sav_path, rom_path, base_len);
    memcpy(sav_path + base_len, ".sav", 5);
}

/* Check if selected ROM has a .sav file; if so, show popup and return 0.
 * Otherwise return 1 (proceed with selection). */
static int check_save_popup(void)
{
    const char *path = filepicker_get_selected_path();
    int type = filepicker_get_selected_type();
    char sav_path[MAX_PATH_LEN];
    FILE *f;

    if (!path) return 1;

    build_sav_path(path, sav_path, sizeof(sav_path));
    f = fopen(sav_path, "rb");
    if (f) {
        fclose(f);
        strncpy(g_save_popup_path, path, MAX_PATH_LEN - 1);
        g_save_popup_path[MAX_PATH_LEN - 1] = '\0';
        g_save_popup_type = type;
        g_save_popup_visible = 1;
        g_save_popup_load_save = 0;
        return 0;
    }
    return 1;
}

/* Resolve save popup: restore selection state from popup and return 1 */
static int resolve_save_popup(int load_save)
{
    g_save_popup_load_save = load_save;
    g_save_popup_visible = 0;
    strncpy(g_last_rom_path, g_save_popup_path, MAX_PATH_LEN - 1);
    g_last_rom_path[MAX_PATH_LEN - 1] = '\0';
    g_last_rom_type = g_save_popup_type;
    g_has_last_rom = 1;
    g_resume_selected = 1;
    g_art_selected = 0;
    return 1;
}

/* Draw auto-save warning dialog (filepicker) */
static void draw_fp_aswarn_popup(void)
{
    int popup_x, popup_y;
    int tw, text_y, i;
    int yes_x, no_x, btn_cy;
    static const char *warn_lines[] = {
        "If you already have a save file,",
        "this will overwrite it",
        "automatically when exiting a game",
        "to return to the ROM select",
        "screen. Are you sure you want to",
        NULL
    };
    const char *last_line;

    if (!g_fp_aswarn_visible) return;

    if (g_fp_aswarn_action == 0)
        last_line = "enable auto-save?";
    else
        last_line = "disable Ask Before Saving?";

    popup_x = (SCREEN_WIDTH - FP_ASWARN_W) / 2;
    popup_y = (SCREEN_HEIGHT - FP_ASWARN_H) / 2;

    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);

    /* Dark overlay on top of settings popup */
    draw_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0.0f, 0.0f, 0.0f, 0.4f);

    /* Box with orange border */
    draw_rect(popup_x - 1, popup_y - 1, FP_ASWARN_W + 2, FP_ASWARN_H + 2,
              1.0f, 0.5f, 0.15f, 1.0f);
    draw_rect(popup_x, popup_y, FP_ASWARN_W, FP_ASWARN_H,
              0.0f, 0.0f, 0.0f, 0.95f);

    /* Warning text (centered, orange, 6 lines) */
    text_y = popup_y + 16;
    for (i = 0; warn_lines[i] != NULL; i++) {
        tw = font_string_width(warn_lines[i], 2);
        font_draw_string(warn_lines[i], popup_x + (FP_ASWARN_W - tw) / 2,
                         text_y, 2, 1.0f, 0.5f, 0.15f, 0.8f);
        text_y += 18;
    }
    /* Last line (dynamic) */
    tw = font_string_width(last_line, 2);
    font_draw_string(last_line, popup_x + (FP_ASWARN_W - tw) / 2,
                     text_y, 2, 1.0f, 0.5f, 0.15f, 0.8f);

    /* Yes / No buttons */
    btn_cy = popup_y + FP_ASWARN_H - FP_ASWARN_BTN_H - 16;
    yes_x = popup_x + FP_ASWARN_W / 2 - FP_ASWARN_BTN_W - 20;
    no_x  = popup_x + FP_ASWARN_W / 2 + 20;

    /* Yes */
    draw_rect(yes_x, btn_cy, FP_ASWARN_BTN_W, FP_ASWARN_BTN_H,
              1.0f, 0.5f, 0.15f, 0.8f);
    tw = font_string_width("Yes", 2);
    font_draw_string("Yes", yes_x + (FP_ASWARN_BTN_W - tw) / 2,
                     btn_cy + 8, 2, 1.0f, 1.0f, 1.0f, 0.9f);

    /* No */
    draw_rect(no_x, btn_cy, FP_ASWARN_BTN_W, FP_ASWARN_BTN_H,
              0.3f, 0.3f, 0.3f, 0.8f);
    tw = font_string_width("No", 2);
    font_draw_string("No", no_x + (FP_ASWARN_BTN_W - tw) / 2,
                     btn_cy + 8, 2, 1.0f, 0.5f, 0.15f, 0.8f);
}

/* Draw the file picker UI */
void filepicker_draw(void)
{
    int total_height, visible_height;
    int i, item_y;
    float max_scroll;
    int has_dotdot, scroll_start, scroll_area_top, scroll_count;

    /* Clear screen to black */
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Animated logo background (centered, dimmed) */
    if (g_logo_texture) {
        Uint32 now = SDL_GetTicks();
        float logo_u = (float)LOGO_IMG_W / (float)LOGO_TEX_SIZE;
        float logo_v = (float)LOGO_IMG_H / (float)LOGO_TEX_SIZE;
        int logo_draw_w = LOGO_IMG_W * 3;
        int logo_draw_h = LOGO_IMG_H * 3;
        int logo_x = (SCREEN_WIDTH - logo_draw_w) / 2;
        int logo_y = (SCREEN_HEIGHT - logo_draw_h) / 2;
        GLfloat logo_verts[] = {
            logo_x,                logo_y,
            logo_x + logo_draw_w,  logo_y,
            logo_x,                logo_y + logo_draw_h,
            logo_x + logo_draw_w,  logo_y + logo_draw_h
        };
        GLfloat logo_tc[] = {
            0.0f,   0.0f,
            logo_u, 0.0f,
            0.0f,   logo_v,
            logo_u, logo_v
        };

        /* Advance frame if delay elapsed */
        if (g_logo_last_tick == 0) g_logo_last_tick = now;
        if (now - g_logo_last_tick >= logo_frame_delays[g_logo_frame]) {
            g_logo_frame = (g_logo_frame + 1) % LOGO_FRAME_COUNT;
            g_logo_last_tick = now;
            glBindTexture(GL_TEXTURE_2D, g_logo_texture);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                            LOGO_TEX_SIZE, LOGO_TEX_SIZE,
                            GL_RGB, GL_UNSIGNED_SHORT_5_6_5,
                            logo_frame_data[g_logo_frame]);
            glBindTexture(GL_TEXTURE_2D, 0);
        }

        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, g_logo_texture);
        glColor4f(1.0f, 1.0f, 1.0f, 0.3f);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glVertexPointer(2, GL_FLOAT, 0, logo_verts);
        glTexCoordPointer(2, GL_FLOAT, 0, logo_tc);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glDisableClientState(GL_VERTEX_ARRAY);
        glDisable(GL_TEXTURE_2D);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    }

    /* Title (right-aligned, cycling rainbow like string lights) */
    {
        static const char title[] = "EMU7800";
        static const float colors[][3] = {
            {1.0f, 0.0f, 0.0f},       /* red */
            {1.0f, 0.5f, 0.0f},       /* orange */
            {1.0f, 1.0f, 0.0f},       /* yellow */
            {0.0f, 0.8f, 0.0f},       /* green */
            {0.0f, 0.4f, 1.0f},       /* blue */
            {0.29f, 0.0f, 0.51f},     /* indigo */
            {0.56f, 0.0f, 1.0f}       /* violet */
        };
        static Uint32 last_tick = 0;
        static int color_step = 0;
        Uint32 now = SDL_GetTicks();
        int tw = font_string_width(title, 5);
        int cw = tw / 7;
        float tx = (float)(SCREEN_WIDTH - MARGIN_X - tw);
        int i, ci;
        char ch[2] = {0, 0};

        if (last_tick == 0) last_tick = now;
        if (now - last_tick >= 1000) {
            color_step = (color_step + 1) % 7;
            last_tick = now;
        }

        for (i = 0; i < 7; i++) {
            ci = (i + color_step) % 7;
            ch[0] = title[i];
            font_draw_string(ch, tx + i * cw, TITLE_Y, 5,
                             colors[ci][0], colors[ci][1], colors[ci][2], 1.0f);
        }
    }

    /* Gear icon (right-aligned under title, replaces "Settings" text) */
    {
        int gear_x = SCREEN_WIDTH - MARGIN_X - GEAR_IMG_W;
        int gear_y = TITLE_Y + 5 * 8 + 2;  /* just below scale-5 title */
        float u_max = (float)GEAR_IMG_W / GEAR_TEX_SIZE;
        float v_max = (float)GEAR_IMG_H / GEAR_TEX_SIZE;
        GLfloat gverts[8], gtc[8];

        gverts[0] = gear_x;                gverts[1] = gear_y;
        gverts[2] = gear_x + GEAR_IMG_W;   gverts[3] = gear_y;
        gverts[4] = gear_x;                gverts[5] = gear_y + GEAR_IMG_H;
        gverts[6] = gear_x + GEAR_IMG_W;   gverts[7] = gear_y + GEAR_IMG_H;

        gtc[0] = 0;      gtc[1] = 0;
        gtc[2] = u_max;  gtc[3] = 0;
        gtc[4] = 0;      gtc[5] = v_max;
        gtc[6] = u_max;  gtc[7] = v_max;

        glEnable(GL_TEXTURE_2D);
        glEnable(GL_BLEND);
        glBindTexture(GL_TEXTURE_2D, g_gear_texture);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glVertexPointer(2, GL_FLOAT, 0, gverts);
        glTexCoordPointer(2, GL_FLOAT, 0, gtc);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glDisableClientState(GL_VERTEX_ARRAY);
        glBindTexture(GL_TEXTURE_2D, 0);
        glDisable(GL_TEXTURE_2D);
    }

    /* Auto-show update popup once when update is found */
    if (updater_has_update() && !g_update_auto_shown && !g_update_popup_visible) {
        g_update_auto_shown = 1;
        g_update_popup_visible = 1;
    }

    /* "Update!" text link (to the left of gear icon — only after LATER) */
    if (updater_has_update() && g_update_later) {
        int gear_x = SCREEN_WIDTH - MARGIN_X - GEAR_IMG_W;
        int gear_y = TITLE_Y + 5 * 8 + 2;
        int update_w = font_string_width("Update!", 2);
        int update_x = gear_x - 4 - update_w;
        int update_y = gear_y + (GEAR_IMG_H - 16) / 2;  /* vertically centered with gear */
        font_draw_string("Update!", update_x, update_y, 2,
                         1.0f, 0.5f, 0.15f, 1.0f);
    }

    /* Resume button (top-left, only if last ROM is set) */
    if (g_has_last_rom) {
        const char *slash;
        const char *romname;
        const char *dot;
        char basename[MAX_NAME_LEN];
        int btn_w, namelen;

        /* Size button box to fit "RESUME" text */
        btn_w = font_string_width("RESUME", 2) + 16;

        glDisable(GL_TEXTURE_2D);
        draw_rect(RESUME_X, RESUME_Y, btn_w, RESUME_H,
                  0.3f, 0.3f, 0.3f, 0.6f);

        font_draw_string("RESUME", RESUME_X + 8, RESUME_Y + 12, 2,
                         1.0f, 0.5f, 0.15f, 1.0f);
        if (g_keyboard_detected) {
            int kw = font_string_width("1", 2);
            font_draw_string("1", RESUME_X + (btn_w - kw) / 2, RESUME_Y + 1, 2,
                             1.0f, 1.0f, 1.0f, 0.9f);
        }

        /* Show ROM filename without extension to the right of button */
        slash = strrchr(g_last_rom_path, '/');
        romname = slash ? slash + 1 : g_last_rom_path;
        strncpy(basename, romname, MAX_NAME_LEN - 1);
        basename[MAX_NAME_LEN - 1] = '\0';
        /* Strip file extension */
        dot = strrchr(basename, '.');
        if (dot) {
            namelen = dot - basename;
            basename[namelen] = '\0';
        }
        /* Truncate with "..." if it would go past screen center */
        {
            int name_x = RESUME_X + btn_w + 10;
            int max_w = SCREEN_WIDTH / 2 - name_x;
            int name_w = font_string_width(basename, 2);
            if (name_w > max_w && max_w > 0) {
                int dots_w = font_string_width("...", 2);
                int max_chars = (max_w - dots_w) / (8 * 2);
                if (max_chars < 0) max_chars = 0;
                basename[max_chars] = '\0';
                strncat(basename, "...", MAX_NAME_LEN - 1 - max_chars);
            }
            font_draw_string(basename, name_x, RESUME_Y + 12, 2,
                             0.9f, 0.9f, 0.9f, 0.9f);
        }
    }

    /* "RECENT" button below Resume */
    if (g_recent_count > 1) {
        int btn_w = font_string_width("RECENT", 2) + 16;
        int btn_y = RESUME_Y + RESUME_H + 4;
        glDisable(GL_TEXTURE_2D);
        draw_rect(RESUME_X, btn_y, btn_w, RESUME_H,
                  0.3f, 0.3f, 0.3f, 0.6f);
        font_draw_string("RECENT", RESUME_X + 8, btn_y + 12, 2,
                         1.0f, 0.5f, 0.15f, 1.0f);
        if (g_keyboard_detected) {
            int kw = font_string_width("2", 2);
            font_draw_string("2", RESUME_X + (btn_w - kw) / 2, btn_y + 1, 2,
                             1.0f, 1.0f, 1.0f, 0.9f);
        }
    }

    /* Separator line (below gear icon, or below RECENT button when visible) */
    {
        int gear_bottom = TITLE_Y + 5 * 8 + 2 + GEAR_IMG_H;  /* gear icon bottom */
        int sep_y = gear_bottom + 2;
        if (g_recent_count > 1) {
            int recent_bottom = RESUME_Y + RESUME_H + 4 + RESUME_H;
            if (recent_bottom + 6 > sep_y) sep_y = recent_bottom + 6;
        }
        glDisable(GL_TEXTURE_2D);
        draw_rect(MARGIN_X, sep_y, SCREEN_WIDTH - 2 * MARGIN_X, 2,
                  0.3f, 0.3f, 0.4f, 1.0f);
    }

    if (g_file_count == 0) {
        /* No files message */
        int nw = font_string_width("Directory is empty", 2);
        font_draw_string("Directory is empty",
                         (SCREEN_WIDTH - nw) / 2.0f, SCREEN_HEIGHT / 2 - 8, 2,
                         0.8f, 0.4f, 0.4f, 1.0f);
        draw_recent_popup();
        draw_settings_popup();
        draw_fp_aswarn_popup();
        draw_about_popup();
        draw_save_popup();
        draw_delete_confirm_popup();
        draw_dirask_popup();
        draw_dirpicker_popup();
        draw_update_popup();
        SDL_GL_SwapBuffers();
        return;
    }

    /* Pinned ".." entry at top, scrollable items below */
    has_dotdot = (g_file_count > 0 && strcmp(g_files[0].name, "..") == 0);
    scroll_start = has_dotdot ? 1 : 0;
    scroll_area_top = has_dotdot ? (LIST_TOP + ITEM_HEIGHT) : LIST_TOP;
    scroll_count = g_file_count - scroll_start;

    /* Draw pinned ".." entry (grey, fixed position) */
    if (has_dotdot) {
        int dd_y = LIST_TOP + (ITEM_HEIGHT - 16) / 2;
        font_draw_string("../", MARGIN_X, dd_y, 2,
                         0.6f, 0.6f, 0.6f, 1.0f);
    }

    /* Clamp scroll (only scrollable items) */
    visible_height = LIST_BOTTOM - scroll_area_top;
    total_height = scroll_count * ITEM_HEIGHT;
    max_scroll = total_height - visible_height;
    if (max_scroll < 0) max_scroll = 0;
    if (g_scroll_offset < 0) g_scroll_offset = 0;
    if (g_scroll_offset > max_scroll) g_scroll_offset = max_scroll;

    /* Draw scrollable file list */
    for (i = scroll_start; i < g_file_count; i++) {
        float iy = scroll_area_top + (i - scroll_start) * ITEM_HEIGHT - g_scroll_offset;

        /* Skip items outside visible area (don't overlap pinned "..") */
        if (iy < scroll_area_top || iy > LIST_BOTTOM) continue;

        item_y = (int)(iy + (ITEM_HEIGHT - 16) / 2);

        {
            char disp[MAX_NAME_LEN + 4];
            int max_w = SCROLLBAR_X - 8 - MARGIN_X;
            int tw;

            if (g_files[i].is_dir) {
                snprintf(disp, sizeof(disp), "%s/", g_files[i].name);
            } else {
                strncpy(disp, g_files[i].name, MAX_NAME_LEN + 3);
                disp[MAX_NAME_LEN + 3] = '\0';
            }

            tw = font_string_width(disp, 2);
            if (tw > max_w) {
                int dots_w = font_string_width("...", 2);
                int max_chars = (max_w - dots_w) / (8 * 2);
                if (max_chars < 0) max_chars = 0;
                disp[max_chars] = '\0';
                strcat(disp, "...");
            }

            if (g_files[i].is_dir) {
                font_draw_string(disp, MARGIN_X, item_y, 2,
                                 1.0f, 0.5f, 0.15f, 1.0f);
            } else {
                const char *ext = strrchr(g_files[i].name, '.');
                if (ext && strcasecmp(ext, ".sav") == 0) {
                    font_draw_string(disp, MARGIN_X, item_y, 2,
                                     0.7f, 0.3f, 1.0f, 1.0f);
                } else {
                    font_draw_string(disp, MARGIN_X, item_y, 2,
                                     1.0f, 1.0f, 1.0f, 1.0f);
                }
            }
        }
    }

    /* Asteroids launch image (bottom-right) */
    if (g_ast_texture) {
        GLfloat ast_verts[] = {
            AST_BTN_X,             AST_BTN_Y,
            AST_BTN_X + AST_BTN_W, AST_BTN_Y,
            AST_BTN_X,             AST_BTN_Y + AST_BTN_H,
            AST_BTN_X + AST_BTN_W, AST_BTN_Y + AST_BTN_H
        };
        GLfloat ast_texcoords[] = {
            0.0f, 0.0f,
            1.0f, 0.0f,
            0.0f, 1.0f,
            1.0f, 1.0f
        };
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, g_ast_texture);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glVertexPointer(2, GL_FLOAT, 0, ast_verts);
        glTexCoordPointer(2, GL_FLOAT, 0, ast_texcoords);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glDisableClientState(GL_VERTEX_ARRAY);
        glDisable(GL_TEXTURE_2D);
    }

    draw_recent_popup();
    draw_settings_popup();
    draw_fp_aswarn_popup();
    draw_about_popup();
    draw_save_popup();
    draw_delete_confirm_popup();
    draw_dirask_popup();
    draw_dirpicker_popup();
    draw_update_popup();

    glDisable(GL_BLEND);

    SDL_GL_SwapBuffers();
}

/* Touch down */
void filepicker_touch_down(int x, int y)
{
    g_keyboard_detected = 0;
    g_touch_active = 1;
    g_touch_start_x = x;
    g_touch_start_y = y;
    g_touch_last_y = y;
    g_touch_moved = 0;
    g_dirpicker_touch_moved = 0;
    g_dirpicker_touch_active = 0;
    g_scroll_at_touch_start = g_scroll_offset;
}

/* Touch move */
void filepicker_touch_move(int x, int y)
{
    int dy;
    (void)x;

    if (!g_touch_active) return;

    dy = abs(y - g_touch_start_y);
    if (dy > TAP_THRESHOLD) {
        g_touch_moved = 1;
    }

    /* Directory picker scroll handling */
    if (g_dirpicker_popup_visible) {
        if (g_touch_moved) {
            if (!g_dirpicker_touch_active) {
                g_dirpicker_touch_active = 1;
                g_dirpicker_touch_start_y = g_touch_start_y;
                g_dirpicker_scroll_at_start = g_dirpicker_scroll;
                g_dirpicker_touch_moved = 0;
            }
            g_dirpicker_touch_moved = 1;
            g_dirpicker_scroll = g_dirpicker_scroll_at_start - (y - g_dirpicker_touch_start_y);
        }
        return;
    }

    /* Don't scroll file list when popup is visible */
    if (g_dirask_popup_visible || g_recent_popup_visible || g_about_popup_visible || g_save_popup_visible || g_delete_confirm_visible || g_settings_popup_visible || g_fp_aswarn_visible || g_update_popup_visible) return;

    /* Scroll */
    if (g_touch_moved) {
        g_scroll_offset = g_scroll_at_touch_start - (y - g_touch_start_y);
    }

    g_touch_last_y = y;
}

/* Touch up - returns 1 if a ROM file was selected */
int filepicker_touch_up(int x, int y)
{
    int idx;
    float iy;

    if (!g_touch_active) return 0;
    g_touch_active = 0;

    /* If finger moved too much, it was a scroll, not a tap */
    if (g_touch_moved) {
        g_dirpicker_touch_active = 0;
        return 0;
    }

    /* Handle update popup touches */
    if (g_update_popup_visible) {
        char ul[UPDATE_MAX_LINES][64];
        int uc = 0;
        int popup_h;
        int popup_x, popup_y;
        {
            const char *note = updater_get_note();
            if (note && note[0])
                uc = wrap_note_lines(note, UPDATE_POPUP_W - 2 * POPUP_PAD, ul, UPDATE_MAX_LINES);
        }
        popup_h = update_popup_height(uc);
        popup_x = (SCREEN_WIDTH - UPDATE_POPUP_W) / 2;
        popup_y = (SCREEN_HEIGHT - popup_h) / 2;

        if (x >= popup_x && x < popup_x + UPDATE_POPUP_W &&
            y >= popup_y && y < popup_y + popup_h) {
            /* Check button taps */
            int w1 = font_string_width("UPDATE", 2) + 16;
            int w2 = font_string_width("LATER", 2) + 16;
            int btn_w = w1 > w2 ? w1 : w2;
            int gap = 16;
            int total_w = btn_w * 2 + gap;
            int bx = popup_x + (UPDATE_POPUP_W - total_w) / 2;
            int by = popup_y + popup_h - POPUP_PAD - RESUME_H;

            if (y >= by && y < by + RESUME_H) {
                /* UPDATE button */
                if (x >= bx && x < bx + btn_w) {
                    updater_install();
                    g_update_popup_visible = 0;
                    return 0;
                }
                bx += btn_w + gap;
                /* LATER button */
                if (x >= bx && x < bx + btn_w) {
                    g_update_later = 1;
                    g_update_popup_visible = 0;
                    return 0;
                }
            }
        } else {
            /* Tap outside popup — same as LATER */
            g_update_later = 1;
            g_update_popup_visible = 0;
        }
        return 0;
    }

    /* Handle directory picker popup touches (highest priority — has scrolling list) */
    if (g_dirpicker_popup_visible) {
        int popup_x = (SCREEN_WIDTH - DIRPICKER_W) / 2;
        int popup_y = (SCREEN_HEIGHT - DIRPICKER_H) / 2;

        if (x >= popup_x && x < popup_x + DIRPICKER_W &&
            y >= popup_y && y < popup_y + DIRPICKER_H) {

            /* List area */
            {
                int text_y = popup_y + POPUP_PAD + DIRPICKER_TITLE_H + 8;
                int list_bottom = text_y + DIRPICKER_LIST_H;

                if (y >= text_y && y < list_bottom && !g_dirpicker_touch_moved) {
                    float tap_offset = (y - text_y) + g_dirpicker_scroll;
                    int item_idx = (int)(tap_offset / DIRPICKER_ITEM_H);
                    if (item_idx >= 0 && item_idx < g_dirpicker_count) {
                        /* Navigate into selected directory */
                        dirpicker_scan(g_dirpicker_dirs[item_idx].path);
                        g_dirpicker_touch_active = 0;
                        return 0;
                    }
                }
            }

            /* Check Set button (centered) */
            {
                int set_w = font_string_width("Set", 2) + 16;
                int bx = popup_x + (DIRPICKER_W - set_w) / 2;
                int by = popup_y + DIRPICKER_H - POPUP_PAD - RESUME_H;

                if (y >= by && y < by + RESUME_H &&
                    x >= bx && x < bx + set_w) {
                    strncpy(g_default_romdir, g_dirpicker_current, MAX_PATH_LEN - 1);
                    g_default_romdir[MAX_PATH_LEN - 1] = '\0';
                    save_settings();
                    g_dirpicker_popup_visible = 0;
                    filepicker_scan(g_default_romdir);
                    g_dirpicker_touch_active = 0;
                    return 0;
                }
            }
        } else {
            /* Tap outside popup — close it */
            g_dirpicker_popup_visible = 0;
        }
        g_dirpicker_touch_active = 0;
        return 0;
    }

    /* Handle directory ask popup touches */
    if (g_dirask_popup_visible) {
        int popup_x = (SCREEN_WIDTH - DIRASK_W) / 2;
        int popup_y = (SCREEN_HEIGHT - DIRASK_H) / 2;

        if (x >= popup_x && x < popup_x + DIRASK_W &&
            y >= popup_y && y < popup_y + DIRASK_H) {
            /* Check buttons (uniform width) */
            int w1 = font_string_width("Yes", 2) + 16;
            int w2 = font_string_width("Later", 2) + 16;
            int btn_w = w1 > w2 ? w1 : w2;
            int gap = 16;
            int total_w, bx, by;
            total_w = btn_w * 2 + gap;
            bx = popup_x + (DIRASK_W - total_w) / 2;
            by = popup_y + DIRASK_H - POPUP_PAD - RESUME_H;

            if (y >= by && y < by + RESUME_H) {
                /* Yes — open directory picker */
                if (x >= bx && x < bx + btn_w) {
                    g_dirask_popup_visible = 0;
                    dirpicker_scan(g_current_dir);
                    g_dirpicker_popup_visible = 1;
                    return 0;
                }
                bx += btn_w + gap;
                /* Later — suppress future prompts */
                if (x >= bx && x < bx + btn_w) {
                    g_romdir_prompt_never = 1;
                    save_settings();
                    g_dirask_popup_visible = 0;
                    return 0;
                }
            }
        } else {
            /* Tap outside popup — same as Later */
            g_romdir_prompt_never = 1;
            save_settings();
            g_dirask_popup_visible = 0;
        }
        return 0;
    }

    /* Handle recent popup touches (highest priority) */
    if (g_recent_popup_visible) {
        int popup_h, popup_x, popup_y;
        int recent_title_h = 28;  /* scale 3 title height */
        popup_h = POPUP_PAD + recent_title_h + POPUP_GAP
                + g_recent_count * POPUP_ITEM_H
                + POPUP_GAP + POPUP_BTN_H + POPUP_PAD;
        popup_x = (SCREEN_WIDTH - POPUP_W) / 2;
        popup_y = (SCREEN_HEIGHT - popup_h) / 2;

        if (x >= popup_x && x < popup_x + POPUP_W &&
            y >= popup_y && y < popup_y + popup_h) {
            /* Check "Clear List" button (centered) */
            {
                int clr_w = font_string_width("Clear List", 2) + 16;
                int clr_x = popup_x + (POPUP_W - clr_w) / 2;
                int clr_y = popup_y + popup_h - POPUP_PAD - POPUP_BTN_H;
                if (x >= clr_x && x < clr_x + clr_w &&
                    y >= clr_y && y < clr_y + POPUP_BTN_H) {
                    g_recent_count = 0;
                    save_recent_list();
                    g_recent_popup_visible = 0;
                    return 0;
                }
            }
            /* Check item taps */
            {
                int items_y = popup_y + POPUP_PAD + recent_title_h + POPUP_GAP;
                if (y >= items_y && y < items_y + g_recent_count * POPUP_ITEM_H) {
                    int item_idx = (y - items_y) / POPUP_ITEM_H;
                    if (item_idx >= 0 && item_idx < g_recent_count) {
                        strncpy(g_last_rom_path, g_recent_paths[item_idx], MAX_PATH_LEN - 1);
                        g_last_rom_path[MAX_PATH_LEN - 1] = '\0';
                        g_last_rom_type = g_recent_types[item_idx];
                        g_has_last_rom = 1;
                        g_resume_selected = 1;
                        g_art_selected = 0;
                        g_recent_popup_visible = 0;
                        return check_save_popup();
                    }
                }
            }
        } else {
            /* Tap outside popup - close it */
            g_recent_popup_visible = 0;
        }
        return 0;
    }

    /* Handle auto-save warning popup touches (filepicker — higher priority than settings) */
    if (g_fp_aswarn_visible) {
        int popup_x = (SCREEN_WIDTH - FP_ASWARN_W) / 2;
        int popup_y = (SCREEN_HEIGHT - FP_ASWARN_H) / 2;
        int btn_cy = popup_y + FP_ASWARN_H - FP_ASWARN_BTN_H - 16;
        int yes_x = popup_x + FP_ASWARN_W / 2 - FP_ASWARN_BTN_W - 20;
        int no_x  = popup_x + FP_ASWARN_W / 2 + 20;

        if (x >= yes_x && x < yes_x + FP_ASWARN_BTN_W &&
            y >= btn_cy && y < btn_cy + FP_ASWARN_BTN_H) {
            /* Yes — apply pending action */
            if (g_fp_aswarn_action == 0) {
                input_set_autosave(1);
            } else {
                input_set_autosave_ask(0);
            }
            save_settings();
            g_fp_aswarn_visible = 0;
        } else if (x >= no_x && x < no_x + FP_ASWARN_BTN_W &&
                   y >= btn_cy && y < btn_cy + FP_ASWARN_BTN_H) {
            /* No — don't change */
            g_fp_aswarn_visible = 0;
        }
        return 0;
    }

    /* Handle settings popup touches */
    if (g_settings_popup_visible) {
        int popup_x = (SCREEN_WIDTH - SETTINGS_POPUP_W) / 2;
        int popup_y = (SCREEN_HEIGHT - SETTINGS_POPUP_H) / 2;

        if (x >= popup_x && x < popup_x + SETTINGS_POPUP_W &&
            y >= popup_y && y < popup_y + SETTINGS_POPUP_H) {

            int text_y = popup_y + POPUP_PAD;
            int row_y = text_y + SETTINGS_TITLE_H + POPUP_PAD;
            int row_idx = (y - row_y) / SETTINGS_ROW_H;

            if (row_idx >= 0 && row_idx < SETTINGS_ROWS) {
                int cur_row_y = row_y + row_idx * SETTINGS_ROW_H;
                int btn_w, btn_x, btn_y_c;

                /* Uniform button width (same as draw code) */
                {
                    int w1 = font_string_width("MEDIUM", 2);
                    int w2 = font_string_width("EMAIL", 2);
                    int w3 = font_string_width("BRIGHT", 2);
                    btn_w = w1 > w2 ? w1 : w2;
                    if (w3 > btn_w) btn_w = w3;
                    btn_w += 16;
                }

                btn_y_c = cur_row_y + (SETTINGS_ROW_H - RESUME_H) / 2;
                btn_x = popup_x + SETTINGS_POPUP_W - POPUP_PAD - btn_w;

                switch (row_idx) {
                    case 0: /* Scanlines toggle */
                        if (x >= btn_x && x < btn_x + btn_w &&
                            y >= btn_y_c && y < btn_y_c + RESUME_H) {
                            video_set_scanlines(!video_get_scanlines());
                            save_settings();
                        }
                        break;
                    case 1: /* Scanline Brightness cycle */
                        if (x >= btn_x && x < btn_x + btn_w &&
                            y >= btn_y_c && y < btn_y_c + RESUME_H) {
                            video_set_scanline_brightness((video_get_scanline_brightness() + 1) % 3);
                            save_settings();
                        }
                        break;
                    case 2: /* Palette cycle */
                        if (x >= btn_x && x < btn_x + btn_w &&
                            y >= btn_y_c && y < btn_y_c + RESUME_H) {
                            video_set_maria_palette((video_get_maria_palette() + 1) % MARIA_PALETTE_COUNT);
                            save_settings();
                        }
                        break;
                    case 3: /* Control Brightness cycle */
                        if (x >= btn_x && x < btn_x + btn_w &&
                            y >= btn_y_c && y < btn_y_c + RESUME_H) {
                            input_set_control_dim((input_get_control_dim() + 1) % 3);
                            save_settings();
                        }
                        break;
                    case 4: /* Auto-Save toggle */
                        if (x >= btn_x && x < btn_x + btn_w &&
                            y >= btn_y_c && y < btn_y_c + RESUME_H) {
                            if (!input_get_autosave() && !input_get_autosave_ask()) {
                                /* Turning ON with Ask OFF — show warning */
                                g_fp_aswarn_action = 0;
                                g_fp_aswarn_visible = 1;
                            } else {
                                input_set_autosave(!input_get_autosave());
                                save_settings();
                            }
                        }
                        break;
                    case 5: /* Ask Before Saving toggle (only if autosave ON) */
                        if (x >= btn_x && x < btn_x + btn_w &&
                            y >= btn_y_c && y < btn_y_c + RESUME_H) {
                            if (input_get_autosave()) {
                                if (input_get_autosave_ask()) {
                                    /* Turning OFF with autosave ON — show warning */
                                    g_fp_aswarn_action = 1;
                                    g_fp_aswarn_visible = 1;
                                } else {
                                    input_set_autosave_ask(1);
                                    save_settings();
                                }
                            }
                        }
                        break;
                    case 6: /* Change ROM Directory — SET */
                        if (x >= btn_x && x < btn_x + btn_w &&
                            y >= btn_y_c && y < btn_y_c + RESUME_H) {
                            g_settings_popup_visible = 0;
                            dirpicker_scan(g_current_dir[0] ? g_current_dir : "/media/internal/");
                            g_dirpicker_popup_visible = 1;
                        }
                        break;
                    case 7: /* Bug Report — EMAIL */
                        if (x >= btn_x && x < btn_x + btn_w &&
                            y >= btn_y_c && y < btn_y_c + RESUME_H) {
                            g_settings_popup_visible = 0;
                            {
                                typedef int (*ServiceCallFunc)(const char *, const char *);
                                ServiceCallFunc fn = (ServiceCallFunc)dlsym(RTLD_DEFAULT, "PDL_ServiceCall");
                                if (fn) fn("palm://com.palm.applicationManager/open",
                                           "{\"target\":\"mailto:alanmorford@gmail.com?subject=EMU7800%20bug%20report\"}");
                            }
                        }
                        break;
                    case 8: /* About */
                        if (x >= btn_x && x < btn_x + btn_w &&
                            y >= btn_y_c && y < btn_y_c + RESUME_H) {
                            g_settings_popup_visible = 0;
                            g_about_popup_visible = 1;
                        }
                        break;
                }
            }
        } else {
            /* Tap outside popup — close it */
            g_settings_popup_visible = 0;
        }
        return 0;
    }

    /* Handle about popup touches — tap anywhere to close */
    if (g_about_popup_visible) {
        g_about_popup_visible = 0;
        return 0;
    }

    /* Handle delete confirmation popup touches */
    if (g_delete_confirm_visible) {
        int popup_x = (SCREEN_WIDTH - SAVE_POPUP_W) / 2;
        int popup_y = (SCREEN_HEIGHT - SAVE_POPUP_H) / 2;

        if (x >= popup_x && x < popup_x + SAVE_POPUP_W &&
            y >= popup_y && y < popup_y + SAVE_POPUP_H) {
            int w1 = font_string_width("Yes", 2) + 16;
            int w2 = font_string_width("No", 2) + 16;
            int btn_w = w1 > w2 ? w1 : w2;
            int gap = 16;
            int total_w = btn_w * 2 + gap;
            int bx = popup_x + (SAVE_POPUP_W - total_w) / 2;
            int by = popup_y + POPUP_PAD + 40 + 16;

            if (y >= by && y < by + RESUME_H) {
                /* Yes - delete save and launch ROM */
                if (x >= bx && x < bx + btn_w) {
                    char sav_path[MAX_PATH_LEN];
                    build_sav_path(g_save_popup_path, sav_path, sizeof(sav_path));
                    unlink(sav_path);
                    g_delete_confirm_visible = 0;
                    return resolve_save_popup(0);
                }
                bx += btn_w + gap;
                /* No - return to save popup */
                if (x >= bx && x < bx + btn_w) {
                    g_delete_confirm_visible = 0;
                    g_save_popup_visible = 1;
                    return 0;
                }
            }
        } else {
            /* Tap outside - return to save popup */
            g_delete_confirm_visible = 0;
            g_save_popup_visible = 1;
        }
        return 0;
    }

    /* Handle save popup touches */
    if (g_save_popup_visible) {
        int popup_x = (SCREEN_WIDTH - SAVE_POPUP_W) / 2;
        int popup_y = (SCREEN_HEIGHT - SAVE_POPUP_H) / 2;

        if (x >= popup_x && x < popup_x + SAVE_POPUP_W &&
            y >= popup_y && y < popup_y + SAVE_POPUP_H) {
            /* Check button taps (uniform width) */
            int w1 = font_string_width("Yes", 2) + 16;
            int w2 = font_string_width("No", 2) + 16;
            int w3 = font_string_width("Delete", 2) + 16;
            int btn_w = w1 > w2 ? w1 : w2;
            int gap = 16;
            int total_w, bx, by;
            if (w3 > btn_w) btn_w = w3;
            total_w = btn_w * 3 + gap * 2;
            bx = popup_x + (SAVE_POPUP_W - total_w) / 2;
            by = popup_y + POPUP_PAD + 40 + 16;

            if (y >= by && y < by + RESUME_H) {
                /* Yes button */
                if (x >= bx && x < bx + btn_w) {
                    return resolve_save_popup(1);
                }
                bx += btn_w + gap;
                /* No button */
                if (x >= bx && x < bx + btn_w) {
                    return resolve_save_popup(0);
                }
                bx += btn_w + gap;
                /* Delete button - show confirmation popup */
                if (x >= bx && x < bx + btn_w) {
                    g_save_popup_visible = 0;
                    g_delete_confirm_visible = 1;
                    return 0;
                }
            }
        } else {
            /* Tap outside popup - close without launching */
            g_save_popup_visible = 0;
        }
        return 0;
    }

    /* Check Asteroids button tap */
    if (x >= AST_BTN_X && x < AST_BTN_X + AST_BTN_W &&
        y >= AST_BTN_Y && y < AST_BTN_Y + AST_BTN_H) {
        g_art_selected = 1;
        g_resume_selected = 0;
        return check_save_popup();
    }

    /* Check Resume button tap */
    if (g_has_last_rom &&
        x >= RESUME_X && x < RESUME_X + font_string_width("RESUME", 2) + 16 &&
        y >= RESUME_Y && y < RESUME_Y + RESUME_H) {
        g_resume_selected = 1;
        g_art_selected = 0;
        return check_save_popup();
    }

    /* Check RECENT button tap */
    if (g_recent_count > 1) {
        int btn_w = font_string_width("RECENT", 2) + 16;
        int btn_y = RESUME_Y + RESUME_H + 4;
        if (x >= RESUME_X && x < RESUME_X + btn_w &&
            y >= btn_y && y < btn_y + RESUME_H) {
            g_recent_popup_visible = 1;
            return 0;
        }
    }

    /* Check gear icon tap (opens Settings popup) */
    {
        int gear_x = SCREEN_WIDTH - MARGIN_X - GEAR_IMG_W;
        int gear_y = TITLE_Y + 5 * 8 + 2;
        if (x >= gear_x && x < gear_x + GEAR_IMG_W &&
            y >= gear_y && y < gear_y + GEAR_IMG_H) {
            g_settings_popup_visible = 1;
            return 0;
        }
    }

    /* Check "Update!" text tap (to the left of gear, only after LATER) */
    if (updater_has_update() && g_update_later) {
        int gear_x = SCREEN_WIDTH - MARGIN_X - GEAR_IMG_W;
        int gear_y = TITLE_Y + 5 * 8 + 2;
        int update_w = font_string_width("Update!", 2);
        int update_h = 2 * 8;
        int update_x = gear_x - 4 - update_w;
        int update_y = gear_y + (GEAR_IMG_H - 16) / 2;
        if (x >= update_x && x < update_x + update_w &&
            y >= update_y && y < update_y + update_h) {
            g_update_popup_visible = 1;
            return 0;
        }
    }

    /* Determine which item was tapped */
    {
        int has_dd = (g_file_count > 0 && strcmp(g_files[0].name, "..") == 0);
        int sarea_top = has_dd ? (LIST_TOP + ITEM_HEIGHT) : LIST_TOP;

        /* Check tap on pinned ".." entry */
        if (has_dd && y >= LIST_TOP && y < LIST_TOP + ITEM_HEIGHT) {
            filepicker_scan(g_files[0].path);
            return 0;
        }

        /* Check tap is in the scrollable list area */
        if (y < sarea_top || y > LIST_BOTTOM) return 0;

        iy = y - sarea_top + g_scroll_offset;
        if (iy < 0) return 0;
        idx = (int)(iy / ITEM_HEIGHT) + (has_dd ? 1 : 0);
        if (idx < 0 || idx >= g_file_count) return 0;
    }

    /* Directory: navigate into it */
    if (g_files[idx].is_dir) {
        filepicker_scan(g_files[idx].path);
        return 0;
    }

    /* Only allow selecting ROM files */
    if (g_files[idx].type < 0) return 0;

    g_selected_index = idx;
    g_resume_selected = 0;
    g_art_selected = 0;
    return check_save_popup();
}

/* Get selected path */
const char *filepicker_get_selected_path(void)
{
    if (g_art_selected) return BUNDLED_ASTEROIDS_ROM;
    if (g_resume_selected && g_has_last_rom) return g_last_rom_path;
    if (g_selected_index < 0 || g_selected_index >= g_file_count) return NULL;
    return g_files[g_selected_index].path;
}

/* Get selected machine type */
int filepicker_get_selected_type(void)
{
    if (g_art_selected) return MACHINE_7800;
    if (g_resume_selected && g_has_last_rom) return g_last_rom_type;
    if (g_selected_index < 0 || g_selected_index >= g_file_count) return MACHINE_2600;
    return g_files[g_selected_index].type;
}

/* Store last played ROM for resume (also persists to disk) */
void filepicker_set_last_rom(const char *path, int type)
{
    strncpy(g_last_rom_path, path, MAX_PATH_LEN - 1);
    g_last_rom_path[MAX_PATH_LEN - 1] = '\0';
    g_last_rom_type = type;
    g_has_last_rom = 1;
    save_last_rom();
    add_to_recent(path, type);
}

/* Check if a last ROM is available */
int filepicker_has_last_rom(void)
{
    return g_has_last_rom;
}

/* Check if save state should be loaded (consumes the flag) */
int filepicker_should_load_save(void)
{
    int result = g_save_popup_load_save;
    g_save_popup_load_save = 0;
    return result;
}

/* Get the current directory being browsed */
const char *filepicker_get_current_dir(void)
{
    return g_current_dir;
}

/* Get the saved default ROM directory (NULL if not set or dir doesn't exist) */
const char *filepicker_get_default_romdir(void)
{
    struct stat st;
    if (g_default_romdir[0] == '\0') return NULL;
    if (stat(g_default_romdir, &st) != 0 || !S_ISDIR(st.st_mode)) return NULL;
    return g_default_romdir;
}

/* Handle keyboard input in filepicker state.
 * Returns 1 if a ROM was selected (caller should launch). */
int filepicker_key_down(int sym)
{
    if (!g_keyboard_detected) {
        g_keyboard_detected = 1;
        save_settings();
    }

    /* Save popup: Y/N/D */
    if (g_save_popup_visible) {
        if (sym == SDLK_y) {
            return resolve_save_popup(1);
        } else if (sym == SDLK_n) {
            return resolve_save_popup(0);
        } else if (sym == SDLK_d) {
            g_save_popup_visible = 0;
            g_delete_confirm_visible = 1;
        }
        return 0;
    }

    /* Recent popup: 1-9 selects item */
    if (g_recent_popup_visible) {
        if (sym >= SDLK_1 && sym <= SDLK_9) {
            int idx = sym - SDLK_1;
            if (idx < g_recent_count) {
                strncpy(g_last_rom_path, g_recent_paths[idx], MAX_PATH_LEN - 1);
                g_last_rom_path[MAX_PATH_LEN - 1] = '\0';
                g_last_rom_type = g_recent_types[idx];
                g_has_last_rom = 1;
                g_resume_selected = 1;
                g_art_selected = 0;
                g_recent_popup_visible = 0;
                return check_save_popup();
            }
        }
        return 0;
    }

    /* Any other popup visible — ignore keys */
    if (g_dirask_popup_visible || g_dirpicker_popup_visible ||
        g_settings_popup_visible || g_about_popup_visible ||
        g_delete_confirm_visible || g_fp_aswarn_visible ||
        g_update_popup_visible) {
        return 0;
    }

    /* Main filepicker screen: 1=Resume, 2=Recent */
    if (sym == SDLK_1 && g_has_last_rom) {
        g_resume_selected = 1;
        g_art_selected = 0;
        return check_save_popup();
    }
    if (sym == SDLK_2 && g_recent_count > 1) {
        g_recent_popup_visible = 1;
        return 0;
    }

    return 0;
}

/* Returns 1 if a keyboard was detected (persisted across sessions) */
int filepicker_keyboard_detected(void)
{
    return g_keyboard_detected;
}
