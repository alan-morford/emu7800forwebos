/*
 * updater.h
 *
 * Silent update checker via App Museum II web service.
 * Spawns a background thread on init to query for newer versions.
 *
 * Copyright (c) 2026 EMU7800
 */

#ifndef UPDATER_H
#define UPDATER_H

/* Must match appinfo.json version */
#define APP_VERSION "1.8.0"

/* Start the background update check (idempotent — no-op if already started) */
void updater_check_start(void);

/* Returns 1 if an update was found and not yet dismissed */
int updater_has_update(void);

/* Get the new version string (e.g. "1.6.0") — valid only when updater_has_update() */
const char *updater_get_version(void);

/* Get the version note text — valid only when updater_has_update() */
const char *updater_get_note(void);

/* Launch Preware to install the update */
void updater_install(void);

/* Dismiss the update notification for this session */
void updater_dismiss(void);

/* Join the background thread (call on shutdown) */
void updater_shutdown(void);

#endif /* UPDATER_H */
