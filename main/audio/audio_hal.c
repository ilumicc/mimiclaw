#include "audio/audio_hal.h"

#if MIMI_VOICE_ENABLED

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_common.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "audio_hal";

#define AUDIO_HAL_MIC_SAMPLE_RATE_HZ          16000
#define AUDIO_HAL_SPK_DEFAULT_SAMPLE_RATE_HZ  16000
#define AUDIO_HAL_MIC_DMA_DESC_NUM            6
#define AUDIO_HAL_MIC_DMA_FRAME_NUM           256
#define AUDIO_HAL_SPK_DMA_DESC_NUM            6
#define AUDIO_HAL_SPK_DMA_FRAME_NUM           256
#define AUDIO_HAL_MIC_RAW_CHUNK_SAMPLES       512
#define AUDIO_HAL_SPK_CHUNK_SAMPLES           512

static i2s_chan_handle_t s_i2s_mic_rx = NULL;
static i2s_chan_handle_t s_i2s_spk_tx = NULL;
static int32_t *s_mic_raw_chunk = NULL;
static int16_t *s_spk_chunk = NULL;
static bool s_initialized = false;

static inline size_t min_size(size_t a, size_t b)
{
    return a < b ? a : b;
}

static void audio_hal_release_resources(void)
{
    if (s_i2s_mic_rx) {
        i2s_channel_disable(s_i2s_mic_rx);
        i2s_del_channel(s_i2s_mic_rx);
        s_i2s_mic_rx = NULL;
    }

    if (s_i2s_spk_tx) {
        i2s_channel_disable(s_i2s_spk_tx);
        i2s_del_channel(s_i2s_spk_tx);
        s_i2s_spk_tx = NULL;
    }

    if (s_mic_raw_chunk) {
        free(s_mic_raw_chunk);
        s_mic_raw_chunk = NULL;
    }

    if (s_spk_chunk) {
        free(s_spk_chunk);
        s_spk_chunk = NULL;
    }

    gpio_set_level(MIMI_I2S_SPK_SD_GPIO, 0);
    gpio_reset_pin(MIMI_I2S_SPK_SD_GPIO);

    s_initialized = false;
}

esp_err_t audio_hal_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t ret;

    gpio_config_t sd_cfg = {
        .pin_bit_mask = (1ULL << MIMI_I2S_SPK_SD_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&sd_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MAX98357A SD pin config failed: %s", esp_err_to_name(ret));
        return ret;
    }
    gpio_set_level(MIMI_I2S_SPK_SD_GPIO, 0);

    s_mic_raw_chunk = heap_caps_malloc(AUDIO_HAL_MIC_RAW_CHUNK_SAMPLES * sizeof(int32_t),
                                       MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_spk_chunk = heap_caps_malloc(AUDIO_HAL_SPK_CHUNK_SAMPLES * sizeof(int16_t),
                                   MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_mic_raw_chunk || !s_spk_chunk) {
        ESP_LOGE(TAG, "Failed to allocate DMA-capable audio buffers");
        audio_hal_release_resources();
        return ESP_ERR_NO_MEM;
    }

    i2s_chan_config_t mic_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    mic_chan_cfg.dma_desc_num = AUDIO_HAL_MIC_DMA_DESC_NUM;
    mic_chan_cfg.dma_frame_num = AUDIO_HAL_MIC_DMA_FRAME_NUM;
    ret = i2s_new_channel(&mic_chan_cfg, NULL, &s_i2s_mic_rx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S0 RX channel: %s", esp_err_to_name(ret));
        audio_hal_release_resources();
        return ret;
    }

    i2s_std_slot_config_t mic_slot_cfg =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
    mic_slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    i2s_std_config_t mic_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_HAL_MIC_SAMPLE_RATE_HZ),
        .slot_cfg = mic_slot_cfg,
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIMI_I2S_MIC_BCLK_GPIO,
            .ws = MIMI_I2S_MIC_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = MIMI_I2S_MIC_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(s_i2s_mic_rx, &mic_std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S0 RX std mode: %s", esp_err_to_name(ret));
        audio_hal_release_resources();
        return ret;
    }

    ret = i2s_channel_enable(s_i2s_mic_rx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S0 RX channel: %s", esp_err_to_name(ret));
        audio_hal_release_resources();
        return ret;
    }

    i2s_chan_config_t spk_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    spk_chan_cfg.dma_desc_num = AUDIO_HAL_SPK_DMA_DESC_NUM;
    spk_chan_cfg.dma_frame_num = AUDIO_HAL_SPK_DMA_FRAME_NUM;
    ret = i2s_new_channel(&spk_chan_cfg, &s_i2s_spk_tx, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S1 TX channel: %s", esp_err_to_name(ret));
        audio_hal_release_resources();
        return ret;
    }

    i2s_std_slot_config_t spk_slot_cfg =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    spk_slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    i2s_std_config_t spk_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_HAL_SPK_DEFAULT_SAMPLE_RATE_HZ),
        .slot_cfg = spk_slot_cfg,
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIMI_I2S_SPK_BCLK_GPIO,
            .ws = MIMI_I2S_SPK_WS_GPIO,
            .dout = MIMI_I2S_SPK_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(s_i2s_spk_tx, &spk_std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S1 TX std mode: %s", esp_err_to_name(ret));
        audio_hal_release_resources();
        return ret;
    }

    ret = i2s_channel_enable(s_i2s_spk_tx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S1 TX channel: %s", esp_err_to_name(ret));
        audio_hal_release_resources();
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Audio HAL initialized (I2S0 RX 16kHz/32-bit, I2S1 TX 16-bit)");
    return ESP_OK;
}

