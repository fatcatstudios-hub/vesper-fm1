/* Measure how much of each stage's output is "fuzz": energy not at the note's harmonics. */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#define FM1_DSP_DATA
#include "fm1_sea.h"
#include "sampledrum.h"
#include "sea_encode.h"
#include "sample_metadata.inc"
int main(void)
{
    /* SEA5 codec noise on the piano C4 zone: decode vs original PCM. */
    FILE *f = fopen("/home/claude/fm1_open/build/samples/PIANO_C4.raw", "rb");
    static int16_t pcm[200000], out[200000]; size_t n = fread(pcm, 2, 200000, f); fclose(f);
    uint8_t *sea = fm1_sea_encode(pcm, (unsigned)n, sample_dqt);
    for (size_t c = 0; c * 320 < n; ++c) fm1_sea_decode(sea + c * 228, 228, 5, sample_dqt, out + c * 320);
    for (unsigned w = 0; w < 4; ++w) {
        size_t a = w * 16000 + 1600, b = a + 16000; double s = 0, e = 0;
        for (size_t i = a; i < b && i < n; ++i) { s += (double)pcm[i] * pcm[i]; e += (double)(out[i] - pcm[i]) * (out[i] - pcm[i]); }
        printf("SEA5 piano C4 window %.1f-%.1f s: signal/noise %.1f dB\n", a / 32000.0, b / 32000.0, 10 * log10(s / e));
    }
    return 0;
}
