/*
 * controller.c
 *
 * Wired USB game controller support (direct evdev reads).
 *
 * webOS 3.x's SDL has no joydev -- SDL_INIT_JOYSTICK finds nothing, pads
 * only ever appear as /dev/input/eventN. This mirrors the technique used by
 * webOSArchive/webos-sdlquake-hd (see plugin/packaging/postinst for the jail
 * side of it), pared down to what EMU7800 needs. No per-model button tables
 * -- just the generic BTN_GAMEPAD/BTN_JOYSTICK blocks plus a hat or a
 * centered stick axis for direction.
 *
 * Button map (matches the Android port's CONTROLS MAP popup exactly):
 *   B / Y (EAST/WEST)  -> Fire 1        A / X (SOUTH/NORTH) -> Fire 2
 *   D-Pad (hat/stick)  -> Joystick      L Stick (analog)    -> Paddle
 *   LT (axis or TL2)   -> Select sw     RT (axis or TR2)    -> Reset sw
 *   Start              -> Pause         Select (Back)       -> Back
 *   LB (TL)            -> Save State    RB (TR)             -> Load State
 *
 * Copyright (c) 2026 EMU7800
 */

#include "controller.h"

#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <sys/ioctl.h>

extern void log_msg(const char *msg);

#define MAX_NODES        16   /* /dev/input/event0 .. event15 */
#define SCAN_INTERVAL     60  /* poll calls between hotplug scans (~1s @ 60fps) */
#define NAV_REPEAT_DELAY  24  /* frames held before nav auto-repeat kicks in */
#define NAV_REPEAT_EVERY   8  /* frames between repeats once it kicks in */

/* ---- Single active pad ------------------------------------------------- */
static int  g_fd = -1;
static int  g_node_idx = -1;
static char g_pad_name[80];

static unsigned char g_node_boring[MAX_NODES];
static int g_scan_countdown = 0;

/* Centered-axis ("stick as d-pad") detection, probed once at open time */
static int g_have_hat;
static int g_have_stick_x, g_have_stick_y;
static int g_stick_x_lo, g_stick_x_hi;
static int g_stick_y_lo, g_stick_y_hi;

/* Full range of ABS_X, for analog paddle use regardless of hat presence */
static int g_stick_x_min, g_stick_x_max, g_stick_x_raw;

/* Analog trigger axes (ABS_Z/ABS_RZ -- LT/RT), probed once at open time.
 * "far" is whichever end is NOT the resting value, so this works whether a
 * trigger rests at its minimum or (like a stick) at its center. Different
 * pads report triggers on different axis codes -- ABS_Z/ABS_RZ is one
 * convention, ABS_BRAKE/ABS_GAS (confirmed on a USB "Atari Game Controller"
 * via the unmapped-axis diagnostic) is another -- so g_trig_l_code/r_code
 * record which one this pad actually uses. */
static int g_have_trig_l, g_have_trig_r;
static int g_trig_l_code, g_trig_r_code;
static int g_trig_l_rest, g_trig_l_far;
static int g_trig_r_rest, g_trig_r_far;

/* Current decoded level state */
static int g_up, g_down, g_left, g_right;
static int g_fire, g_fire2, g_start, g_back;
static int g_save, g_load;             /* LB/RB -- edge-driven, one-shot */
static int g_select_sw, g_reset_sw;     /* LT/RT -- level-driven console switches */

/* Diagnostic: log any button code we don't recognize, once per unique code
 * per connection, so a mismapped pad can be fixed from the log instead of
 * guessed at. Presses only (not releases). */
#define MAX_LOGGED_CODES 16
static int g_logged_codes[MAX_LOGGED_CODES];
static int g_logged_count;

/* Previous-frame levels, for edge detection */
static int g_prev_start, g_prev_back, g_prev_fire, g_prev_fire2;
static int g_prev_save, g_prev_load;
static int g_up_hold, g_down_hold;

/* This frame's edges (recomputed each controller_poll()) */
static int g_e_start, g_e_back, g_e_nav_up, g_e_nav_down, g_e_a, g_e_b;
static int g_e_save, g_e_load;

