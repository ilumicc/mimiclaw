#pragma once

#include <stddef.h>
#include <stdbool.h>

#include "esp_err.h"

esp_err_t tts_service_init(void);
esp_err_t tts_service_start(void);

esp_err_t tts_service_enqueue_text(const char *text);
esp_err_t tts_service_stop_current(void);
esp_err_t tts_service_flush_queue(void);

bool tts_service_is_ready(void);
bool tts_service_is_configured(void);

esp_err_t tts_service_diag(char *buf, size_t buf_size);
