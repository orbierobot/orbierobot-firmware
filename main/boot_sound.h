#pragma once
#include <stdint.h>

/* Boot greeting: 16000Hz, 16-bit, 1-ch PCM */
/* Voice: Orbie cloned Fish model 496ea6bb94244ec0a8b557baf96c3550 */
/* Line: "Hi there, I'm Orbie, your little robot companion. It's really lovely to meet you." */
/* Duration: 7.92s */

#define BOOT_PCM_SAMPLE_RATE 16000
#define BOOT_PCM_NUM_SAMPLES 126642
#define BOOT_PCM_NUM_BYTES 253284

extern const uint8_t boot_pcm_data[];
