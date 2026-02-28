/*
 * PDL.h
 *
 * Palm Development Library Stub Header
 * Provides PDL API declarations for cross-compilation
 *
 * On device, link against actual libpdl.so
 */

#ifndef PDL_H
#define PDL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Boolean type */
typedef int PDL_bool;
#define PDL_TRUE  1
#define PDL_FALSE 0

/* Error codes */
typedef int PDL_Err;
#define PDL_NOERROR         0
#define PDL_EMEMORY         1
#define PDL_EOTHER          2
#define PDL_EINVALIDINPUT   3
#define PDL_ECONNECTION     4
#define PDL_NOTALLOWED      5
#define PDL_EUNAVAIL        6
#define PDL_ESTATE          7

/* JS Parameters structure (opaque) */
typedef struct PDL_JSParameters PDL_JSParameters;

/* Initialization */
PDL_Err PDL_Init(unsigned int flags);
PDL_Err PDL_Quit(void);

/* Screen control */
PDL_Err PDL_ScreenTimeoutEnable(PDL_bool enable);
PDL_Err PDL_SetOrientation(int orientation);
PDL_Err PDL_GetScreenMetrics(int *width, int *height);

/* Touch input */
PDL_Err PDL_SetTouchAggression(int level);  /* 0=fewer touches, 1=more touches */

/* JS Plugin interface */
PDL_Err PDL_RegisterJSHandler(const char *functionName,
                               PDL_bool (*handler)(PDL_JSParameters *params));
PDL_Err PDL_JSRegistrationComplete(void);

/* JS Parameter access */
int PDL_GetNumJSParams(PDL_JSParameters *params);
const char *PDL_GetJSParamString(PDL_JSParameters *params, int index);
int PDL_GetJSParamInt(PDL_JSParameters *params, int index);
double PDL_GetJSParamDouble(PDL_JSParameters *params, int index);

/* JS Response */
PDL_Err PDL_JSReply(PDL_JSParameters *params, const char *reply);

/* Call JS from plugin */
PDL_Err PDL_CallJS(const char *functionName, const char **params, int numParams);

/* Device info */
PDL_Err PDL_GetHardwareID(char *buffer, int bufferLen);
PDL_Err PDL_GetUniqueID(char *buffer, int bufferLen);

/* Misc */
PDL_Err PDL_Minimize(void);
PDL_Err PDL_LaunchBrowser(const char *url);
PDL_Err PDL_Vibrate(int periodMS, int durationMS);

/* Service calls */
PDL_Err PDL_ServiceCall(const char *uri, const char *payload);
PDL_Err PDL_ServiceCallWithCallback(const char *uri, const char *payload,
                                     PDL_bool (*callback)(const char *result, void *user),
                                     void *user, PDL_bool removeAfterResponse);

#ifdef __cplusplus
}
#endif

#endif /* PDL_H */