static void reset_pad_state(void)
{
    g_up = g_down = g_left = g_right = 0;
    g_fire = g_fire2 = g_start = g_back = 0;
    g_save = g_load = g_select_sw = g_reset_sw = 0;
    g_prev_start = g_prev_back = g_prev_fire = g_prev_fire2 = 0;
    g_prev_save = g_prev_load = 0;
    g_up_hold = g_down_hold = 0;
    g_have_hat = g_have_stick_x = g_have_stick_y = 0;
    g_stick_x_min = g_stick_x_max = g_stick_x_raw = 0;
    g_have_trig_l = g_have_trig_r = 0;
    g_trig_l_code = g_trig_r_code = -1;
    g_logged_count = 0;
}

/* Log an unrecognized button code once (see g_logged_codes above). */
static void log_unmapped_button(int code, int value)
{
    int i;
    if (!value) return;
    for (i = 0; i < g_logged_count; i++) {
        if (g_logged_codes[i] == code) return;
    }
    if (g_logged_count < MAX_LOGGED_CODES) {
        char msg[80];
        g_logged_codes[g_logged_count++] = code;
        snprintf(msg, sizeof(msg), "controller: unmapped button code 0x%03x", code);
        log_msg(msg);
    }
}

/* Same, but for an EV_ABS axis we don't otherwise handle -- offset into a
 * disjoint range of the shared dedup array so it can never collide with a
 * button code, and includes the current value (useful to see whether it's
 * actually moving when the button is pressed). Logs on every new code seen,
 * not just nonzero, since a resting axis reads a nonzero value too. */
static void log_unmapped_axis(int code, int value)
{
    int key = 0x1000 + code;
    int i;
    for (i = 0; i < g_logged_count; i++) {
        if (g_logged_codes[i] == key) return;
    }
    if (g_logged_count < MAX_LOGGED_CODES) {
        char msg[80];
        g_logged_codes[g_logged_count++] = key;
        snprintf(msg, sizeof(msg), "controller: unmapped axis code 0x%02x value=%d", code, value);
        log_msg(msg);
    }
}

static void close_pad(int log_it)
{
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
        if (log_it) {
            char msg[128];
            snprintf(msg, sizeof(msg), "controller: disconnected (%s)", g_pad_name);
            log_msg(msg);
        }
    }
    g_node_idx = -1;
    reset_pad_state();
}

void controller_init(void)
{
    memset(g_node_boring, 0, sizeof(g_node_boring));
    g_scan_countdown = 0;
    g_fd = -1;
    g_node_idx = -1;
    reset_pad_state();
}

void controller_shutdown(void)
{
    close_pad(0);
}

int controller_connected(void)
{
    return g_fd >= 0;
}

/* ---- Capability probing ------------------------------------------------ */

static int has_key_cap(int fd, int code)
{
    unsigned long bits[(KEY_MAX / (8 * sizeof(long))) + 1];
    memset(bits, 0, sizeof(bits));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0) return 0;
    return (bits[code / (8 * sizeof(long))] >> (code % (8 * sizeof(long)))) & 1UL;
}

/* Probe ABS_HAT0X/Y and ABS_X/Y so we know, once, whether this pad's d-pad
 * lives on a hat or on a stick axis that rests centered (as opposed to an
 * analog trigger, which rests pegged at one end and must not be read as
 * direction). */
