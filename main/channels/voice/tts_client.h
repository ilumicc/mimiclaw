#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "mimi_config.h"

#if MIMI_VOICE_ENABLED

typedef struct {
    uint8_t *pcm_data;
    size_t pcm_len;
    uint32_t sample_rate;
} tts_result_t;

esp_err_t tts_client_init(void);
esp_err_t tts_synthesize(const char *text, tts_result_t *out_result);
void tts_result_free(tts_result_t *result);

#else

typedef struct {
    uint8_t *pcm_data;
    size_t pcm_len;
    uint32_t sample_rate;
} tts_result_t;

static inline esp_err_t tts_client_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t tts_synthesize(const char *text, tts_result_t *out_result)
{
    (void)text;
    if (out_result) {
        out_result->pcm_data = NULL;
        out_result->pcm_len = 0;
        out_result->sample_rate = 0;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static inline void tts_result_free(tts_result_t *result)
{
    (void)result;
}

#endif
