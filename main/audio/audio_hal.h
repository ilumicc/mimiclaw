#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "mimi_config.h"

#if MIMI_VOICE_ENABLED

esp_err_t audio_hal_init(void);
size_t audio_hal_mic_read(int16_t *pcm_out, size_t max_samples, uint32_t timeout_ms);
esp_err_t audio_hal_spk_write(const int16_t *pcm, size_t samples, size_t *samples_written, uint32_t timeout_ms);
esp_err_t audio_hal_spk_set_sample_rate(uint32_t sample_rate_hz);
esp_err_t audio_hal_spk_enable(bool enable);
esp_err_t audio_hal_deinit(void);

#else

static inline esp_err_t audio_hal_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static inline size_t audio_hal_mic_read(int16_t *pcm_out, size_t max_samples, uint32_t timeout_ms)
{
    (void)pcm_out;
    (void)max_samples;
    (void)timeout_ms;
    return 0;
}

static inline esp_err_t audio_hal_spk_write(const int16_t *pcm, size_t samples, size_t *samples_written, uint32_t timeout_ms)
{
    (void)pcm;
    (void)samples;
    if (samples_written) {
        *samples_written = 0;
    }
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t audio_hal_spk_set_sample_rate(uint32_t sample_rate_hz)
{
    (void)sample_rate_hz;
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t audio_hal_spk_enable(bool enable)
{
    (void)enable;
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t audio_hal_deinit(void)
{
    return ESP_OK;
}

#endif
