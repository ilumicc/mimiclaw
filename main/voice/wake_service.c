#include "voice/wake_service.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "mimi_config.h"

static const char *TAG = "wake";

static SemaphoreHandle_t s_lock = NULL;
static bool s_initialized = false;
static bool s_enabled = (MIMI_WAKE_ENABLED_DEFAULT != 0);
static uint32_t s_cooldown_ms = MIMI_WAKE_COOLDOWN_MS;
static uint32_t s_pause_depth = 0;
static TickType_t s_last_wake_tick = 0;
static wake_service_cb_t s_callback = NULL;
static void *s_callback_ctx = NULL;

static wake_service_stats_t s_stats = {0};

static bool wake_lock_take(TickType_t ticks)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (!s_lock) {
        return false;
    }
    return xSemaphoreTake(s_lock, ticks) == pdTRUE;
}

static void wake_lock_give(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static esp_err_t wake_save_enabled(bool enabled)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MIMI_NVS_VOICE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(nvs, MIMI_NVS_KEY_WAKE_ENABLED, enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static esp_err_t wake_save_cooldown_ms(uint32_t cooldown_ms)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MIMI_NVS_VOICE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    uint16_t value = (cooldown_ms > 65535U) ? 65535U : (uint16_t)cooldown_ms;
    err = nvs_set_u16(nvs, MIMI_NVS_KEY_WAKE_COOLDOWN_MS, value);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t wake_service_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    if (!wake_lock_take(pdMS_TO_TICKS(100))) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_initialized) {
        wake_lock_give();
        return ESP_OK;
    }

    s_enabled = (MIMI_WAKE_ENABLED_DEFAULT != 0);
    s_cooldown_ms = MIMI_WAKE_COOLDOWN_MS;

    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_VOICE, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t wake_enabled = 0;
        if (nvs_get_u8(nvs, MIMI_NVS_KEY_WAKE_ENABLED, &wake_enabled) == ESP_OK) {
            s_enabled = (wake_enabled != 0);
        }

        uint16_t cooldown = 0;
        if (nvs_get_u16(nvs, MIMI_NVS_KEY_WAKE_COOLDOWN_MS, &cooldown) == ESP_OK && cooldown > 0) {
            s_cooldown_ms = cooldown;
        }
        nvs_close(nvs);
    }

    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.initialized = true;
    s_stats.enabled = s_enabled;
    s_stats.cooldown_ms = s_cooldown_ms;

    s_initialized = true;
    wake_lock_give();

    ESP_LOGI(TAG, "Wake service initialized: enabled=%s cooldown=%u ms",
             s_enabled ? "yes" : "no",
             (unsigned)s_cooldown_ms);
    return ESP_OK;
}

esp_err_t wake_service_set_enabled(bool enabled)
{
    if (!s_initialized) {
        esp_err_t init_err = wake_service_init();
        if (init_err != ESP_OK) {
            return init_err;
        }
    }

    if (!wake_lock_take(pdMS_TO_TICKS(100))) {
        return ESP_ERR_TIMEOUT;
    }

    s_enabled = enabled;
    s_stats.enabled = enabled;
    wake_lock_give();

    esp_err_t err = wake_save_enabled(enabled);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Wake %s", enabled ? "enabled" : "disabled");
    }
    return err;
}

bool wake_service_is_enabled(void)
{
    return s_enabled;
}

esp_err_t wake_service_set_cooldown_ms(uint32_t cooldown_ms)
{
    if (cooldown_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_initialized) {
        esp_err_t init_err = wake_service_init();
        if (init_err != ESP_OK) {
            return init_err;
        }
    }

    if (!wake_lock_take(pdMS_TO_TICKS(100))) {
        return ESP_ERR_TIMEOUT;
    }

    uint32_t cooldown_apply_ms = (cooldown_ms > 65535U) ? 65535U : cooldown_ms;
    s_cooldown_ms = cooldown_apply_ms;
    s_stats.cooldown_ms = cooldown_apply_ms;
    wake_lock_give();

    return wake_save_cooldown_ms(cooldown_apply_ms);
}

uint32_t wake_service_get_cooldown_ms(void)
{
    return s_cooldown_ms;
}

