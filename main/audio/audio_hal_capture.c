#include "audio/audio_hal_capture.h"

#include <string.h>

#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "mimi_config.h"

static const char *TAG = "audio_rx";

static SemaphoreHandle_t s_lock = NULL;
static i2s_chan_handle_t s_rx = NULL;
static TaskHandle_t s_read_task = NULL;
static audio_ring_buffer_t s_ring = {0};
static bool s_initialized = false;
static bool s_enabled = false;
static bool s_running = false;
static uint32_t s_sequence = 0;
static audio_hal_capture_stats_t s_stats = {0};

static bool capture_lock_take(TickType_t ticks)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (!s_lock) {
        return false;
    }
    return xSemaphoreTake(s_lock, ticks) == pdTRUE;
}

static void capture_lock_give(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static bool capture_running(void)
{
    bool running = false;
    if (!capture_lock_take(pdMS_TO_TICKS(20))) {
        return false;
    }
    running = s_running;
    capture_lock_give();
    return running;
}

static bool load_capture_enabled_from_nvs(void)
{
    bool enabled = (MIMI_MIC_CAPTURE_ENABLED_DEFAULT != 0);

    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_VOICE, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t value = 0;
        if (nvs_get_u8(nvs, MIMI_NVS_KEY_MIC_CAPTURE_ENABLED, &value) == ESP_OK) {
            enabled = (value != 0);
        }
        nvs_close(nvs);
    }

    return enabled;
}

static void capture_read_task(void *arg)
{
    (void)arg;

    int16_t pcm_buf[MIMI_AUDIO_FRAME_MAX_SAMPLES] = {0};
    const size_t requested_bytes = (size_t)MIMI_MIC_FRAME_SAMPLES * sizeof(int16_t);

    while (capture_running()) {
        size_t bytes_read = 0;
        int64_t t0 = esp_timer_get_time();
        esp_err_t err = i2s_channel_read(s_rx,
                                         pcm_buf,
                                         requested_bytes,
                                         &bytes_read,
                                         pdMS_TO_TICKS(200));
        uint32_t latency_us = (uint32_t)(esp_timer_get_time() - t0);

        if (capture_lock_take(pdMS_TO_TICKS(20))) {
            if (s_stats.avg_read_latency_us == 0) {
                s_stats.avg_read_latency_us = latency_us;
            } else {
                s_stats.avg_read_latency_us =
                    ((s_stats.avg_read_latency_us * 7U) + latency_us) / 8U;
            }
            capture_lock_give();
        }

        if (err != ESP_OK) {
            if (capture_lock_take(pdMS_TO_TICKS(20))) {
                s_stats.read_fail++;
                capture_lock_give();
            }
            continue;
        }

        if (bytes_read == 0) {
            continue;
        }

        size_t max_bytes = sizeof(pcm_buf);
        if (bytes_read > max_bytes) {
            bytes_read = max_bytes;
            if (capture_lock_take(pdMS_TO_TICKS(20))) {
                s_stats.overflow_events++;
                capture_lock_give();
            }
        }

        audio_frame_t frame = {0};
        frame.ts_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
        frame.sequence = s_sequence++;
        frame.sample_rate_hz = MIMI_MIC_SAMPLE_RATE_HZ;
        frame.channels = 1;
        frame.bits_per_sample = 16;
        frame.data_bytes = (uint16_t)bytes_read;
        frame.samples_per_channel = (uint16_t)(bytes_read / sizeof(int16_t));
        memcpy(frame.pcm, pcm_buf, bytes_read);

        esp_err_t push_err = audio_ring_buffer_push(&s_ring, &frame, 0);

        if (capture_lock_take(pdMS_TO_TICKS(20))) {
            if (push_err == ESP_OK) {
                s_stats.read_ok++;
            } else {
                s_stats.ring_drop++;
            }
            capture_lock_give();
        }
    }

    if (capture_lock_take(pdMS_TO_TICKS(100))) {
        s_read_task = NULL;
        capture_lock_give();
    }

    vTaskDelete(NULL);
}

esp_err_t audio_hal_capture_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    if (!capture_lock_take(pdMS_TO_TICKS(100))) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_initialized) {
        capture_lock_give();
        return ESP_OK;
    }

    memset(&s_stats, 0, sizeof(s_stats));
    s_enabled = load_capture_enabled_from_nvs();
    s_stats.enabled = s_enabled;
    s_stats.sample_rate_hz = MIMI_MIC_SAMPLE_RATE_HZ;
    s_stats.frame_samples = MIMI_MIC_FRAME_SAMPLES;

    if (!s_enabled) {
        s_initialized = true;
        s_stats.initialized = true;
        capture_lock_give();
        ESP_LOGI(TAG, "Mic capture disabled (set %s in %s to enable)",
                 MIMI_NVS_KEY_MIC_CAPTURE_ENABLED,
                 MIMI_NVS_VOICE);
        return ESP_OK;
    }

