/* SPDX-License-Identifier: MIT
 * Copyright (c) 2025 Dani Biro; modifications (c) 2026 M-VAVE.
 * Portable browser decoder for the fixed SEA chunk format in fm1_sea.h.
 * Saturated LMS predictor; see ../../THIRD_PARTY.md for the SEA notice. */
#include "fm1_sea.h"
#include <string.h>

static int32_t clamp16(int32_t value)
{
    return value > 32767 ? 32767 : value < -32768 ? -32768 : value;
}
static int valid(const uint8_t *chunk, size_t bytes, unsigned bits,
                 const int16_t *dqt)
{
    return chunk && dqt && (bits == 5 || bits == 6) &&
        bytes == 28 + FM1_SEA_FRAMES * bits / 8 &&
        chunk[0] == 1 && chunk[1] == (0x40 | bits) &&
        chunk[2] == FM1_SEA_GROUP && chunk[3] == 0x5a;
}
static void seed(const uint8_t *chunk, int32_t state[8])
{
    for (unsigned i = 0; i < 8; ++i) {
        unsigned value = chunk[4 + i*2] | (unsigned)chunk[5 + i*2] << 8;
        state[i] = value < 32768 ? (int32_t)value : (int32_t)value - 65536;
    }
}
static void group(const uint8_t *chunk, unsigned bits, const int16_t *dqt,
                  unsigned g, int32_t state[8], int16_t pcm[FM1_SEA_GROUP])
{
    /* VESPER 2: the same decode, faster (the FM-1 decodes samples in the audio interrupt): each code read from a
     * 16-bit window instead of bit by bit, the predictor in locals, and the history (saturated to 16 bits) times
     * the weights (likewise) as 32-bit products summed in 64 bits: no 64-bit multiply. Bit-exact with the original. */
    const unsigned scale = (chunk[20 + g/2] >> ((g & 1) ? 0 : 4)) & 15, mask = (1U << bits) - 1U;
    const int16_t *q = dqt + (scale << bits);
    const uint8_t *data = chunk + 28;
    int32_t s0 = state[0], s1 = state[1], s2 = state[2], s3 = state[3];
    int32_t w0 = state[4], w1 = state[5], w2 = state[6], w3 = state[7];
    unsigned at = g * FM1_SEA_GROUP * bits;
    for (unsigned i = 0; i < FM1_SEA_GROUP; ++i, at += bits) {
        unsigned byte = at >> 3, sh = at & 7U, win = (unsigned)data[byte] << 8;
        if (sh + bits > 8U) win |= data[byte + 1];
        unsigned code = (win >> (16U - bits - sh)) & mask;
        int64_t sum = (int64_t)(s0 * w0) + (int64_t)(s1 * w1) + (int64_t)(s2 * w2) + (int64_t)(s3 * w3);
        int32_t residual = q[code];
        int32_t value = clamp16((int32_t)(sum >> 13) + residual);
        int32_t delta = residual >> 4;
        w0 = clamp16(w0 + (s0 < 0 ? -delta : delta));
        w1 = clamp16(w1 + (s1 < 0 ? -delta : delta));
        w2 = clamp16(w2 + (s2 < 0 ? -delta : delta));
        w3 = clamp16(w3 + (s3 < 0 ? -delta : delta));
        s0 = s1; s1 = s2; s2 = s3; s3 = value;
        pcm[i] = (int16_t)value;
    }
    state[0] = s0; state[1] = s1; state[2] = s2; state[3] = s3;
    state[4] = w0; state[5] = w1; state[6] = w2; state[7] = w3;
}
int fm1_sea_decode(const uint8_t *chunk, size_t bytes, unsigned bits,
                   const int16_t *dqt, int16_t pcm[FM1_SEA_FRAMES])
{
    if (!pcm || !valid(chunk, bytes, bits, dqt)) return -1;
    int32_t state[8];
    seed(chunk, state);
    for (unsigned g = 0; g < FM1_SEA_FRAMES/FM1_SEA_GROUP; ++g)
        group(chunk, bits, dqt, g, state, pcm + g*FM1_SEA_GROUP);
    return 0;
}
int fm1_sea_decode_groups(const uint8_t *chunk, size_t bytes, unsigned bits,
    const int16_t *dqt, unsigned first, unsigned last, int32_t state[8],
    int16_t pcm[FM1_SEA_GROUP])
{
    if (!state || !pcm || !valid(chunk, bytes, bits, dqt) || first > last ||
        last >= FM1_SEA_FRAMES/FM1_SEA_GROUP) return -1;
    /* A continuation state is always saturated by the preceding group. */
    if (first) {
        for (unsigned i = 0; i < 8; ++i)
            if (state[i] < -32768 || state[i] > 32767) return -1;
    } else seed(chunk, state);
    for (unsigned g = first; g <= last; ++g) group(chunk, bits, dqt, g, state, pcm);
    return 0;
}
