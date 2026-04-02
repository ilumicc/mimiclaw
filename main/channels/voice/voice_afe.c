#include "channels/voice/voice_afe.h"

#if MIMI_VOICE_ENABLED

#include <string.h>

#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_log.h"
#include "esp_vad.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "model_path.h"

#ifndef MIMI_SECRET_WAKE_WORD
#define MIMI_SECRET_WAKE_WORD ""
#endif

#define VOICE_AFE_MODEL_PARTITION_LABEL   "model"
#define VOICE_AFE_INPUT_FORMAT            "M"
#define VOICE_AFE_VAD_MIN_SPEECH_MS       300
#define VOICE_AFE_VAD_MIN_NOISE_MS        1000

static const char *TAG = "voice_afe";

static const esp_afe_sr_iface_t *s_afe_iface = NULL;
static esp_afe_sr_data_t *s_afe_data = NULL;
static srmodel_list_t *s_models = NULL;
static afe_config_t *s_afe_cfg = NULL;
static int s_feed_chunksize = 0;
static int s_feed_channels = 0;
static bool s_initialized = false;

static char *voice_afe_select_wakenet_model(srmodel_list_t *models)
{
    if (!models) {
        return NULL;
    }

    if (MIMI_SECRET_WAKE_WORD[0] != '\0') {
        if (esp_srmodel_exists(models, (char *)MIMI_SECRET_WAKE_WORD) >= 0) {
            return (char *)MIMI_SECRET_WAKE_WORD;
        }

        char *filtered = esp_srmodel_filter(models, ESP_WN_PREFIX, MIMI_SECRET_WAKE_WORD);
        if (filtered) {
            return filtered;
        }
    }

    return esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
}

static void voice_afe_cleanup(void)
{
    if (s_afe_iface && s_afe_data) {
        s_afe_iface->destroy(s_afe_data);
    }
    s_afe_data = NULL;
    s_afe_iface = NULL;

    if (s_afe_cfg) {
        afe_config_free(s_afe_cfg);
        s_afe_cfg = NULL;
    }

    if (s_models) {
        esp_srmodel_deinit(s_models);
        s_models = NULL;
    }

    s_feed_chunksize = 0;
    s_feed_channels = 0;
    s_initialized = false;
}

esp_err_t voice_afe_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_models = esp_srmodel_init(VOICE_AFE_MODEL_PARTITION_LABEL);
    if (!s_models) {
        ESP_LOGE(TAG, "Failed to load SR models from partition '%s'", VOICE_AFE_MODEL_PARTITION_LABEL);
        voice_afe_cleanup();
        return ESP_FAIL;
    }

    char *wakenet_model = voice_afe_select_wakenet_model(s_models);
    if (!wakenet_model) {
        ESP_LOGE(TAG, "No WakeNet model found in partition '%s'", VOICE_AFE_MODEL_PARTITION_LABEL);
        voice_afe_cleanup();
        return ESP_ERR_NOT_FOUND;
    }

    s_afe_cfg = afe_config_init(VOICE_AFE_INPUT_FORMAT, s_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!s_afe_cfg) {
        ESP_LOGE(TAG, "afe_config_init failed");
        voice_afe_cleanup();
        return ESP_FAIL;
    }

    s_afe_cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    s_afe_cfg->vad_min_speech_ms = VOICE_AFE_VAD_MIN_SPEECH_MS;
    s_afe_cfg->vad_min_noise_ms = VOICE_AFE_VAD_MIN_NOISE_MS;
    s_afe_cfg->wakenet_init = true;
    s_afe_cfg->wakenet_model_name = wakenet_model;

    afe_config_t *checked_cfg = afe_config_check(s_afe_cfg);
    if (!checked_cfg) {
        ESP_LOGE(TAG, "afe_config_check failed");
        voice_afe_cleanup();
        return ESP_FAIL;
    }
    s_afe_cfg = checked_cfg;

    s_afe_iface = esp_afe_handle_from_config(s_afe_cfg);
    if (!s_afe_iface) {
        ESP_LOGE(TAG, "esp_afe_handle_from_config failed");
        voice_afe_cleanup();
        return ESP_FAIL;
    }

    s_afe_data = s_afe_iface->create_from_config(s_afe_cfg);
    if (!s_afe_data) {
        ESP_LOGE(TAG, "AFE create_from_config failed");
        voice_afe_cleanup();
        return ESP_FAIL;
    }

    s_feed_chunksize = s_afe_iface->get_feed_chunksize(s_afe_data);
    if (s_afe_iface->get_feed_channel_num) {
        s_feed_channels = s_afe_iface->get_feed_channel_num(s_afe_data);
    } else {
        s_feed_channels = s_afe_iface->get_channel_num(s_afe_data);
    }

    if (s_feed_chunksize <= 0 || s_feed_channels <= 0) {
        ESP_LOGE(TAG, "Invalid AFE feed params: chunksize=%d channels=%d", s_feed_chunksize, s_feed_channels);
        voice_afe_cleanup();
        return ESP_FAIL;
    }

    if (s_afe_iface->print_pipeline) {
        s_afe_iface->print_pipeline(s_afe_data);
    }

    s_initialized = true;
    ESP_LOGI(TAG, "AFE initialized, model=%s, feed_chunksize=%d, feed_channels=%d",
             wakenet_model, s_feed_chunksize, s_feed_channels);
    return ESP_OK;
}

