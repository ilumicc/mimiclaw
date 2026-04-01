#include "tts/tts_service.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"

#include "mimi_config.h"
#include "tts/groq_tts_client.h"
#include "audio/speaker_i2s.h"

typedef struct {
    char text[MIMI_TTS_MAX_TEXT_LEN + 1];
} tts_req_t;

static const char *TAG = "tts_service";

static QueueHandle_t s_queue = NULL;
static TaskHandle_t s_worker = NULL;
static bool s_ready = false;

static void tts_worker_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "TTS worker started on core %d", xPortGetCoreID());

    while (1) {
        tts_req_t req;
        if (xQueueReceive(s_queue, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (!req.text[0]) {
            continue;
        }

        uint8_t *wav = NULL;
        size_t wav_len = 0;

        esp_err_t err = groq_tts_synthesize_wav(req.text, &wav, &wav_len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Synthesis failed: %s", esp_err_to_name(err));
            free(wav);
            continue;
        }

        err = speaker_i2s_play_wav(wav, wav_len);
        free(wav);

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Playback failed: %s", esp_err_to_name(err));
            continue;
        }

        ESP_LOGI(TAG, "Spoken %u chars", (unsigned)strlen(req.text));
    }
}

esp_err_t tts_service_init(void)
{
    if (s_queue) {
        return ESP_OK;
    }

    s_queue = xQueueCreate(MIMI_TTS_QUEUE_LEN, sizeof(tts_req_t));
    if (!s_queue) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = groq_tts_client_init();
    if (err != ESP_OK) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return err;
    }

    s_ready = true;
    ESP_LOGI(TAG, "TTS service initialized (queue=%d)", MIMI_TTS_QUEUE_LEN);
    return ESP_OK;
}

esp_err_t tts_service_start(void)
{
    if (!s_ready) {
        esp_err_t err = tts_service_init();
        if (err != ESP_OK) {
            return err;
        }
    }

    if (s_worker) {
        return ESP_OK;
    }

    if (xTaskCreatePinnedToCore(tts_worker_task, "tts_worker",
                                MIMI_TTS_STACK, NULL,
                                MIMI_TTS_PRIO, &s_worker,
                                MIMI_TTS_CORE) != pdPASS) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t tts_service_enqueue_text(const char *text)
{
    if (!s_ready || !s_queue) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!text || !text[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    tts_req_t req = {0};
    size_t src_len = strlen(text);
    size_t n = src_len;
    if (n >= sizeof(req.text)) {
        n = sizeof(req.text) - 1;
        ESP_LOGW(TAG, "TTS text truncated from %u to %u bytes", (unsigned)src_len, (unsigned)n);
    }
    memcpy(req.text, text, n);
    req.text[n] = '\0';

    if (xQueueSend(s_queue, &req, 0) != pdTRUE) {
        ESP_LOGW(TAG, "TTS queue full (len=%u), dropping text", (unsigned)strlen(req.text));
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

bool tts_service_is_ready(void)
{
    return s_ready;
}

bool tts_service_is_configured(void)
{
    return groq_tts_is_configured();
}

esp_err_t tts_service_diag(char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    UBaseType_t queued = s_queue ? uxQueueMessagesWaiting(s_queue) : 0;
    snprintf(buf, buf_size,
             "ready=%s configured=%s queue=%u/%d worker=%s model=%s voice=%s speaker_init=%s",
             s_ready ? "yes" : "no",
             groq_tts_is_configured() ? "yes" : "no",
             (unsigned)queued,
             MIMI_TTS_QUEUE_LEN,
             s_worker ? "yes" : "no",
             groq_tts_get_model(),
             groq_tts_get_voice(),
             speaker_i2s_is_initialized() ? "yes" : "no");

    return ESP_OK;
}
