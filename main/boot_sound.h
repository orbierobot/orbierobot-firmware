#pragma once
#include <stdint.h>

/* Boot greeting: 16000Hz, 16-bit, 1-ch PCM */
/* Duration: 7.63s */

#define BOOT_PCM_SAMPLE_RATE 16000
#define BOOT_PCM_NUM_SAMPLES 122112
#define BOOT_PCM_NUM_BYTES 244224

extern const uint8_t boot_pcm_data[];
