#pragma once

#include <stdint.h>

#include "mimi_config.h"

#ifndef MIMI_AUDIO_FRAME_MAX_SAMPLES
#define MIMI_AUDIO_FRAME_MAX_SAMPLES 320
#endif

typedef struct {
    uint64_t ts_ms;
    uint32_t sequence;
    uint32_t sample_rate_hz;
    uint16_t samples_per_channel;
    uint8_t channels;
    uint8_t bits_per_sample;
    uint16_t data_bytes;
    int16_t pcm[MIMI_AUDIO_FRAME_MAX_SAMPLES];
} audio_frame_t;