esp_err_t voice_afe_feed(const int16_t *audio, size_t samples)
{
    if (!s_initialized || !s_afe_iface || !s_afe_data) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!audio) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t expected_samples = (size_t)s_feed_chunksize * (size_t)s_feed_channels;
    if (samples != expected_samples) {
        ESP_LOGW(TAG, "AFE feed expects %d x %d = %d samples, got %d",
                 s_feed_chunksize, s_feed_channels, (int)expected_samples, (int)samples);
        return ESP_ERR_INVALID_SIZE;
    }

    int ret = s_afe_iface->feed(s_afe_data, audio);
    if (ret < 0) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t voice_afe_fetch(voice_afe_result_t *out, uint32_t timeout_ms)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    out->processed_audio = NULL;
    out->processed_samples = 0;
    out->wake_word_detected = false;
    out->vad_speech = false;

    if (!s_initialized || !s_afe_iface || !s_afe_data) {
        return ESP_ERR_INVALID_STATE;
    }

    afe_fetch_result_t *result = NULL;
    if (s_afe_iface->fetch_with_delay) {
        TickType_t wait_ticks = (timeout_ms == UINT32_MAX)
                                    ? portMAX_DELAY
                                    : pdMS_TO_TICKS(timeout_ms);
        result = s_afe_iface->fetch_with_delay(s_afe_data, wait_ticks);
    } else {
        (void)timeout_ms;
        result = s_afe_iface->fetch(s_afe_data);
    }

    if (!result) {
        return ESP_ERR_TIMEOUT;
    }

    out->processed_audio = result->data;
    out->processed_samples = (result->data_size > 0)
                                 ? ((size_t)result->data_size / sizeof(int16_t))
                                 : 0;
    out->wake_word_detected = (result->wakeup_state == WAKENET_DETECTED);
    out->vad_speech = (result->vad_state == VAD_SPEECH);

    return ESP_OK;
}

int voice_afe_get_feed_chunksize(void)
{
    return s_feed_chunksize;
}

int voice_afe_get_feed_channels(void)
{
    return s_feed_channels;
}

esp_err_t voice_afe_wakenet_enable(bool enable)
{
    if (!s_initialized || !s_afe_iface || !s_afe_data) {
        return ESP_ERR_INVALID_STATE;
    }

    int ret = enable
                  ? s_afe_iface->enable_wakenet(s_afe_data)
                  : s_afe_iface->disable_wakenet(s_afe_data);

    return (ret < 0) ? ESP_FAIL : ESP_OK;
}

esp_err_t voice_afe_deinit(void)
{
    voice_afe_cleanup();
    return ESP_OK;
}

#endif
