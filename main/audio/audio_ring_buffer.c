#include "audio/audio_ring_buffer.h"

#include <string.h>

#include "freertos/semphr.h"

static bool ring_lock_take(audio_ring_buffer_t *rb, TickType_t timeout_ticks)
{
    if (!rb || !rb->lock) {
        return false;
    }
    return xSemaphoreTake(rb->lock, timeout_ticks) == pdTRUE;
}

static void ring_lock_give(audio_ring_buffer_t *rb)
{
    if (rb && rb->lock) {
        xSemaphoreGive(rb->lock);
    }
}

esp_err_t audio_ring_buffer_init(audio_ring_buffer_t *rb, uint16_t depth)
{
    if (!rb || depth == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(rb, 0, sizeof(*rb));
    rb->queue = xQueueCreate(depth, sizeof(audio_frame_t));
    if (!rb->queue) {
        return ESP_ERR_NO_MEM;
    }

    rb->lock = xSemaphoreCreateMutex();
    if (!rb->lock) {
        vQueueDelete(rb->queue);
        rb->queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    rb->depth = depth;
    return ESP_OK;
}

void audio_ring_buffer_deinit(audio_ring_buffer_t *rb)
{
    if (!rb) {
        return;
    }

    if (rb->queue) {
        vQueueDelete(rb->queue);
        rb->queue = NULL;
    }
    if (rb->lock) {
        vSemaphoreDelete(rb->lock);
        rb->lock = NULL;
    }
    memset(&rb->stats, 0, sizeof(rb->stats));
    rb->depth = 0;
}

esp_err_t audio_ring_buffer_push(audio_ring_buffer_t *rb,
                                 const audio_frame_t *frame,
                                 TickType_t timeout_ticks)
{
    if (!rb || !rb->queue || !frame) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xQueueSend(rb->queue, frame, timeout_ticks) != pdTRUE) {
        if (ring_lock_take(rb, pdMS_TO_TICKS(10))) {
            rb->stats.drops++;
            ring_lock_give(rb);
        }
        return ESP_ERR_TIMEOUT;
    }

    if (ring_lock_take(rb, pdMS_TO_TICKS(10))) {
        rb->stats.pushes++;
        UBaseType_t fill = uxQueueMessagesWaiting(rb->queue);
        rb->stats.current_fill = (uint32_t)fill;
        if (rb->stats.current_fill > rb->stats.peak_fill) {
            rb->stats.peak_fill = rb->stats.current_fill;
        }
        ring_lock_give(rb);
    }

    return ESP_OK;
}

esp_err_t audio_ring_buffer_pop(audio_ring_buffer_t *rb,
                                audio_frame_t *frame,
                                TickType_t timeout_ticks)
{
    if (!rb || !rb->queue || !frame) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xQueueReceive(rb->queue, frame, timeout_ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (ring_lock_take(rb, pdMS_TO_TICKS(10))) {
        rb->stats.pops++;
        rb->stats.current_fill = (uint32_t)uxQueueMessagesWaiting(rb->queue);
        ring_lock_give(rb);
    }

    return ESP_OK;
}

void audio_ring_buffer_reset(audio_ring_buffer_t *rb)
{
    if (!rb || !rb->queue) {
        return;
    }

    xQueueReset(rb->queue);
    if (ring_lock_take(rb, pdMS_TO_TICKS(10))) {
        memset(&rb->stats, 0, sizeof(rb->stats));
        ring_lock_give(rb);
    }
}

esp_err_t audio_ring_buffer_get_stats(audio_ring_buffer_t *rb,
                                      audio_ring_buffer_stats_t *out)
{
    if (!rb || !rb->queue || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!ring_lock_take(rb, pdMS_TO_TICKS(20))) {
        return ESP_ERR_TIMEOUT;
    }

    *out = rb->stats;
    out->current_fill = (uint32_t)uxQueueMessagesWaiting(rb->queue);
    ring_lock_give(rb);
    return ESP_OK;
}
