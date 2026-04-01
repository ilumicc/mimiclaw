#include "audio/speaker_i2s.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"

#include "mimi_config.h"

static const char *TAG = "speaker";
#define SPEAKER_DEFAULT_SAMPLE_RATE 24000U
#define SPEAKER_PROGRESS_LOG_STEP    32768U


typedef struct {
    uint16_t audio_format;
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    size_t data_offset;
    size_t data_len;
} wav_meta_t;

static i2s_chan_handle_t s_tx = NULL;
static bool s_initialized = false;
static bool s_channel_enabled = false;
static volatile bool s_abort_requested = false;
static SemaphoreHandle_t s_lock = NULL;
static speaker_i2s_stats_t s_stats = {0};

static void speaker_set_sd(bool enabled)
{
#if MIMI_SPK_SD_GPIO >= 0
    gpio_set_level((gpio_num_t)MIMI_SPK_SD_GPIO,
                   enabled ? MIMI_SPK_SD_ACTIVE_LEVEL : (1 - MIMI_SPK_SD_ACTIVE_LEVEL));
#else
    (void)enabled;
#endif
}

static SemaphoreHandle_t speaker_get_lock(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    return s_lock;
}

static bool speaker_lock_take(TickType_t ticks)
{
    SemaphoreHandle_t lock = speaker_get_lock();
    if (!lock) {
        return false;
    }
    return xSemaphoreTake(lock, ticks) == pdTRUE;
}

static void speaker_lock_give(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}


static uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static esp_err_t parse_wav_header(const uint8_t *wav, size_t len, wav_meta_t *meta)
{
    if (!wav || !meta || len < 44) {
        return ESP_ERR_INVALID_ARG;
    }

    if (memcmp(wav, "RIFF", 4) != 0 || memcmp(wav + 8, "WAVE", 4) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(meta, 0, sizeof(*meta));

    size_t pos = 12;
    bool have_fmt = false;
    bool have_data = false;

    while (pos + 8 <= len) {
        const uint8_t *chunk = wav + pos;
        uint32_t chunk_size = rd_le32(chunk + 4);
        size_t payload = pos + 8;
        uint64_t next64 = (uint64_t)payload + (uint64_t)chunk_size;
        bool chunk_fits = next64 <= (uint64_t)len;

        if (memcmp(chunk, "fmt ", 4) == 0) {
            if (!chunk_fits) {
                return ESP_ERR_INVALID_SIZE;
            }
            if (chunk_size < 16) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            meta->audio_format = rd_le16(wav + payload + 0);
            meta->channels = rd_le16(wav + payload + 2);
            meta->sample_rate = rd_le32(wav + payload + 4);
            meta->bits_per_sample = rd_le16(wav + payload + 14);
            have_fmt = true;
        } else if (memcmp(chunk, "data", 4) == 0) {
            if (payload >= len) {
                return ESP_ERR_INVALID_SIZE;
            }

            meta->data_offset = payload;
            if (chunk_size == 0xFFFFFFFFU || !chunk_fits) {
                meta->data_len = len - payload;
                ESP_LOGW(TAG, "WAV data chunk size=%u, clamped to %u bytes",
                         (unsigned)chunk_size,
                         (unsigned)meta->data_len);
            } else {
                meta->data_len = (size_t)chunk_size;
            }
            have_data = true;
        }

        if (have_fmt && have_data) {
            break;
        }

        if (!chunk_fits) {
            return ESP_ERR_INVALID_SIZE;
        }

        size_t next = (size_t)next64;
        pos = next + (chunk_size % 2);
    }

    if (!have_fmt || !have_data) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (meta->channels < 1 || meta->channels > 2) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (meta->sample_rate == 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

esp_err_t speaker_i2s_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SPEAKER_DEFAULT_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIMI_SPK_I2S_BCLK_GPIO,
            .ws = MIMI_SPK_I2S_WS_GPIO,
            .dout = MIMI_SPK_I2S_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_tx);
        s_tx = NULL;
        return err;
    }

#if MIMI_SPK_SD_GPIO >= 0
    gpio_reset_pin((gpio_num_t)MIMI_SPK_SD_GPIO);
    gpio_set_direction((gpio_num_t)MIMI_SPK_SD_GPIO, GPIO_MODE_OUTPUT);
    speaker_set_sd(false);
#endif

    s_initialized = true;
    s_channel_enabled = false;
    ESP_LOGI(TAG, "I2S speaker ready (WS=%d BCLK=%d DOUT=%d SD=%d attn=%dx)",
             MIMI_SPK_I2S_WS_GPIO,
             MIMI_SPK_I2S_BCLK_GPIO,
             MIMI_SPK_I2S_DOUT_GPIO,
             MIMI_SPK_SD_GPIO,
             MIMI_SPK_PCM_ATTENUATION);
    return ESP_OK;
}

