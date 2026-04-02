#pragma once

#include "esp_err.h"
#include "mimi_config.h"

#if MIMI_VOICE_ENABLED

esp_err_t voice_channel_init(void);
esp_err_t voice_channel_start(void);
esp_err_t voice_channel_send_message(const char *text);
esp_err_t voice_channel_stop(void);

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

#endif
