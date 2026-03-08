#pragma once

#include "esp_err.h"

/**
 * Start WiFi onboarding captive portal.
 * Opens a soft AP, DNS hijacker, and HTTP configuration server.
 * Blocks until the user submits credentials, then saves to NVS and restarts.
 */
esp_err_t wifi_onboard_start(void);
