#include "voice/voice_session_coordinator.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"

#include "mimi_config.h"
#include "bus/message_bus.h"
#include "voice/wake_service.h"

static const char *TAG = "voice_session";

static SemaphoreHandle_t s_lock = NULL;
static TaskHandle_t s_task = NULL;
static bool s_initialized = false;

static voice_session_state_t s_state = VOICE_SESSION_IDLE;
static TickType_t s_listen_started_tick = 0;
static voice_session_stats_t s_stats = {0};

static bool session_lock_take(TickType_t ticks)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (!s_lock) {
        return false;
    }
    return xSemaphoreTake(s_lock, ticks) == pdTRUE;
}

static void session_lock_give(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static void publish_system_event(const char *event, const char *detail)
{
    if (!event) {
        return;
    }

    char buf[192] = {0};
    if (detail && detail[0]) {
        snprintf(buf, sizeof(buf), "%s:%s", event, detail);
    } else {
        snprintf(buf, sizeof(buf), "%s", event);
    }

    mimi_msg_t msg = {0};
    snprintf(msg.channel, sizeof(msg.channel), "%s", MIMI_CHAN_SYSTEM);
    snprintf(msg.chat_id, sizeof(msg.chat_id), "%s", "voice");
    msg.content = strdup(buf);
    if (!msg.content) {
        return;
    }

    if (message_bus_push_outbound(&msg) != ESP_OK) {
        free(msg.content);
    }
}

static void session_mark_ready_locked(bool timeout_reason)
{
    if (s_state != VOICE_SESSION_LISTENING) {
        return;
    }

    TickType_t now = xTaskGetTickCount();
    uint32_t elapsed_ms = (uint32_t)pdTICKS_TO_MS(now - s_listen_started_tick);

    s_state = VOICE_SESSION_END_UTTERANCE;
    s_stats.state = s_state;
    s_stats.sessions_ready++;
    s_stats.last_session_ms = elapsed_ms;
    if (timeout_reason) {
        s_stats.sessions_timeout++;
    }

    char detail[128] = {0};
    snprintf(detail, sizeof(detail), "dur_ms=%u reason=%s",
             (unsigned)elapsed_ms,
             timeout_reason ? "timeout" : "final_text");
    publish_system_event("voice_session_ready", detail);

    s_state = VOICE_SESSION_IDLE;
    s_stats.state = s_state;
}

static void wake_event_cb(const wake_event_t *event, void *ctx)
{
    (void)ctx;
    if (!event) {
        return;
    }

    if (!session_lock_take(pdMS_TO_TICKS(100))) {
        return;
    }

    s_stats.wake_hits++;
    snprintf(s_stats.last_wake_source, sizeof(s_stats.last_wake_source), "%s", event->source);
    snprintf(s_stats.last_wake_phrase, sizeof(s_stats.last_wake_phrase), "%s", event->phrase);

    s_state = VOICE_SESSION_WAKE_HIT;
    s_stats.state = s_state;

    s_state = VOICE_SESSION_LISTENING;
    s_stats.state = s_state;
    s_stats.sessions_started++;
    s_listen_started_tick = xTaskGetTickCount();

    session_lock_give();

    publish_system_event("wake_detected", event->source);
    publish_system_event("voice_listening_started", NULL);
}

static void coordinator_task(void *arg)
{
    (void)arg;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(200));

        if (!session_lock_take(pdMS_TO_TICKS(50))) {
            continue;
        }

        if (s_state == VOICE_SESSION_LISTENING) {
            TickType_t now = xTaskGetTickCount();
            uint32_t elapsed_ms = (uint32_t)pdTICKS_TO_MS(now - s_listen_started_tick);
            if (elapsed_ms >= MIMI_VOICE_LISTEN_TIMEOUT_MS) {
                session_mark_ready_locked(true);
            }
        }

        session_lock_give();
    }
}

esp_err_t voice_session_coordinator_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    if (!session_lock_take(pdMS_TO_TICKS(100))) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_initialized) {
        session_lock_give();
        return ESP_OK;
    }

    memset(&s_stats, 0, sizeof(s_stats));
    s_state = VOICE_SESSION_IDLE;
    s_stats.state = s_state;

    session_lock_give();

    esp_err_t err = wake_service_set_callback(wake_event_cb, NULL);
    if (err != ESP_OK) {
        return err;
    }

    if (xTaskCreatePinnedToCore(coordinator_task,
                                "voice_session",
                                MIMI_VOICE_SESSION_STACK,
                                NULL,
                                MIMI_VOICE_SESSION_PRIO,
                                &s_task,
                                MIMI_VOICE_SESSION_CORE) != pdPASS) {
        return ESP_FAIL;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Voice session coordinator initialized");
    return ESP_OK;
}

esp_err_t voice_session_coordinator_on_transcript_final(const char *text)
{
    (void)text;
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!session_lock_take(pdMS_TO_TICKS(100))) {
        return ESP_ERR_TIMEOUT;
    }
    session_mark_ready_locked(false);
    session_lock_give();
    return ESP_OK;
}

esp_err_t voice_session_coordinator_force_ready(void)
{
    return voice_session_coordinator_on_transcript_final(NULL);
}

voice_session_state_t voice_session_coordinator_state(void)
{
    return s_state;
}

const char *voice_session_coordinator_state_str(voice_session_state_t state)
{
    switch (state) {
    case VOICE_SESSION_IDLE:
        return "IDLE";
    case VOICE_SESSION_WAKE_HIT:
        return "WAKE_HIT";
    case VOICE_SESSION_LISTENING:
        return "LISTENING";
    case VOICE_SESSION_END_UTTERANCE:
        return "END_UTTERANCE";
    default:
        return "UNKNOWN";
    }
}

esp_err_t voice_session_coordinator_get_stats(voice_session_stats_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!session_lock_take(pdMS_TO_TICKS(100))) {
        return ESP_ERR_TIMEOUT;
    }
    *out = s_stats;
    out->state = s_state;
    session_lock_give();
    return ESP_OK;
}
