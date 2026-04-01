#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#include "audio/audio_frame.h"
#include "audio/audio_ring_buffer.h"

typedef struct {
    bool initialized;
    bool enabled;
    bool running;
    uint32_t sample_rate_hz;
    uint16_t frame_samples;
    uint32_t read_ok;
    uint32_t read_fail;
    uint32_t ring_drop;
    uint32_t overflow_events;
    uint32_t avg_read_latency_us;
    audio_ring_buffer_stats_t ring;
} audio_hal_capture_stats_t;

esp_err_t audio_hal_capture_init(void);
esp_err_t audio_hal_capture_start(void);
esp_err_t audio_hal_capture_stop(void);

bool audio_hal_capture_is_running(void);

esp_err_t audio_hal_capture_read_frame(audio_frame_t *out, TickType_t timeout_ticks);
esp_err_t audio_hal_capture_get_stats(audio_hal_capture_stats_t *out);
