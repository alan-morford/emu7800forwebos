/*
 * bridge.c
 *
 * Mojo-PDK Bridge
 * Handles JavaScript to plugin communication
 *
 * Copyright (c) 2024 EMU7800
 */

#include <string.h>
#include <stdio.h>
#include "PDL.h"
#include "machine.h"
#include "bridge.h"

/* External functions from main.c */
extern void main_start_emulator(void);
extern void main_stop_emulator(void);
extern int main_is_running(void);

/*
 * loadRom(path, machineType)
 * Load a ROM file from the given path
 * machineType: 0 = auto-detect/2600, 1 = 7800
 * Returns: 0 on success, error code on failure
 */
static PDL_bool js_load_rom(PDL_JSParameters *params)
{
    const char *path;
    int machine_type;
    int result;
    char reply[32];

    if (PDL_GetNumJSParams(params) < 2) {
        PDL_JSReply(params, "-1");
        return PDL_TRUE;
    }

    path = PDL_GetJSParamString(params, 0);
    machine_type = PDL_GetJSParamInt(params, 1);

    if (path == NULL || strlen(path) == 0) {
        PDL_JSReply(params, "-2");
        return PDL_TRUE;
    }

    /* Load the ROM */
    result = machine_load_rom(path, machine_type);

    snprintf(reply, sizeof(reply), "%d", result);
    PDL_JSReply(params, reply);

    return PDL_TRUE;
}

/*
 * startEmulator()
 * Start the emulator
 */
static PDL_bool js_start_emulator(PDL_JSParameters *params)
{
    if (machine_is_loaded()) {
        main_start_emulator();
        PDL_JSReply(params, "0");
    } else {
        PDL_JSReply(params, "-1");
    }
    return PDL_TRUE;
}

/*
 * stopEmulator()
 * Stop the emulator
 */
static PDL_bool js_stop_emulator(PDL_JSParameters *params)
{
    main_stop_emulator();
    PDL_JSReply(params, "0");
    return PDL_TRUE;
}

/*
 * isRunning()
 * Check if emulator is running
 */
static PDL_bool js_is_running(PDL_JSParameters *params)
{
    char reply[8];
    snprintf(reply, sizeof(reply), "%d", main_is_running() ? 1 : 0);
    PDL_JSReply(params, reply);
    return PDL_TRUE;
}

/*
 * reset()
 * Reset the current machine
 */
static PDL_bool js_reset(PDL_JSParameters *params)
{
    if (machine_is_loaded()) {
        machine_reset();
        PDL_JSReply(params, "0");
    } else {
        PDL_JSReply(params, "-1");
    }
    return PDL_TRUE;
}

/*
 * Initialize the JavaScript bridge
 * Register all handler functions
 */
void bridge_init(void)
{
    PDL_RegisterJSHandler("loadRom", js_load_rom);
    PDL_RegisterJSHandler("startEmulator", js_start_emulator);
    PDL_RegisterJSHandler("stopEmulator", js_stop_emulator);
    PDL_RegisterJSHandler("isRunning", js_is_running);
    PDL_RegisterJSHandler("reset", js_reset);
}
