#include "channels/voice/voice_channel.h"

#if MIMI_VOICE_ENABLED

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio_hal.h"
#include "bus/message_bus.h"
#include "channels/voice/voice_afe.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define VOICE_SAMPLE_RATE_HZ            16000
#define VOICE_CHAT_ID                   "voice_default"

typedef enum {
    VOICE_STATE_IDLE = 0,
    VOICE_STATE_RECORDING,
    VOICE_STATE_PROCESSING,
    VOICE_STATE_PLAYING,
} voice_state_t;

static const char *TAG = "voice_channel";

static TaskHandle_t s_feed_task = NULL;
static TaskHandle_t s_detect_task = NULL;
static bool s_initialized = false;
static bool s_running = false;

static voice_state_t s_state = VOICE_STATE_IDLE;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;

static int16_t *s_feed_buffer = NULL;
static size_t s_feed_frame_samples = 0;

static int16_t *s_recording_buffer = NULL;
static size_t s_recording_capacity_bytes = 0;
static size_t s_recording_len_bytes = 0;
static uint32_t s_silence_ms = 0;
static uint32_t s_speech_ms = 0;

static bool s_stt_ready = false;
static bool s_tts_ready = false;

static voice_state_t voice_state_get(void)
{
    voice_state_t state;
    portENTER_CRITICAL(&s_state_lock);
    state = s_state;
    portEXIT_CRITICAL(&s_state_lock);
    return state;
}

static void voice_state_set(voice_state_t state)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state = state;
    portEXIT_CRITICAL(&s_state_lock);
}

static void voice_recording_reset(void)
{
    s_recording_len_bytes = 0;
    s_silence_ms = 0;
    s_speech_ms = 0;
}

static uint32_t voice_samples_to_ms(size_t samples)
{
    if (samples == 0) {
        return 0;
    }
    return (uint32_t)((samples * 1000U) / VOICE_SAMPLE_RATE_HZ);
}

static bool voice_recording_append(const int16_t *audio, size_t samples)
{
    if (!audio || samples == 0 || !s_recording_buffer) {
        return false;
    }

    size_t bytes = samples * sizeof(int16_t);
    size_t remaining = (s_recording_capacity_bytes > s_recording_len_bytes)
                           ? (s_recording_capacity_bytes - s_recording_len_bytes)
                           : 0;

    if (bytes > remaining) {
        if (remaining > 0) {
            memcpy(((uint8_t *)s_recording_buffer) + s_recording_len_bytes, audio, remaining);
            s_recording_len_bytes += remaining;
        }
        return true;
    }

    memcpy(((uint8_t *)s_recording_buffer) + s_recording_len_bytes, audio, bytes);
    s_recording_len_bytes += bytes;
    return false;
}

static esp_err_t voice_stt_client_init(void)
{
    s_stt_ready = true;
    return ESP_OK;
}

static esp_err_t voice_tts_client_init(void)
{
    s_tts_ready = true;
    return ESP_OK;
}

