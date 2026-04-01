#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "audio/audio_frame.h"

typedef enum {
    VOICE_SESSION_IDLE = 0,
    VOICE_SESSION_WAKE_HIT,
    VOICE_SESSION_LISTENING,
    VOICE_SESSION_END_UTTERANCE,
} voice_session_state_t;

typedef struct {
    voice_session_state_t state;
    uint32_t wake_hits;
    uint32_t sessions_started;
    uint32_t sessions_ready;
    uint32_t sessions_timeout;
    uint32_t sessions_clipped;
    uint32_t last_session_ms;
    uint32_t last_session_bytes;
    uint32_t last_session_frames;
    char last_wake_source[16];
    char last_wake_phrase[64];
} voice_session_stats_t;

typedef struct {
    bool valid;
    bool timeout_reason;
    bool clipped;
    uint64_t start_ts_ms;
    uint64_t end_ts_ms;
    uint32_t duration_ms;
    uint32_t audio_bytes;
    uint32_t audio_frames;
    uint32_t sample_rate_hz;
    uint8_t channels;
    uint8_t bits_per_sample;
} voice_session_ready_t;

esp_err_t voice_session_coordinator_init(void);

esp_err_t voice_session_coordinator_on_audio_frame(const audio_frame_t *frame);
esp_err_t voice_session_coordinator_on_transcript_final(const char *text);
esp_err_t voice_session_coordinator_force_ready(void);

voice_session_state_t voice_session_coordinator_state(void);
const char *voice_session_coordinator_state_str(voice_session_state_t state);

esp_err_t voice_session_coordinator_get_stats(voice_session_stats_t *out);
esp_err_t voice_session_coordinator_get_last_ready(voice_session_ready_t *out);