#if (MIMI_MIC_I2S_WS_GPIO < 0) || (MIMI_MIC_I2S_BCLK_GPIO < 0) || (MIMI_MIC_I2S_DIN_GPIO < 0)
    s_enabled = false;
    s_stats.enabled = false;
    s_initialized = true;
    s_stats.initialized = true;
    capture_lock_give();
    ESP_LOGW(TAG, "Mic capture disabled: configure MIMI_MIC_I2S_* GPIO pins first");
    return ESP_OK;
#endif

    esp_err_t err = audio_ring_buffer_init(&s_ring, MIMI_MIC_RING_DEPTH);
    if (err != ESP_OK) {
        capture_lock_give();
        return err;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    err = i2s_new_channel(&chan_cfg, NULL, &s_rx);
    if (err != ESP_OK) {
        audio_ring_buffer_deinit(&s_ring);
        capture_lock_give();
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIMI_MIC_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIMI_MIC_I2S_BCLK_GPIO,
            .ws = MIMI_MIC_I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = MIMI_MIC_I2S_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(s_rx, &std_cfg);
    if (err != ESP_OK) {
        i2s_del_channel(s_rx);
        s_rx = NULL;
        audio_ring_buffer_deinit(&s_ring);
        capture_lock_give();
        return err;
    }

    s_initialized = true;
    s_stats.initialized = true;
    capture_lock_give();

    ESP_LOGI(TAG, "Mic capture initialized (WS=%d BCLK=%d DIN=%d, %u Hz, %u samples/frame)",
             MIMI_MIC_I2S_WS_GPIO,
             MIMI_MIC_I2S_BCLK_GPIO,
             MIMI_MIC_I2S_DIN_GPIO,
             (unsigned)MIMI_MIC_SAMPLE_RATE_HZ,
             (unsigned)MIMI_MIC_FRAME_SAMPLES);
    return ESP_OK;
}

esp_err_t audio_hal_capture_start(void)
{
    esp_err_t err = audio_hal_capture_init();
    if (err != ESP_OK) {
        return err;
    }

    if (!s_enabled) {
        return ESP_OK;
    }

    if (!capture_lock_take(pdMS_TO_TICKS(100))) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_running) {
        capture_lock_give();
        return ESP_OK;
    }

    err = i2s_channel_enable(s_rx);
    if (err != ESP_OK) {
        capture_lock_give();
        return err;
    }

    s_running = true;
    s_stats.running = true;
    audio_ring_buffer_reset(&s_ring);

    if (xTaskCreatePinnedToCore(capture_read_task,
                                "audio_capture",
                                MIMI_AUDIO_CAPTURE_STACK,
                                NULL,
                                MIMI_AUDIO_CAPTURE_PRIO,
                                &s_read_task,
                                MIMI_AUDIO_CAPTURE_CORE) != pdPASS) {
        s_running = false;
        s_stats.running = false;
        i2s_channel_disable(s_rx);
        capture_lock_give();
        return ESP_FAIL;
    }

    capture_lock_give();
    ESP_LOGI(TAG, "Mic capture started");
    return ESP_OK;
}

esp_err_t audio_hal_capture_stop(void)
{
    if (!s_enabled) {
        return ESP_OK;
    }

    if (!capture_lock_take(pdMS_TO_TICKS(100))) {
        return ESP_ERR_TIMEOUT;
    }

    if (!s_running) {
        capture_lock_give();
        return ESP_OK;
    }

    s_running = false;
    s_stats.running = false;
    capture_lock_give();

    for (int i = 0; i < 10; i++) {
        if (!s_read_task) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    esp_err_t err = i2s_channel_disable(s_rx);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    ESP_LOGI(TAG, "Mic capture stopped");
    return ESP_OK;
}

bool audio_hal_capture_is_running(void)
{
    return s_running;
}

esp_err_t audio_hal_capture_read_frame(audio_frame_t *out, TickType_t timeout_ticks)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_enabled) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return audio_ring_buffer_pop(&s_ring, out, timeout_ticks);
}

esp_err_t audio_hal_capture_get_stats(audio_hal_capture_stats_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!capture_lock_take(pdMS_TO_TICKS(100))) {
        return ESP_ERR_TIMEOUT;
    }

    *out = s_stats;
    out->initialized = s_initialized;
    out->enabled = s_enabled;
    out->running = s_running;
    capture_lock_give();

    if (s_enabled) {
        audio_ring_buffer_get_stats(&s_ring, &out->ring);
    } else {
        memset(&out->ring, 0, sizeof(out->ring));
    }

    return ESP_OK;
}