esp_err_t wake_service_pause(const char *reason)
{
    (void)reason;
    if (!s_initialized) {
        esp_err_t init_err = wake_service_init();
        if (init_err != ESP_OK) {
            return init_err;
        }
    }

    if (!wake_lock_take(pdMS_TO_TICKS(100))) {
        return ESP_ERR_TIMEOUT;
    }

    s_pause_depth++;
    s_stats.pause_depth = s_pause_depth;
    s_stats.paused = (s_pause_depth > 0);
    wake_lock_give();
    return ESP_OK;
}

esp_err_t wake_service_resume(const char *reason)
{
    (void)reason;
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!wake_lock_take(pdMS_TO_TICKS(100))) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_pause_depth > 0) {
        s_pause_depth--;
    }
    s_stats.pause_depth = s_pause_depth;
    s_stats.paused = (s_pause_depth > 0);
    wake_lock_give();
    return ESP_OK;
}

bool wake_service_is_paused(void)
{
    return s_pause_depth > 0;
}

esp_err_t wake_service_notify_detected(const char *source, const char *phrase)
{
    if (!s_initialized) {
        esp_err_t init_err = wake_service_init();
        if (init_err != ESP_OK) {
            return init_err;
        }
    }

    const char *src = (source && source[0]) ? source : "unknown";
    const char *text = phrase ? phrase : "";
    TickType_t now_tick = xTaskGetTickCount();

    if (!wake_lock_take(pdMS_TO_TICKS(100))) {
        return ESP_ERR_TIMEOUT;
    }

    if (!s_enabled) {
        s_stats.suppressed_disabled++;
        wake_lock_give();
        return ESP_ERR_INVALID_STATE;
    }

    if (s_pause_depth > 0) {
        s_stats.suppressed_paused++;
        wake_lock_give();
        return ESP_ERR_INVALID_STATE;
    }

    if (s_last_wake_tick != 0) {
        uint32_t elapsed_ms = (uint32_t)pdTICKS_TO_MS(now_tick - s_last_wake_tick);
        if (elapsed_ms < s_cooldown_ms) {
            s_stats.suppressed_cooldown++;
            wake_lock_give();
            return ESP_ERR_TIMEOUT;
        }
    }

    s_last_wake_tick = now_tick;
    s_stats.wake_detected++;
    s_stats.last_wake_age_ms = 0;
    snprintf(s_stats.last_source, sizeof(s_stats.last_source), "%s", src);
    snprintf(s_stats.last_phrase, sizeof(s_stats.last_phrase), "%s", text);

    wake_service_cb_t cb = s_callback;
    void *cb_ctx = s_callback_ctx;
    wake_lock_give();

    ESP_LOGI(TAG, "Wake detected from %s: %.48s", src, text);

    if (cb) {
        wake_event_t event = {
            .ts_ms = (uint64_t)(esp_timer_get_time() / 1000ULL),
        };
        snprintf(event.source, sizeof(event.source), "%s", src);
        snprintf(event.phrase, sizeof(event.phrase), "%s", text);
        cb(&event, cb_ctx);

        if (wake_lock_take(pdMS_TO_TICKS(100))) {
            s_stats.callbacks_fired++;
            wake_lock_give();
        }
    }

    return ESP_OK;
}

esp_err_t wake_service_set_callback(wake_service_cb_t cb, void *ctx)
{
    if (!s_initialized) {
        esp_err_t init_err = wake_service_init();
        if (init_err != ESP_OK) {
            return init_err;
        }
    }

    if (!wake_lock_take(pdMS_TO_TICKS(100))) {
        return ESP_ERR_TIMEOUT;
    }

    s_callback = cb;
    s_callback_ctx = ctx;
    wake_lock_give();
    return ESP_OK;
}

esp_err_t wake_service_get_stats(wake_service_stats_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        esp_err_t init_err = wake_service_init();
        if (init_err != ESP_OK) {
            return init_err;
        }
    }

    if (!wake_lock_take(pdMS_TO_TICKS(100))) {
        return ESP_ERR_TIMEOUT;
    }

    *out = s_stats;
    out->initialized = s_initialized;
    out->enabled = s_enabled;
    out->paused = (s_pause_depth > 0);
    out->pause_depth = s_pause_depth;
    out->cooldown_ms = s_cooldown_ms;

    if (s_last_wake_tick != 0) {
        TickType_t now_tick = xTaskGetTickCount();
        out->last_wake_age_ms = (uint32_t)pdTICKS_TO_MS(now_tick - s_last_wake_tick);
    }

    wake_lock_give();
    return ESP_OK;
}
