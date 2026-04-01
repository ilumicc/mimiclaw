#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "audio/audio_frame.h"

typedef struct {
    uint64_t ts_ms;
    char source[16];
    char phrase[64];
} wake_event_t;

typedef struct {
    bool initialized;
    bool enabled;
    bool paused;
    bool local_detector_enabled;
    uint32_t pause_depth;
    uint32_t cooldown_ms;
    uint32_t wake_detected;
    uint32_t suppressed_disabled;
    uint32_t suppressed_paused;
    uint32_t suppressed_cooldown;
    uint32_t callbacks_fired;
    uint32_t audio_frames_processed;
    uint32_t local_rms_hits;
    uint16_t local_rms_threshold;
    uint32_t last_wake_age_ms;
    char last_source[16];
    char last_phrase[64];
} wake_service_stats_t;

typedef void (*wake_service_cb_t)(const wake_event_t *event, void *ctx);

esp_err_t wake_service_init(void);

esp_err_t wake_service_set_enabled(bool enabled);
bool wake_service_is_enabled(void);

esp_err_t wake_service_set_cooldown_ms(uint32_t cooldown_ms);
esp_err_t wake_service_set_local_detector_enabled(bool enabled);
esp_err_t wake_service_set_rms_threshold(uint16_t threshold);

esp_err_t wake_service_on_audio_frame(const audio_frame_t *frame);

uint32_t wake_service_get_cooldown_ms(void);

esp_err_t wake_service_pause(const char *reason);
esp_err_t wake_service_resume(const char *reason);
bool wake_service_is_paused(void);

esp_err_t wake_service_notify_detected(const char *source, const char *phrase);

esp_err_t wake_service_set_callback(wake_service_cb_t cb, void *ctx);
esp_err_t wake_service_get_stats(wake_service_stats_t *out);