bool speaker_i2s_is_initialized(void)
{
    return s_initialized;
}

static esp_err_t speaker_reconfig_clock(uint32_t sample_rate)
{
    esp_err_t err;
    if (s_channel_enabled) {
        err = i2s_channel_disable(s_tx);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "i2s_channel_disable before reconfig failed: %s", esp_err_to_name(err));
            return err;
        }
        s_channel_enabled = false;
    }

    if (sample_rate != SPEAKER_DEFAULT_SAMPLE_RATE) {
        ESP_LOGI(TAG, "Reconfig I2S clock to %u Hz", (unsigned)sample_rate);
        i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
        err = i2s_channel_reconfig_std_clock(s_tx, &clk_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_reconfig_std_clock failed: %s", esp_err_to_name(err));
            return err;
        }
    } else {
        ESP_LOGI(TAG, "Skip clock reconfig (default %u Hz)", (unsigned)sample_rate);
    }

    err = i2s_channel_enable(s_tx);
    if (err == ESP_OK) {
        s_channel_enabled = true;
        return ESP_OK;
    }

    ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(err));
    return err;
}

static inline int16_t speaker_attenuate_sample(int16_t sample)
{
#if MIMI_SPK_PCM_ATTENUATION <= 1
    return sample;
#else
    return (int16_t)(sample / MIMI_SPK_PCM_ATTENUATION);
#endif
}

static bool speaker_should_abort(void)
{
    return s_abort_requested;
}

static esp_err_t speaker_check_deadline(int64_t deadline_us, const char *stage, size_t done, size_t total)
{
    if (deadline_us <= 0) {
        return ESP_OK;
    }

    if (esp_timer_get_time() <= deadline_us) {
        return ESP_OK;
    }

    s_stats.write_timeouts++;
    ESP_LOGE(TAG, "Playback deadline exceeded at %s (%u/%u bytes)",
             stage,
             (unsigned)done,
             (unsigned)total);
    return ESP_ERR_TIMEOUT;
}



static esp_err_t write_pcm_stereo(const uint8_t *data, size_t len, int64_t deadline_us)
{
#if MIMI_SPK_PCM_ATTENUATION <= 1
    size_t written = 0;
    size_t next_log = SPEAKER_PROGRESS_LOG_STEP;
    while (written < len) {
        if (speaker_should_abort()) {
            return ESP_ERR_INVALID_STATE;
        }

        esp_err_t dl_err = speaker_check_deadline(deadline_us, "stereo", written, len);
        if (dl_err != ESP_OK) {
            return dl_err;
        }

        size_t out = 0;
        esp_err_t err = i2s_channel_write(s_tx,
                                          data + written,
                                          len - written,
                                          &out,
                                          pdMS_TO_TICKS(MIMI_SPK_I2S_WRITE_TIMEOUT_MS));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_write(stereo) failed at %u/%u: %s",
                     (unsigned)written,
                     (unsigned)len,
                     esp_err_to_name(err));
            return err;
        }
        if (out == 0) {
            s_stats.write_timeouts++;
            ESP_LOGE(TAG, "i2s_channel_write(stereo) no progress at %u/%u",
                     (unsigned)written,
                     (unsigned)len);
            return ESP_ERR_TIMEOUT;
        }
        written += out;

        if (len >= SPEAKER_PROGRESS_LOG_STEP && (written >= next_log || written == len)) {
            ESP_LOGI(TAG, "PCM stereo progress %u/%u", (unsigned)written, (unsigned)len);
            next_log += SPEAKER_PROGRESS_LOG_STEP;
        }
    }
    return ESP_OK;
