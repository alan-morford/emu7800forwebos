/*
 * audio.c
 *
 * SDL Audio Output
 *
 * Copyright (c) 2024 EMU7800
 */

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <SDL.h>
#include "audio.h"
#include "machine.h"
#include "tiasound.h"

/* Get current time in microseconds (for audio callback timing) */
static uint64_t audio_get_time_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

/* Audio settings */
/* TIA generates audio at 31440 Hz for NTSC (3.58MHz / 114) */
#define AUDIO_SAMPLE_RATE  31440
#define AUDIO_SAMPLES      512     /* ~16ms buffer - lower latency */
#define AUDIO_CHANNELS     1

/* Ring buffer for audio
 * Size affects latency vs stability tradeoff:
 * - Larger = more latency but handles timing variance
 * - Smaller = lower latency but more underrun risk
 * At 31440 Hz: 4096 samples = ~130ms latency (enough for ~8 frames)
 */
#define RING_BUFFER_SIZE   4096
static int16_t g_ring_buffer[RING_BUFFER_SIZE];
static volatile int g_ring_read = 0;
static volatile int g_ring_write = 0;

static int g_audio_open = 0;
static int g_actual_sample_rate = AUDIO_SAMPLE_RATE;

/* Diagnostic counters for audio */
static volatile int g_audio_underruns = 0;
static volatile int g_audio_callback_count = 0;

/* Timing diagnostics (all times in microseconds) */
static volatile uint64_t g_last_callback_time = 0;
static volatile uint64_t g_last_callback_interval = 0;
static volatile uint64_t g_last_callback_duration = 0;
static volatile uint64_t g_max_callback_interval = 0;
static volatile uint64_t g_max_callback_duration = 0;
static volatile int g_callback_interval_anomalies = 0;  /* Intervals > 25ms */

/* Audio callback - called by SDL audio thread */
static void audio_callback(void *userdata, Uint8 *stream, int len)
{
    uint64_t now = audio_get_time_us();
    uint64_t interval;
    int16_t *out = (int16_t *)stream;
    int samples = len / sizeof(int16_t);
    int i;
    int underrun_samples = 0;

    (void)userdata;

    /* Track callback interval (time since last callback) */
    if (g_last_callback_time > 0) {
        interval = now - g_last_callback_time;
        g_last_callback_interval = interval;
        if (interval > g_max_callback_interval) {
            g_max_callback_interval = interval;
        }
        /* Expected interval ~16ms for 512 samples at 31440Hz */
        if (interval > 25000) {  /* > 25ms is anomalous */
            g_callback_interval_anomalies++;
        }
    }

    g_audio_callback_count++;

    for (i = 0; i < samples; i++) {
        if (g_ring_read != g_ring_write) {
            out[i] = g_ring_buffer[g_ring_read];
            g_ring_read = (g_ring_read + 1) % RING_BUFFER_SIZE;
        } else {
            out[i] = 0;
            underrun_samples++;
        }
    }

    if (underrun_samples > 0) {
        g_audio_underruns++;
    }

    /* Track callback duration */
    g_last_callback_duration = audio_get_time_us() - now;
    if (g_last_callback_duration > g_max_callback_duration) {
        g_max_callback_duration = g_last_callback_duration;
    }
    g_last_callback_time = now;
}

/* Get audio diagnostics */
int audio_get_underruns(void)
{
    return g_audio_underruns;
}

int audio_get_buffer_fill(void)
{
    int write = g_ring_write;
    int read = g_ring_read;
    if (write >= read) {
        return write - read;
    } else {
        return RING_BUFFER_SIZE - read + write;
    }
}

/* Get detailed audio timing diagnostics and reset max values */
void audio_get_timing_stats(uint64_t *last_interval, uint64_t *max_interval,
                            uint64_t *last_duration, uint64_t *max_duration,
                            int *interval_anomalies, int *callback_count)
{
    if (last_interval) *last_interval = g_last_callback_interval;
    if (max_interval) *max_interval = g_max_callback_interval;
    if (last_duration) *last_duration = g_last_callback_duration;
    if (max_duration) *max_duration = g_max_callback_duration;
    if (interval_anomalies) *interval_anomalies = g_callback_interval_anomalies;
    if (callback_count) *callback_count = g_audio_callback_count;

    /* Reset max values after reading */
    g_max_callback_interval = 0;
    g_max_callback_duration = 0;
}

/* Initialize audio */
int audio_init(void)
{
    SDL_AudioSpec desired, obtained;

    memset(&desired, 0, sizeof(desired));
    desired.freq = AUDIO_SAMPLE_RATE;
    desired.format = AUDIO_S16SYS;
    desired.channels = AUDIO_CHANNELS;
    desired.samples = AUDIO_SAMPLES;
    desired.callback = audio_callback;
    desired.userdata = NULL;

    if (SDL_OpenAudio(&desired, &obtained) < 0) {
        return -1;
    }

    /* Use the actual sample rate SDL gave us */
    g_actual_sample_rate = obtained.freq;

    g_ring_read = 0;
    g_ring_write = 0;
    g_audio_open = 1;

    /* Start audio */
    SDL_PauseAudio(0);

    return 0;
}

/* Get actual sample rate (for TIA sound init) */
int audio_get_sample_rate(void)
{
    return g_actual_sample_rate;
}

/* Shutdown audio */
void audio_shutdown(void)
{
    if (g_audio_open) {
        SDL_CloseAudio();
        g_audio_open = 0;
    }
}

/*
 * Update audio buffer - lock-free SPSC queue.
 * Producer (this function) only writes g_ring_write.
 * Consumer (audio_callback) only writes g_ring_read.
 * No SDL locking needed - volatile indices provide visibility.
 */
void audio_update(void)
{
    int16_t *buffer;
    int samples;
    int i;
    int next_write;
    int read_pos;

    if (!g_audio_open) return;

    buffer = machine_get_sound_buffer();
    samples = machine_get_sound_samples();

    if (!buffer || samples <= 0) return;

    /* Snapshot read position once to avoid repeated volatile reads */
    read_pos = g_ring_read;

    /* Add samples to ring buffer - no locking needed */
    for (i = 0; i < samples; i++) {
        next_write = (g_ring_write + 1) % RING_BUFFER_SIZE;
        if (next_write != read_pos) {
            g_ring_buffer[g_ring_write] = buffer[i];
            g_ring_write = next_write;
        }
        /* If buffer full, just drop samples silently */
    }
}

/* Pause audio */
void audio_pause(void)
{
    if (g_audio_open) {
        SDL_PauseAudio(1);
    }
}

/* Resume audio */
void audio_resume(void)
{
    if (g_audio_open) {
        SDL_PauseAudio(0);
    }
}
