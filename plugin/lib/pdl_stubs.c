/*
 * PDL stub library for cross-compilation
 * These empty functions satisfy the linker
 * Real implementations come from device libraries at runtime
 */

/* PDL stubs */
int PDL_Init(unsigned int flags) { return 0; }
int PDL_Quit(void) { return 0; }
int PDL_ScreenTimeoutEnable(int enable) { return 0; }
int PDL_SetOrientation(int orientation) { return 0; }
int PDL_GetScreenMetrics(int *width, int *height) {
    if (width) *width = 1024;
    if (height) *height = 768;
    return 0;
}