#else
    const size_t chunk_bytes = 1024;
    int16_t *scaled = malloc(chunk_bytes);
    if (!scaled) {
        return ESP_ERR_NO_MEM;
    }

    size_t offset = 0;
    size_t next_log = SPEAKER_PROGRESS_LOG_STEP;
    while (offset < len) {
        if (speaker_should_abort()) {
            free(scaled);
            return ESP_ERR_INVALID_STATE;
        }

        esp_err_t dl_err = speaker_check_deadline(deadline_us, "stereo_scaled", offset, len);
        if (dl_err != ESP_OK) {
            free(scaled);
            return dl_err;
        }

        size_t take = len - offset;
        if (take > chunk_bytes) {
            take = chunk_bytes;
        }
        if (take % 2) {
            take--;
        }
        if (take == 0) {
            break;
        }

        const int16_t *src = (const int16_t *)(data + offset);
        size_t samples = take / 2;
        for (size_t i = 0; i < samples; i++) {
            scaled[i] = speaker_attenuate_sample(src[i]);
        }

        size_t written = 0;
        while (written < take) {
            if (speaker_should_abort()) {
                free(scaled);
                return ESP_ERR_INVALID_STATE;
            }

            dl_err = speaker_check_deadline(deadline_us, "stereo_scaled_write", offset + written, len);
            if (dl_err != ESP_OK) {
                free(scaled);
                return dl_err;
            }

            size_t out = 0;
            esp_err_t err = i2s_channel_write(s_tx,
                                              ((const uint8_t *)scaled) + written,
                                              take - written,
                                              &out,
                                              pdMS_TO_TICKS(MIMI_SPK_I2S_WRITE_TIMEOUT_MS));
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "i2s_channel_write(stereo_scaled) failed at %u/%u: %s",
                         (unsigned)(offset + written),
                         (unsigned)len,
                         esp_err_to_name(err));
                free(scaled);
                return err;
            }
            if (out == 0) {
                s_stats.write_timeouts++;
                ESP_LOGE(TAG, "i2s_channel_write(stereo_scaled) no progress at %u/%u",
                         (unsigned)(offset + written),
                         (unsigned)len);
                free(scaled);
                return ESP_ERR_TIMEOUT;
            }
            written += out;
        }

        offset += take;
        if (len >= SPEAKER_PROGRESS_LOG_STEP && (offset >= next_log || offset == len)) {
            ESP_LOGI(TAG, "PCM stereo(scaled) progress %u/%u", (unsigned)offset, (unsigned)len);
            next_log += SPEAKER_PROGRESS_LOG_STEP;
        }
    }

    free(scaled);
    return ESP_OK;
#endif
}

