#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "mimi_config.h"

typedef struct {
    const int16_t *processed_audio;
    size_t processed_samples;
    bool wake_word_detected;
    bool vad_speech;
} voice_afe_result_t;

#if MIMI_VOICE_ENABLED

esp_err_t voice_afe_init(void);
esp_err_t voice_afe_feed(const int16_t *audio, size_t samples);
esp_err_t voice_afe_fetch(voice_afe_result_t *out, uint32_t timeout_ms);
int voice_afe_get_feed_chunksize(void);
int voice_afe_get_feed_channels(void);
esp_err_t voice_afe_wakenet_enable(bool enable);
esp_err_t voice_afe_deinit(void);

#else

static inline esp_err_t voice_afe_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t voice_afe_feed(const int16_t *audio, size_t samples)
{
    (void)audio;
    (void)samples;
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t voice_afe_fetch(voice_afe_result_t *out, uint32_t timeout_ms)
{
    if (out) {
        out->processed_audio = NULL;
        out->processed_samples = 0;
        out->wake_word_detected = false;
        out->vad_speech = false;
    }
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

static inline int voice_afe_get_feed_chunksize(void)
{
    return 0;
}

static inline int voice_afe_get_feed_channels(void)
{
    return 0;
}

static inline esp_err_t voice_afe_wakenet_enable(bool enable)
{
    (void)enable;
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t voice_afe_deinit(void)
{
    return ESP_OK;
}

#endif