static esp_err_t voice_stt_transcribe_pcm(const int16_t *audio, size_t samples,
                                          char *out_text, size_t out_text_size)
{
    if (!s_stt_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!audio || samples == 0 || !out_text || out_text_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t duration_ms = voice_samples_to_ms(samples);
    snprintf(out_text, out_text_size, "voice captured (%u ms)", (unsigned)duration_ms);
    return ESP_OK;
}

static esp_err_t voice_tts_synthesize_pcm(const char *text,
                                          int16_t **out_audio,
                                          size_t *out_samples,
                                          uint32_t *out_sample_rate_hz)
{
    if (!s_tts_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!text || !out_audio || !out_samples || !out_sample_rate_hz) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_audio = NULL;
    *out_samples = 0;
    *out_sample_rate_hz = VOICE_SAMPLE_RATE_HZ;

    size_t text_len = strlen(text);
    uint32_t tone_ms = 200 + (text_len > 100 ? 500 : (uint32_t)(text_len * 5));
    size_t samples = ((size_t)tone_ms * VOICE_SAMPLE_RATE_HZ) / 1000;
    if (samples == 0) {
        samples = VOICE_SAMPLE_RATE_HZ / 10;
    }

    int16_t *buf = heap_caps_malloc(samples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    const int period = VOICE_SAMPLE_RATE_HZ / 440;
    for (size_t i = 0; i < samples; i++) {
        buf[i] = ((int)(i % period) < (period / 2)) ? 900 : -900;
    }

    *out_audio = buf;
    *out_samples = samples;
    return ESP_OK;
}

static esp_err_t voice_channel_publish_transcript(void)
{
    if (!s_recording_buffer || s_recording_len_bytes == 0) {
        voice_state_set(VOICE_STATE_IDLE);
        return ESP_OK;
    }

    if (s_speech_ms < MIMI_VOICE_MIN_SPEECH_MS) {
        ESP_LOGI(TAG, "Discard short speech: %u ms < %u ms",
                 (unsigned)s_speech_ms, (unsigned)MIMI_VOICE_MIN_SPEECH_MS);
        voice_recording_reset();
        voice_state_set(VOICE_STATE_IDLE);
        return ESP_OK;
    }

    char transcript[256] = {0};
    esp_err_t stt_err = voice_stt_transcribe_pcm(
        s_recording_buffer,
        s_recording_len_bytes / sizeof(int16_t),
        transcript,
        sizeof(transcript));
    voice_recording_reset();

    if (stt_err != ESP_OK || transcript[0] == '\0') {
        ESP_LOGW(TAG, "STT failed: %s", esp_err_to_name(stt_err));
        voice_state_set(VOICE_STATE_IDLE);
        return stt_err != ESP_OK ? stt_err : ESP_FAIL;
    }

    mimi_msg_t msg = {0};
    strncpy(msg.channel, MIMI_CHAN_VOICE, sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, VOICE_CHAT_ID, sizeof(msg.chat_id) - 1);
    msg.content = strdup(transcript);
    if (!msg.content) {
        voice_state_set(VOICE_STATE_IDLE);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t push_err = message_bus_push_inbound(&msg);
    if (push_err != ESP_OK) {
        free(msg.content);
        voice_state_set(VOICE_STATE_IDLE);
        return push_err;
    }

    ESP_LOGI(TAG, "Voice transcript queued: %s", transcript);
    return ESP_OK;
}

static void voice_feed_task(void *arg)
{
    (void)arg;
    size_t fill = 0;

    while (s_running) {
        size_t need = s_feed_frame_samples - fill;
        size_t got = audio_hal_mic_read(s_feed_buffer + fill, need, 50);
        if (got == 0) {
            continue;
        }

        fill += got;
        if (fill < s_feed_frame_samples) {
            continue;
        }

        if (voice_afe_feed(s_feed_buffer, s_feed_frame_samples) != ESP_OK) {
            ESP_LOGW(TAG, "voice_afe_feed failed");
        }
        fill = 0;
    }

    s_feed_task = NULL;
    vTaskDelete(NULL);
}

static void voice_detect_task(void *arg)
{
    (void)arg;

    while (s_running) {
        voice_afe_result_t result = {0};
        if (voice_afe_fetch(&result, 100) != ESP_OK) {
            continue;
        }

        voice_state_t state = voice_state_get();
        if (state == VOICE_STATE_PLAYING || state == VOICE_STATE_PROCESSING) {
            continue;
        }

        if (state == VOICE_STATE_IDLE) {
            if (!result.wake_word_detected) {
                continue;
            }

            ESP_LOGI(TAG, "Wake word detected, transition IDLE -> RECORDING");
            voice_recording_reset();
            voice_state_set(VOICE_STATE_RECORDING);
            state = VOICE_STATE_RECORDING;
        }

        if (state != VOICE_STATE_RECORDING) {
            continue;
        }

        bool overflow = voice_recording_append(result.processed_audio, result.processed_samples);

        uint32_t chunk_ms = voice_samples_to_ms(result.processed_samples);
        if (result.vad_speech) {
            s_speech_ms += chunk_ms;
            s_silence_ms = 0;
        } else {
            s_silence_ms += chunk_ms;
        }

        bool end_of_speech = (s_silence_ms >= MIMI_VOICE_SILENCE_TIMEOUT_MS);
        if (!overflow && !end_of_speech) {
            continue;
        }

        if (overflow) {
            ESP_LOGI(TAG, "Recording overflow (%u bytes), forcing PROCESSING",
                     (unsigned)s_recording_len_bytes);
        } else {
            ESP_LOGI(TAG, "Silence timeout (%u ms), transition RECORDING -> PROCESSING",
                     (unsigned)s_silence_ms);
        }

        voice_state_set(VOICE_STATE_PROCESSING);
        if (voice_channel_publish_transcript() != ESP_OK && voice_state_get() == VOICE_STATE_PROCESSING) {
            voice_state_set(VOICE_STATE_IDLE);
        }
    }

    s_detect_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t voice_channel_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t err = audio_hal_init();
    if (err != ESP_OK) {
        return err;
    }

    err = voice_afe_init();
    if (err != ESP_OK) {
        return err;
    }

    err = voice_stt_client_init();
    if (err != ESP_OK) {
        return err;
    }

    err = voice_tts_client_init();
    if (err != ESP_OK) {
        return err;
    }

    int feed_chunksize = voice_afe_get_feed_chunksize();
    int feed_channels = voice_afe_get_feed_channels();
    if (feed_chunksize <= 0 || feed_channels <= 0) {
        return ESP_FAIL;
    }

    s_feed_frame_samples = (size_t)feed_chunksize * (size_t)feed_channels;
    s_feed_buffer = heap_caps_malloc(
        s_feed_frame_samples * sizeof(int16_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_feed_buffer) {
        return ESP_ERR_NO_MEM;
    }

    s_recording_capacity_bytes =
        ((size_t)MIMI_VOICE_MAX_RECORD_MS * VOICE_SAMPLE_RATE_HZ * sizeof(int16_t)) / 1000;
    s_recording_buffer = heap_caps_malloc(s_recording_capacity_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_recording_buffer) {
        free(s_feed_buffer);
        s_feed_buffer = NULL;
        return ESP_ERR_NO_MEM;
    }

    voice_recording_reset();
    voice_state_set(VOICE_STATE_IDLE);
    audio_hal_spk_enable(false);

    s_initialized = true;
    ESP_LOGI(TAG, "Voice channel initialized (recording buffer=%u bytes)",
             (unsigned)s_recording_capacity_bytes);
    return ESP_OK;
}

esp_err_t voice_channel_start(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_running) {
        return ESP_OK;
    }

    s_running = true;

    BaseType_t feed_ok = xTaskCreatePinnedToCore(
        voice_feed_task,
        "voice_feed",
        MIMI_VOICE_FEED_STACK,
        NULL,
        MIMI_VOICE_FEED_PRIO,
        &s_feed_task,
        MIMI_VOICE_FEED_CORE);

    if (feed_ok != pdPASS) {
        s_running = false;
        return ESP_FAIL;
    }

    BaseType_t detect_ok = xTaskCreatePinnedToCore(
        voice_detect_task,
        "voice_detect",
        MIMI_VOICE_DETECT_STACK,
        NULL,
        MIMI_VOICE_DETECT_PRIO,
        &s_detect_task,
        MIMI_VOICE_DETECT_CORE);

    if (detect_ok != pdPASS) {
        s_running = false;
        if (s_feed_task) {
            vTaskDelete(s_feed_task);
            s_feed_task = NULL;
        }
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t voice_channel_send_message(const char *text)
{
    if (!s_initialized || !text) {
        return ESP_ERR_INVALID_ARG;
    }

    voice_state_set(VOICE_STATE_PLAYING);

    esp_err_t ret = voice_afe_wakenet_enable(false);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Disable wakenet failed: %s", esp_err_to_name(ret));
    }

    int16_t *tts_audio = NULL;
    size_t tts_samples = 0;
    uint32_t tts_sample_rate_hz = VOICE_SAMPLE_RATE_HZ;

    ret = voice_tts_synthesize_pcm(text, &tts_audio, &tts_samples, &tts_sample_rate_hz);
    if (ret == ESP_OK && tts_audio && tts_samples > 0) {
        audio_hal_spk_set_sample_rate(tts_sample_rate_hz);
        audio_hal_spk_enable(true);

        size_t total_written = 0;
        while (total_written < tts_samples) {
            size_t just_written = 0;
            esp_err_t wr = audio_hal_spk_write(
                tts_audio + total_written,
                tts_samples - total_written,
                &just_written,
                MIMI_VOICE_TTS_TIMEOUT_MS);
            total_written += just_written;

            if (wr != ESP_OK || just_written == 0) {
                ret = (wr != ESP_OK) ? wr : ESP_FAIL;
                break;
            }
        }

        audio_hal_spk_enable(false);
    }

    if (tts_audio) {
        free(tts_audio);
    }

    esp_err_t wn_err = voice_afe_wakenet_enable(true);
    if (wn_err != ESP_OK) {
        ESP_LOGW(TAG, "Enable wakenet failed: %s", esp_err_to_name(wn_err));
        if (ret == ESP_OK) {
            ret = wn_err;
        }
    }

    voice_state_set(VOICE_STATE_IDLE);
    return ret;
}

esp_err_t voice_channel_stop(void)
{
    s_running = false;

    if (s_feed_task) {
        vTaskDelete(s_feed_task);
        s_feed_task = NULL;
    }
    if (s_detect_task) {
        vTaskDelete(s_detect_task);
        s_detect_task = NULL;
    }

    if (s_feed_buffer) {
        free(s_feed_buffer);
        s_feed_buffer = NULL;
    }
    if (s_recording_buffer) {
        free(s_recording_buffer);
        s_recording_buffer = NULL;
    }

    voice_recording_reset();
    s_recording_capacity_bytes = 0;
    s_feed_frame_samples = 0;

    audio_hal_spk_enable(false);
    voice_afe_deinit();
    audio_hal_deinit();

    s_stt_ready = false;
    s_tts_ready = false;
    s_initialized = false;
    voice_state_set(VOICE_STATE_IDLE);

    return ESP_OK;
}

#endif