static esp_err_t write_pcm_mono_dup(const uint8_t *data, size_t len, int64_t deadline_us)
{
    const size_t mono_chunk = 1024;
    int16_t *stereo = malloc(mono_chunk * 2);
    if (!stereo) {
        return ESP_ERR_NO_MEM;
    }

    size_t offset = 0;
    size_t next_log = SPEAKER_PROGRESS_LOG_STEP;
    while (offset < len) {
        if (speaker_should_abort()) {
            free(stereo);
            return ESP_ERR_INVALID_STATE;
        }

        esp_err_t dl_err = speaker_check_deadline(deadline_us, "mono_dup", offset, len);
        if (dl_err != ESP_OK) {
            free(stereo);
            return dl_err;
        }

        size_t take = len - offset;
        if (take > mono_chunk) {
            take = mono_chunk;
        }
        if (take % 2) {
            take--;
        }
        if (take == 0) {
            break;
        }

        const int16_t *mono = (const int16_t *)(data + offset);
        size_t samples = take / 2;
        for (size_t i = 0; i < samples; i++) {
            stereo[i * 2] = mono[i];
            stereo[i * 2 + 1] = mono[i];
        }

        esp_err_t err = write_pcm_stereo((const uint8_t *)stereo, samples * 4, deadline_us);
        if (err != ESP_OK) {
            free(stereo);
            return err;
        }

        offset += take;
        if (offset >= next_log || offset == len) {
            ESP_LOGI(TAG, "PCM mono progress %u/%u", (unsigned)offset, (unsigned)len);
            next_log += SPEAKER_PROGRESS_LOG_STEP;
        }
    }

    free(stereo);
    return ESP_OK;
}

static void speaker_force_gpio_idle(void)
{
    const gpio_num_t pins[] = {
        (gpio_num_t)MIMI_SPK_I2S_WS_GPIO,
        (gpio_num_t)MIMI_SPK_I2S_BCLK_GPIO,
        (gpio_num_t)MIMI_SPK_I2S_DOUT_GPIO,
    };
    for (size_t i = 0; i < (sizeof(pins) / sizeof(pins[0])); i++) {
        gpio_reset_pin(pins[i]);
        gpio_set_direction(pins[i], GPIO_MODE_OUTPUT);
        gpio_set_level(pins[i], 0);
    }

#if MIMI_SPK_SD_GPIO >= 0
    gpio_reset_pin((gpio_num_t)MIMI_SPK_SD_GPIO);
    gpio_set_direction((gpio_num_t)MIMI_SPK_SD_GPIO, GPIO_MODE_OUTPUT);
#endif
    speaker_set_sd(false);
}

static void speaker_stop_output(void)
{
    if (s_tx && s_channel_enabled) {
        uint8_t silence[512] = {0};
        size_t out = 0;
        esp_err_t wr = i2s_channel_write(s_tx, silence, sizeof(silence), &out, pdMS_TO_TICKS(200));
        if (wr != ESP_OK) {
            ESP_LOGD(TAG, "Silence flush failed: %s", esp_err_to_name(wr));
        }

        esp_err_t dis = i2s_channel_disable(s_tx);
        if (dis != ESP_OK && dis != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "i2s_channel_disable failed: %s", esp_err_to_name(dis));
        }
        s_channel_enabled = false;
    }

    if (s_tx) {
        esp_err_t del = i2s_del_channel(s_tx);
        if (del != ESP_OK) {
            ESP_LOGW(TAG, "i2s_del_channel failed: %s", esp_err_to_name(del));
        }
        s_tx = NULL;
    }

    s_initialized = false;
    s_stats.is_playing = false;
    speaker_force_gpio_idle();
}

