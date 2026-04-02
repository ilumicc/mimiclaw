#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "mimi_config.h"

typedef enum {
    MIMI_VOICE_STATE_IDLE = 0,
    MIMI_VOICE_STATE_RECORDING,
    MIMI_VOICE_STATE_PROCESSING,
    MIMI_VOICE_STATE_PLAYING,
} mimi_voice_state_t;

#if MIMI_VOICE_ENABLED

esp_err_t voice_channel_init(void);
esp_err_t voice_channel_start(void);
esp_err_t voice_channel_send_message(const char *text);
esp_err_t voice_channel_stop(void);
esp_err_t voice_channel_get_state(mimi_voice_state_t *out_state);
bool voice_channel_is_initialized(void);

#else

static inline esp_err_t voice_channel_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t voice_channel_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t voice_channel_send_message(const char *text)
{
    (void)text;
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t voice_channel_stop(void)
{
    return ESP_OK;
}

static inline esp_err_t voice_channel_get_state(mimi_voice_state_t *out_state)
{
    if (out_state) {
        *out_state = MIMI_VOICE_STATE_IDLE;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static inline bool voice_channel_is_initialized(void)
{
    return false;
}

#endif