static void probe_axes(int fd)
{
    struct input_absinfo ai;

    g_have_hat = (ioctl(fd, EVIOCGABS(ABS_HAT0X), &ai) == 0) ||
                 (ioctl(fd, EVIOCGABS(ABS_HAT0Y), &ai) == 0);

    if (ioctl(fd, EVIOCGABS(ABS_X), &ai) == 0 && ai.maximum > ai.minimum) {
        int center = (ai.minimum + ai.maximum) / 2;
        int quarter = (ai.maximum - ai.minimum) / 4;
        if (quarter < 1) quarter = 1;
        /* Full range + current value, for analog paddle use (see
         * controller_paddle_value()) regardless of hat/centering. */
        g_stick_x_min = ai.minimum;
        g_stick_x_max = ai.maximum;
        g_stick_x_raw = ai.value;
        if (ai.value >= center - quarter && ai.value <= center + quarter) {
            g_have_stick_x = 1;
            g_stick_x_lo = center - quarter;
            g_stick_x_hi = center + quarter;
        }
    }
    if (ioctl(fd, EVIOCGABS(ABS_Y), &ai) == 0 && ai.maximum > ai.minimum) {
        int center = (ai.minimum + ai.maximum) / 2;
        int quarter = (ai.maximum - ai.minimum) / 4;
        if (quarter < 1) quarter = 1;
        if (ai.value >= center - quarter && ai.value <= center + quarter) {
            g_have_stick_y = 1;
            g_stick_y_lo = center - quarter;
            g_stick_y_hi = center + quarter;
        }
    }

    /* Analog triggers (LT/RT). "far" is whichever end isn't the resting
     * value, so a trigger resting at its minimum (typical) or at its center
     * (rare) both work. Two conventions exist for which axis code a trigger
     * lands on -- ABS_Z/ABS_RZ, or ABS_BRAKE/ABS_GAS. ABS_BRAKE/ABS_GAS is
     * tried FIRST: on a USB "Atari Game Controller" confirmed via the
     * unmapped-axis diagnostic, EVIOCGABS(ABS_Z) succeeds with a plausible
     * (but phantom/unused) range, which meant checking ABS_Z first silently
     * claimed the code and the live trigger data on ABS_BRAKE was never even
     * looked at -- the exact "advertises an axis that never moves" trap
     * documented in the webos-sdlquake-hd research (DragonRise's phantom
     * ABS_X). BRAKE/GAS is the one with actual confirmed live data, so it
     * wins the race; ABS_Z/ABS_RZ remain the fallback for pads that only
     * have those. */
    if (ioctl(fd, EVIOCGABS(ABS_BRAKE), &ai) == 0 && ai.maximum > ai.minimum) {
        g_have_trig_l = 1;
        g_trig_l_code = ABS_BRAKE;
        g_trig_l_rest = ai.value;
        g_trig_l_far  = (ai.value - ai.minimum <= ai.maximum - ai.value) ? ai.maximum : ai.minimum;
    } else if (ioctl(fd, EVIOCGABS(ABS_Z), &ai) == 0 && ai.maximum > ai.minimum) {
        g_have_trig_l = 1;
        g_trig_l_code = ABS_Z;
        g_trig_l_rest = ai.value;
        g_trig_l_far  = (ai.value - ai.minimum <= ai.maximum - ai.value) ? ai.maximum : ai.minimum;
    }
    if (ioctl(fd, EVIOCGABS(ABS_GAS), &ai) == 0 && ai.maximum > ai.minimum) {
        g_have_trig_r = 1;
        g_trig_r_code = ABS_GAS;
        g_trig_r_rest = ai.value;
        g_trig_r_far  = (ai.value - ai.minimum <= ai.maximum - ai.value) ? ai.maximum : ai.minimum;
    } else if (ioctl(fd, EVIOCGABS(ABS_RZ), &ai) == 0 && ai.maximum > ai.minimum) {
        g_have_trig_r = 1;
        g_trig_r_code = ABS_RZ;
        g_trig_r_rest = ai.value;
        g_trig_r_far  = (ai.value - ai.minimum <= ai.maximum - ai.value) ? ai.maximum : ai.minimum;
    }
}

/* Has this axis moved more than halfway from its resting value toward the
 * far end? Used to decode an analog trigger as a digital press. */
static int past_halfway(int value, int rest, int far)
{
    if (far == rest) return 0;
    return (far > rest) ? (value > (rest + far) / 2)
                         : (value < (rest + far) / 2);
}

/* ---- Hotplug scan -------------------------------------------------------
 * Only runs while no pad is open. Requires BTN_GAMEPAD or BTN_JOYSTICK
 * capability, which is what excludes the TouchPad/Pre3's own gpio-keys /
 * pmic8058_pwrkey / headset nodes without having to name-match them. */
