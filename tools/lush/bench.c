#include "lush.h"
#include "fm1.h"
#include <stdio.h>
#include <time.h>
#include <x86intrin.h>
int main(void)
{
    static int32_t out[256];
    uint8_t fx[4] = {110, 135, 58, 88};
    lush_effects(fx, 1000);
    lush_layer(0, LUSH_SUPERSAW, 100, 6, 11);
    lush_layer(1, LUSH_DETUNE_PLUCK, 100, 8, 12);
    lush_layer(2, LUSH_DETUNE_BASS, 100, 1, 0);
    lush_layer(3, LUSH_AIR_BELL, 90, 13, 9);
    lush_render(out);
    unsigned pad[4] = {52, 59, 66, 68};
    for (int i = 0; i < 4; ++i) lush_note(0, pad[i], 92);
    lush_note(2, 40, 96);
    unsigned long long cycles = 0; unsigned blocks = 0;
    for (unsigned b = 0; b < 44100 * 20 / 128; ++b) {
        if (b % 10 == 0) { lush_note(1, 64 + (b / 10) % 7, 100); lush_note(0, 60 + (b / 10) % 5, 96); }      /* 8th-ish plucks, overlapping */
        if (b % 40 == 0) { lush_note(3, 76 + (b / 40) % 5, 80); }
        unsigned long long t = __rdtsc();
        lush_render(out);
        cycles += __rdtsc() - t; ++blocks;
    }
    printf("%.1f cycles/sample (x86 TSC), %.2f%% of one 3 GHz core\n",
           (double)cycles / (blocks * 128.0), 100.0 * cycles / (blocks * 128.0) * 44100 / 3e9);
    return 0;
}
