#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

typedef struct {
    uint32_t play_requests;
    uint32_t play_ok;
    uint32_t play_fail;
    uint32_t stop_requests;
    uint32_t abort_requests;
    uint32_t write_timeouts;
    uint32_t last_sample_rate;
    uint16_t last_channels;
    uint16_t last_bits_per_sample;
    uint32_t last_data_len;
    esp_err_t last_error;
    bool is_playing;
} speaker_i2s_stats_t;

esp_err_t speaker_i2s_init(void);
bool speaker_i2s_is_initialized(void);

esp_err_t speaker_i2s_play_wav(const uint8_t *wav_data, size_t wav_len);
esp_err_t speaker_i2s_request_stop(void);
esp_err_t speaker_i2s_stop(void);
esp_err_t speaker_i2s_get_stats(speaker_i2s_stats_t *out);