esp_err_t speaker_i2s_play_wav(const uint8_t *wav_data, size_t wav_len)
{
    if (!wav_data || wav_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!speaker_lock_take(pdMS_TO_TICKS(200))) {
        return ESP_ERR_TIMEOUT;
    }

    s_stats.play_requests++;
    s_stats.is_playing = true;
    s_abort_requested = false;

    esp_err_t err = ESP_OK;
    wav_meta_t meta = {0};
    int64_t deadline_us = 0;

    ESP_LOGI(TAG, "Playback begin: wav_len=%u", (unsigned)wav_len);

    do {
        err = parse_wav_header(wav_data, wav_len, &meta);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Invalid WAV: %s", esp_err_to_name(err));
            break;
        }

        ESP_LOGI(TAG, "WAV meta: fmt=%u ch=%u hz=%u bits=%u data_off=%u data_len=%u",
                 (unsigned)meta.audio_format,
                 (unsigned)meta.channels,
                 (unsigned)meta.sample_rate,
                 (unsigned)meta.bits_per_sample,
                 (unsigned)meta.data_offset,
                 (unsigned)meta.data_len);

        if (meta.audio_format != 1 || meta.bits_per_sample != 16) {
            ESP_LOGE(TAG, "Unsupported WAV format: audio_format=%u bits=%u",
                     (unsigned)meta.audio_format, (unsigned)meta.bits_per_sample);
            err = ESP_ERR_NOT_SUPPORTED;
            break;
        }

        if (meta.data_offset >= wav_len || meta.data_len > (wav_len - meta.data_offset)) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }

        uint64_t bytes_per_sec = ((uint64_t)meta.sample_rate * (uint64_t)meta.channels * (uint64_t)meta.bits_per_sample) / 8ULL;
        if (bytes_per_sec == 0) {
            err = ESP_ERR_INVALID_RESPONSE;
            break;
        }

        uint64_t expected_ms = ((uint64_t)meta.data_len * 1000ULL) / bytes_per_sec;
        uint64_t timeout_ms = expected_ms * 4ULL + 3000ULL;
        if (timeout_ms < 5000ULL) {
            timeout_ms = 5000ULL;
        }
        deadline_us = esp_timer_get_time() + (int64_t)(timeout_ms * 1000ULL);

        ESP_LOGI(TAG, "Playback timing: expected=%llu ms timeout=%llu ms",
                 (unsigned long long)expected_ms,
                 (unsigned long long)timeout_ms);

        err = speaker_i2s_init();
        if (err != ESP_OK) {
            break;
        }

        s_stats.last_sample_rate = meta.sample_rate;
        s_stats.last_channels = meta.channels;
        s_stats.last_bits_per_sample = meta.bits_per_sample;
        s_stats.last_data_len = meta.data_len;

        err = speaker_reconfig_clock(meta.sample_rate);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2S clock setup failed: %s", esp_err_to_name(err));
            break;
        }

        speaker_set_sd(true);

        const uint8_t *pcm = wav_data + meta.data_offset;
        if (meta.channels == 2) {
            err = write_pcm_stereo(pcm, meta.data_len, deadline_us);
        } else {
            err = write_pcm_mono_dup(pcm, meta.data_len, deadline_us);
        }

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Playback OK (%u Hz, %u ch, %u bits, %u bytes)",
                     (unsigned)meta.sample_rate,
                     (unsigned)meta.channels,
                     (unsigned)meta.bits_per_sample,
                     (unsigned)meta.data_len);
        }
    } while (0);

    if (err == ESP_OK) {
        s_stats.play_ok++;
    } else {
        s_stats.play_fail++;
        ESP_LOGE(TAG, "I2S playback failed: %s", esp_err_to_name(err));
    }

    s_stats.last_error = err;
    speaker_stop_output();
    s_abort_requested = false;
    speaker_lock_give();
    return err;
}

esp_err_t speaker_i2s_request_stop(void)
{
    s_abort_requested = true;
    s_stats.abort_requests++;
    return ESP_OK;
}

esp_err_t speaker_i2s_stop(void)
{
    s_stats.stop_requests++;
    s_abort_requested = true;

    if (!speaker_lock_take(pdMS_TO_TICKS(300))) {
        return ESP_ERR_TIMEOUT;
    }

    speaker_stop_output();
    s_abort_requested = false;
    s_stats.last_error = ESP_OK;
    speaker_lock_give();
    return ESP_OK;
}

esp_err_t speaker_i2s_get_stats(speaker_i2s_stats_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!speaker_lock_take(pdMS_TO_TICKS(100))) {
        return ESP_ERR_TIMEOUT;
    }

    *out = s_stats;
    speaker_lock_give();
    return ESP_OK;
}
