/*
 * audio.h
 *
 * SDL Audio Output Header
 *
 * Copyright (c) 2024 EMU7800
 */

#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

/* Initialize audio subsystem */
int audio_init(void);

/* Shutdown audio subsystem */
void audio_shutdown(void);

/* Update audio buffer */
void audio_update(void);

/* Pause audio */
void audio_pause(void);

/* Resume audio */
void audio_resume(void);

/* Get actual sample rate (after SDL negotiation) */
int audio_get_sample_rate(void);

/* Diagnostic functions */
int audio_get_underruns(void);
int audio_get_buffer_fill(void);

/* Get detailed audio callback timing stats (times in microseconds) */
void audio_get_timing_stats(uint64_t *last_interval, uint64_t *max_interval,
                            uint64_t *last_duration, uint64_t *max_duration,
                            int *interval_anomalies, int *callback_count);

#endif /* AUDIO_H */
