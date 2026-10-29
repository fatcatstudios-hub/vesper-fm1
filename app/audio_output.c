/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "audio_output.h"
#include "fm1_engine_control.h"
#include "fm1_audio_io.h"
#include "lush.h"

static unsigned output_gain;
static int32_t usb_pcm[FM1_BLOCK_WORDS] __attribute__((aligned(32)));
static int16_t bluetooth_pcm[FM1_BLOCK_WORDS] __attribute__((aligned(32)));

void fm1_output_reset(void) { output_gain = 0; }
static inline __attribute__((always_inline)) int32_t limit(int64_t value)
{
    /* Bias signed 24-bit PCM into [0, 2^24). Unsigned arithmetic also
     * defines the check at the int64 limits, without two wide compares. */
    if (((uint64_t)value + 8388608U) >> 24)
        return value < 0 ? -8388608 : 8388607;
    return (int32_t)value;
}
int16_t vesper_scope[512];
volatile uint16_t vesper_scope_at;
FM1_AUDIO_CODE void fm1_output_mix(int32_t *output)
{
    if (!output) return;
    lush_render(output); /* WORSHIP: analog/string voices for LUSH tracks */
    /* WORSHIP master high-pass: 12 dB/oct Butterworth at 30 Hz (state-variable,
     * Q 0.707) clears sub rumble and DC while leaving bass notes alone
     * (-0.3 dB at 60 Hz). States are Q16 above the sample; every multiply is
     * 32x32->64, which the compiler keeps inline (no flash helper). */
    {
        static int64_t low[2], band[2];
        for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) {
            unsigned c = i & 1U;
            int32_t b = (int32_t)(band[c] >> 16);
            low[c] += (int64_t)b * 280;                       /* 2 sin(pi 30/44100), Q16 */
            int32_t high = output[i] - (int32_t)(low[c] >> 16) - (int32_t)(((int64_t)b * 92682) >> 16);
            band[c] += (int64_t)high * 280;
            output[i] = high;
        }
    }
    unsigned target = fm1_engine_output_level();
    if (target > 127U) target = 127U;
    int delta = (int)target - (int)output_gain;
    output_gain = (unsigned)((int)output_gain + (delta > 8 ? 8 : delta < -8 ? -8 : delta));

    uint16_t usb_gain[2] = {0, 0};
    int usb_frames = fm1_usb_audio_read(usb_pcm, FM1_BLOCK_FRAMES, usb_gain);
    unsigned left = ((unsigned)usb_gain[0] * output_gain) >> 7;
    unsigned right = ((unsigned)usb_gain[1] * output_gain) >> 7;
    /* Accumulate USB before clipping, then add Bluetooth.
     * Discard the block if the USB stream resets. */
    for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) {
        int64_t value = ((int64_t)output[i] * output_gain) >> 7;
        if (usb_frames == FM1_BLOCK_FRAMES)
            value += ((int64_t)usb_pcm[i] * ((i & 1U) ? right : left)) >> 7;
        output[i] = usb_frames == -2 ? 0 : limit(value);
    }
    if (fm1_bluetooth_audio_read(bluetooth_pcm, FM1_BLOCK_FRAMES) == FM1_BLOCK_FRAMES) {
        /* output is already clipped to 24 bits. Adding signed 16-bit PCM
         * times at most 254 fits in 25 bits, so this stage needs no int64. */
        for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) {
            int32_t value = output[i] + (int32_t)bluetooth_pcm[i] * (int32_t)(output_gain * 2U);
            output[i] = value > 8388607 ? 8388607 : value < -8388608 ? -8388608 : value;
        }
    }
    /* VESPER scope tap: every other frame, mono, 16-bit. */
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES / 2U; ++i)
        vesper_scope[(vesper_scope_at + i) & 511U] = (int16_t)((output[4 * i] + output[4 * i + 1]) >> 9);
    vesper_scope_at = (uint16_t)((vesper_scope_at + FM1_BLOCK_FRAMES / 2U) & 511U);
    (void)fm1_usb_audio_write(output, FM1_BLOCK_FRAMES);
}
