#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "mimi_config.h"

#if MIMI_VOICE_ENABLED

esp_err_t stt_client_init(void);
esp_err_t stt_transcribe(const int16_t *pcm, size_t sample_count, char **out_text);

#else

static inline esp_err_t stt_client_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t stt_transcribe(const int16_t *pcm, size_t sample_count, char **out_text)
{
    (void)pcm;
    (void)sample_count;
    if (out_text) {
        *out_text = NULL;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
