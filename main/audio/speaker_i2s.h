#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

esp_err_t speaker_i2s_init(void);
bool speaker_i2s_is_initialized(void);

esp_err_t speaker_i2s_play_wav(const uint8_t *wav_data, size_t wav_len);
