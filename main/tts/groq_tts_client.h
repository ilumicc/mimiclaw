#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

esp_err_t groq_tts_client_init(void);

esp_err_t groq_tts_set_api_key(const char *api_key);
esp_err_t groq_tts_set_model(const char *model);
esp_err_t groq_tts_set_voice(const char *voice);

bool groq_tts_is_configured(void);
const char *groq_tts_get_model(void);
const char *groq_tts_get_voice(void);

esp_err_t groq_tts_synthesize_wav(const char *text, uint8_t **out_wav, size_t *out_len);
