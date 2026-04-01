#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t voice_channel_init(void);
esp_err_t voice_channel_start(void);
esp_err_t voice_channel_stop(void);
esp_err_t voice_channel_send_text(const char *chat_id, const char *text);

esp_err_t voice_channel_set_ws_url(const char *url);
esp_err_t voice_channel_set_ws_token(const char *token);
esp_err_t voice_channel_set_ws_version(const char *version);

bool voice_channel_is_enabled(void);
bool voice_channel_is_connected(void);
