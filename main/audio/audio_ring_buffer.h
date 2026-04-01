#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "audio/audio_frame.h"

typedef struct {
    uint32_t pushes;
    uint32_t pops;
    uint32_t drops;
    uint32_t current_fill;
    uint32_t peak_fill;
} audio_ring_buffer_stats_t;

typedef struct {
    QueueHandle_t queue;
    SemaphoreHandle_t lock;
    uint16_t depth;
    audio_ring_buffer_stats_t stats;
} audio_ring_buffer_t;

esp_err_t audio_ring_buffer_init(audio_ring_buffer_t *rb, uint16_t depth);
void audio_ring_buffer_deinit(audio_ring_buffer_t *rb);

esp_err_t audio_ring_buffer_push(audio_ring_buffer_t *rb,
                                 const audio_frame_t *frame,
                                 TickType_t timeout_ticks);
esp_err_t audio_ring_buffer_pop(audio_ring_buffer_t *rb,
                                audio_frame_t *frame,
                                TickType_t timeout_ticks);

void audio_ring_buffer_reset(audio_ring_buffer_t *rb);
esp_err_t audio_ring_buffer_get_stats(audio_ring_buffer_t *rb,
                                      audio_ring_buffer_stats_t *out);