static void scan_for_pad(void)
{
    int idx;

    for (idx = 0; idx < MAX_NODES; idx++) {
        char path[32];
        int fd;

        if (g_node_boring[idx]) continue;

        snprintf(path, sizeof(path), "/dev/input/event%d", idx);
        fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;   /* node absent -- don't cache, may appear later */

        if (!has_key_cap(fd, BTN_GAMEPAD) && !has_key_cap(fd, BTN_JOYSTICK)) {
            g_node_boring[idx] = 1;
            close(fd);
            continue;
        }

        /* Found a pad. */
        g_pad_name[0] = '\0';
        ioctl(fd, EVIOCGNAME(sizeof(g_pad_name)), g_pad_name);
        probe_axes(fd);

        g_fd = fd;
        g_node_idx = idx;

        {
            char msg[220];
            snprintf(msg, sizeof(msg),
                     "controller: connected \"%s\" on event%d (hat=%d stickx=%d sticky=%d trigl_code=%d trigr_code=%d)",
                     g_pad_name[0] ? g_pad_name : "?", idx, g_have_hat, g_have_stick_x, g_have_stick_y,
                     g_have_trig_l ? g_trig_l_code : -1, g_have_trig_r ? g_trig_r_code : -1);
            log_msg(msg);
        }
        return;
    }
}

/* ---- Event decode -------------------------------------------------------- */

static void decode_key(int code, int value)
{
    int pressed = value != 0;   /* value 2 = autorepeat; treat as still-down */

    switch (code) {
        /* Face buttons: B/Y -> Fire 1, A/X -> Fire 2 (matches the Android
         * port's CONTROLS MAP exactly). */
        case BTN_EAST:  case BTN_WEST:        g_fire  = pressed; break;
        case BTN_SOUTH: case BTN_NORTH:       g_fire2 = pressed; break;
        case BTN_START:                       g_start = pressed; break;
        case BTN_SELECT:                      g_back  = pressed; break;
        case BTN_TL:                          g_save  = pressed; break;  /* LB */
        case BTN_TR:                          g_load  = pressed; break;  /* RB */
        case BTN_TL2:                         g_select_sw = pressed; break;  /* LT (digital) */
        case BTN_TR2:                         g_reset_sw  = pressed; break;  /* RT (digital) */
        case BTN_JOYSTICK:      /* generic "button 1" */
            g_fire  = pressed; break;
        case BTN_JOYSTICK + 1:  /* generic "button 2" */
            g_fire2 = pressed; break;
        case BTN_JOYSTICK + 4:  /* generic "button 5" (LB) */
            g_save  = pressed; break;
        case BTN_JOYSTICK + 5:  /* generic "button 6" (RB) */
            g_load  = pressed; break;
        case BTN_JOYSTICK + 6:  /* generic "button 7" (LT) */
            g_select_sw = pressed; break;
        case BTN_JOYSTICK + 7:  /* generic "button 8" (RT) */
            g_reset_sw  = pressed; break;
        case BTN_JOYSTICK + 9:  /* generic "button 10" (Logitech-style) */
            g_start = pressed; break;
        case BTN_JOYSTICK + 8:  /* generic "button 9" */
            g_back  = pressed; break;
        /* Some pads (confirmed on a USB "Atari Game Controller") report
         * Start/Select as consumer KEY_MENU/KEY_BACK rather than a BTN_*
         * joystick code -- found via the unmapped-button diagnostic below. */
        case KEY_MENU:                        g_start = pressed; break;
        case KEY_BACK:                        g_back  = pressed; break;
        case BTN_DPAD_UP:    g_up    = pressed; break;
        case BTN_DPAD_DOWN:  g_down  = pressed; break;
        case BTN_DPAD_LEFT:  g_left  = pressed; break;
        case BTN_DPAD_RIGHT: g_right = pressed; break;
        default:
            log_unmapped_button(code, value);
            break;
    }
}

static void decode_abs(int code, int value)
{
    if (code == ABS_HAT0X) {
        g_left  = value < 0;
        g_right = value > 0;
    } else if (code == ABS_HAT0Y) {
        g_up   = value < 0;
        g_down = value > 0;
    } else if (code == ABS_X) {
        g_stick_x_raw = value;   /* kept live for controller_paddle_value() */
        if (!g_have_hat && g_have_stick_x) {
            g_left  = value < g_stick_x_lo;
            g_right = value > g_stick_x_hi;
        }
    } else if (code == ABS_Y && !g_have_hat && g_have_stick_y) {
        g_up   = value < g_stick_y_lo;
        g_down = value > g_stick_y_hi;
    } else if (g_have_trig_l && code == g_trig_l_code) {  /* LT (analog) */
        g_select_sw = past_halfway(value, g_trig_l_rest, g_trig_l_far);
    } else if (g_have_trig_r && code == g_trig_r_code) {  /* RT (analog) */
        g_reset_sw = past_halfway(value, g_trig_r_rest, g_trig_r_far);
    } else if (code != ABS_HAT0X && code != ABS_HAT0Y && code != ABS_X && code != ABS_Y) {
        log_unmapped_axis(code, value);
    }
}