size_t audio_hal_mic_read(int16_t *pcm_out, size_t max_samples, uint32_t timeout_ms)
{
    if (!s_initialized || !pcm_out || max_samples == 0 || !s_i2s_mic_rx) {
        return 0;
    }

    size_t total_samples = 0;

    while (total_samples < max_samples) {
        size_t chunk_samples = min_size(max_samples - total_samples, AUDIO_HAL_MIC_RAW_CHUNK_SAMPLES);
        size_t bytes_read = 0;

        esp_err_t ret = i2s_channel_read(s_i2s_mic_rx,
                                         s_mic_raw_chunk,
                                         chunk_samples * sizeof(int32_t),
                                         &bytes_read,
                                         timeout_ms);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Mic read failed: %s", esp_err_to_name(ret));
            break;
        }

        size_t got_samples = bytes_read / sizeof(int32_t);
        for (size_t i = 0; i < got_samples; i++) {
            int32_t sample = s_mic_raw_chunk[i] >> 14;
            if (sample > INT16_MAX) {
                sample = INT16_MAX;
            } else if (sample < INT16_MIN) {
                sample = INT16_MIN;
            }
            pcm_out[total_samples + i] = (int16_t)sample;
        }

        total_samples += got_samples;
        if (got_samples < chunk_samples) {
            break;
        }
    }

    return total_samples;
}

esp_err_t audio_hal_spk_write(const int16_t *pcm, size_t samples, size_t *samples_written, uint32_t timeout_ms)
{
    if (samples_written) {
        *samples_written = 0;
    }

    if (!s_initialized || !s_i2s_spk_tx) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!pcm || samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = ESP_OK;
    size_t total_written = 0;

    while (total_written < samples) {
        size_t chunk_samples = min_size(samples - total_written, AUDIO_HAL_SPK_CHUNK_SAMPLES);
        memcpy(s_spk_chunk, pcm + total_written, chunk_samples * sizeof(int16_t));

        size_t bytes_written = 0;
        esp_err_t ret = i2s_channel_write(s_i2s_spk_tx,
                                          s_spk_chunk,
                                          chunk_samples * sizeof(int16_t),
                                          &bytes_written,
                                          timeout_ms);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Speaker write failed: %s", esp_err_to_name(ret));
            result = ret;
            break;
        }

        size_t written_samples = bytes_written / sizeof(int16_t);
        total_written += written_samples;

        if (written_samples < chunk_samples) {
            break;
        }
    }

    if (samples_written) {
        *samples_written = total_written;
    }
    return result;
}

esp_err_t audio_hal_spk_set_sample_rate(uint32_t sample_rate_hz)
{
    if (!s_initialized || !s_i2s_spk_tx) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sample_rate_hz == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    bool was_running = true;
    esp_err_t ret = i2s_channel_disable(s_i2s_spk_tx);
    if (ret == ESP_ERR_INVALID_STATE) {
        was_running = false;
    } else if (ret != ESP_OK) {
        return ret;
    }

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz);
    ret = i2s_channel_reconfig_std_clock(s_i2s_spk_tx, &clk_cfg);

    if (was_running) {
        esp_err_t en_ret = i2s_channel_enable(s_i2s_spk_tx);
        if (ret == ESP_OK) {
            ret = en_ret;
        }
    }

    return ret;
}

esp_err_t audio_hal_spk_enable(bool enable)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return gpio_set_level(MIMI_I2S_SPK_SD_GPIO, enable ? 1 : 0);
}

esp_err_t audio_hal_deinit(void)
{
    if (!s_initialized && !s_i2s_mic_rx && !s_i2s_spk_tx) {
        return ESP_OK;
    }

    audio_hal_release_resources();
    ESP_LOGI(TAG, "Audio HAL deinitialized");
    return ESP_OK;
}

#endif
