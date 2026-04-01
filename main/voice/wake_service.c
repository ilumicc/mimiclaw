#include "voice/wake_service.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "audio/audio_hal_capture.h"
#include "mimi_config.h"
#include "voice/voice_session_coordinator.h"

static const char *TAG = "wake";

static SemaphoreHandle_t s_lock = NULL;
static bool s_initialized = false;
static bool s_enabled = (MIMI_WAKE_ENABLED_DEFAULT != 0);
static bool s_local_detector_enabled = (MIMI_WAKE_LOCAL_DETECT_DEFAULT != 0);
static uint16_t s_rms_threshold = MIMI_WAKE_RMS_THRESHOLD_DEFAULT;
static uint8_t s_rms_consec_needed = MIMI_WAKE_RMS_CONSEC_FRAMES_DEFAULT;
static uint8_t s_rms_consec_hits = 0;

static uint32_t s_cooldown_ms = MIMI_WAKE_COOLDOWN_MS;
static uint32_t s_pause_depth = 0;
static TickType_t s_last_wake_tick = 0;
static wake_service_cb_t s_callback = NULL;
static void *s_callback_ctx = NULL;
static TaskHandle_t s_local_task = NULL;

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

static esp_err_t wake_save_u8(const char *key, uint8_t value)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MIMI_NVS_VOICE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(nvs, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static esp_err_t wake_save_u16(const char *key, uint16_t value)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MIMI_NVS_VOICE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u16(nvs, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static esp_err_t wake_save_enabled(bool enabled)
{
    return wake_save_u8(MIMI_NVS_KEY_WAKE_ENABLED, enabled ? 1 : 0);
}

static esp_err_t wake_save_local_enabled(bool enabled)
{
    return wake_save_u8(MIMI_NVS_KEY_WAKE_LOCAL_ENABLED, enabled ? 1 : 0);
}

static esp_err_t wake_save_cooldown_ms(uint32_t cooldown_ms)
{
    uint16_t value = (cooldown_ms > 65535U) ? 65535U : (uint16_t)cooldown_ms;
    return wake_save_u16(MIMI_NVS_KEY_WAKE_COOLDOWN_MS, value);
}

static esp_err_t wake_save_rms_threshold(uint16_t threshold)
{
    return wake_save_u16(MIMI_NVS_KEY_WAKE_RMS_THRESHOLD, threshold);
}

static void load_wake_runtime_config(void)
{
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_VOICE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    uint8_t u8 = 0;
    if (nvs_get_u8(nvs, MIMI_NVS_KEY_WAKE_ENABLED, &u8) == ESP_OK) {
        s_enabled = (u8 != 0);
    }
    if (nvs_get_u8(nvs, MIMI_NVS_KEY_WAKE_LOCAL_ENABLED, &u8) == ESP_OK) {
        s_local_detector_enabled = (u8 != 0);
    }

    uint16_t u16 = 0;
    if (nvs_get_u16(nvs, MIMI_NVS_KEY_WAKE_COOLDOWN_MS, &u16) == ESP_OK && u16 > 0) {
        s_cooldown_ms = u16;
    }
    if (nvs_get_u16(nvs, MIMI_NVS_KEY_WAKE_RMS_THRESHOLD, &u16) == ESP_OK && u16 > 0) {
        s_rms_threshold = u16;
    }

    nvs_close(nvs);
}

static uint32_t audio_mean_abs(const audio_frame_t *frame)
{
    if (!frame || frame->samples_per_channel == 0) {
        return 0;
    }

    uint64_t sum = 0;
    uint16_t count = frame->samples_per_channel;
    if (count > MIMI_AUDIO_FRAME_MAX_SAMPLES) {
        count = MIMI_AUDIO_FRAME_MAX_SAMPLES;
    }

    for (uint16_t i = 0; i < count; i++) {
        int32_t v = frame->pcm[i];
        if (v < 0) {
            v = -v;
        }
        sum += (uint32_t)v;
    }

    return (uint32_t)(sum / count);
}

esp_err_t wake_service_on_audio_frame(const audio_frame_t *frame)
{
    if (!frame) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_initialized) {
        esp_err_t init_err = wake_service_init();
        if (init_err != ESP_OK) {
            return init_err;
        }
    }

    voice_session_coordinator_on_audio_frame(frame);

    bool enabled = false;
    bool paused = false;
    bool local_enabled = false;
    uint16_t threshold = 0;
    uint8_t consec_needed = 0;

    if (!wake_lock_take(pdMS_TO_TICKS(20))) {
        return ESP_ERR_TIMEOUT;
    }

    s_stats.audio_frames_processed++;
    enabled = s_enabled;
    paused = (s_pause_depth > 0);
    local_enabled = s_local_detector_enabled;
    threshold = s_rms_threshold;
    consec_needed = s_rms_consec_needed;

    wake_lock_give();

    if (!enabled || paused || !local_enabled) {
        return ESP_OK;
    }

    uint32_t mean_abs = audio_mean_abs(frame);
    bool trigger = false;

    if (!wake_lock_take(pdMS_TO_TICKS(20))) {
        return ESP_ERR_TIMEOUT;
    }

    if (mean_abs >= threshold) {
        if (s_rms_consec_hits < 255) {
            s_rms_consec_hits++;
        }
    } else {
        s_rms_consec_hits = 0;
    }

    if (s_rms_consec_hits >= consec_needed) {
        s_rms_consec_hits = 0;
        s_stats.local_rms_hits++;
        trigger = true;
    }

    wake_lock_give();

    if (trigger) {
        return wake_service_notify_detected("mic_local", "energy_gate");
    }

    return ESP_OK;
}

static void wake_local_task(void *arg)
{
    (void)arg;

    while (1) {
        audio_frame_t frame = {0};
        esp_err_t err = audio_hal_capture_read_frame(&frame, pdMS_TO_TICKS(250));
        if (err == ESP_OK) {
            wake_service_on_audio_frame(&frame);
        } else if (err == ESP_ERR_NOT_SUPPORTED) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
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
    s_local_detector_enabled = (MIMI_WAKE_LOCAL_DETECT_DEFAULT != 0);
    s_rms_threshold = MIMI_WAKE_RMS_THRESHOLD_DEFAULT;
    s_rms_consec_needed = MIMI_WAKE_RMS_CONSEC_FRAMES_DEFAULT;
    s_cooldown_ms = MIMI_WAKE_COOLDOWN_MS;

    load_wake_runtime_config();

    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.initialized = true;
    s_stats.enabled = s_enabled;
    s_stats.local_detector_enabled = s_local_detector_enabled;
    s_stats.cooldown_ms = s_cooldown_ms;
    s_stats.local_rms_threshold = s_rms_threshold;

    s_initialized = true;
    wake_lock_give();

    if (!s_local_task) {
        if (xTaskCreatePinnedToCore(wake_local_task,
                                    "wake_local",
                                    MIMI_VOICE_SESSION_STACK,
                                    NULL,
                                    MIMI_VOICE_SESSION_PRIO,
                                    &s_local_task,
                                    MIMI_VOICE_SESSION_CORE) != pdPASS) {
            ESP_LOGW(TAG, "Local wake task not started");
        }
    }

    ESP_LOGI(TAG, "Wake service initialized: enabled=%s cooldown=%u ms local=%s threshold=%u",
             s_enabled ? "yes" : "no",
             (unsigned)s_cooldown_ms,
             s_local_detector_enabled ? "yes" : "no",
             (unsigned)s_rms_threshold);
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

esp_err_t wake_service_set_local_detector_enabled(bool enabled)
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

    s_local_detector_enabled = enabled;
    s_stats.local_detector_enabled = enabled;
    s_rms_consec_hits = 0;
    wake_lock_give();

    return wake_save_local_enabled(enabled);
}

esp_err_t wake_service_set_rms_threshold(uint16_t threshold)
{
    if (threshold == 0) {
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

    s_rms_threshold = threshold;
    s_stats.local_rms_threshold = threshold;
    wake_lock_give();

    return wake_save_rms_threshold(threshold);
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
    out->local_detector_enabled = s_local_detector_enabled;
    out->local_rms_threshold = s_rms_threshold;

    if (s_last_wake_tick != 0) {
        TickType_t now_tick = xTaskGetTickCount();
        out->last_wake_age_ms = (uint32_t)pdTICKS_TO_MS(now_tick - s_last_wake_tick);
    }

    wake_lock_give();
    return ESP_OK;
}