static void service_pad(void)
{
    struct input_event ev;
    ssize_t n;

    for (;;) {
        n = read(g_fd, &ev, sizeof(ev));
        if (n != (ssize_t)sizeof(ev)) break;

        if (ev.type == EV_KEY) {
            decode_key(ev.code, ev.value);
        } else if (ev.type == EV_ABS) {
            decode_abs(ev.code, ev.value);
        }
        /* EV_SYN and anything else: ignore */
    }

    if (n < 0 && errno != EAGAIN) {
        /* Unplugged mid-session (typically EIO/ENODEV on the next read). */
        close_pad(1);
    }
}

void controller_poll(void)
{
    if (g_fd >= 0) {
        service_pad();
    } else {
        if (g_scan_countdown <= 0) {
            scan_for_pad();
            g_scan_countdown = SCAN_INTERVAL;
        } else {
            g_scan_countdown--;
        }
    }

    /* Recompute this frame's edges from level transitions. */
    g_e_start    = g_start && !g_prev_start;
    g_e_back     = g_back  && !g_prev_back;
    g_e_a        = g_fire  && !g_prev_fire;
    g_e_b        = g_fire2 && !g_prev_fire2;
    g_e_save     = g_save  && !g_prev_save;
    g_e_load     = g_load  && !g_prev_load;

    if (g_up)   g_up_hold++;   else g_up_hold   = 0;
    if (g_down) g_down_hold++; else g_down_hold = 0;

    g_e_nav_up   = (g_up_hold   == 1) ||
                   (g_up_hold   > NAV_REPEAT_DELAY   && (g_up_hold   - NAV_REPEAT_DELAY)   % NAV_REPEAT_EVERY == 0);
    g_e_nav_down = (g_down_hold == 1) ||
                   (g_down_hold > NAV_REPEAT_DELAY   && (g_down_hold - NAV_REPEAT_DELAY)   % NAV_REPEAT_EVERY == 0);

    g_prev_start = g_start;
    g_prev_back  = g_back;
    g_prev_fire  = g_fire;
    g_prev_fire2 = g_fire2;
    g_prev_save  = g_save;
    g_prev_load  = g_load;
}

void controller_dpad(int *up, int *down, int *left, int *right)
{
    if (up)    *up    = g_up;
    if (down)  *down  = g_down;
    if (left)  *left  = g_left;
    if (right) *right = g_right;
}

int controller_fire(void)  { return g_fire; }
int controller_fire2(void) { return g_fire2; }

int controller_start_edge(void)    { return g_e_start; }
int controller_back_edge(void)     { return g_e_back; }
int controller_nav_up_edge(void)   { return g_e_nav_up; }
int controller_nav_down_edge(void) { return g_e_nav_down; }
int controller_a_edge(void)        { return g_e_a; }
int controller_b_edge(void)        { return g_e_b; }

int controller_select_switch(void) { return g_select_sw; }  /* LT */
int controller_reset_switch(void)  { return g_reset_sw; }   /* RT */
int controller_save_edge(void)     { return g_e_save; }     /* LB */
int controller_load_edge(void)     { return g_e_load; }     /* RB */

/* Analog paddle: only meaningful when a genuine centered stick axis was
 * found (g_have_stick_x) -- a d-pad-only pad has no analog input to give. */
int controller_has_analog_paddle(void)
{
    return g_have_stick_x;
}

int controller_paddle_value(void)
{
    int range = g_stick_x_max - g_stick_x_min;
    if (range <= 0) return 128;
    return ((g_stick_x_raw - g_stick_x_min) * 255) / range;
}
