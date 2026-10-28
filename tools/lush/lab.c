/* Host harness: render LUSH patches directly. */
#include "lush.h"
#include "fm1.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void write_wav(const char *path, const int32_t *pcm, unsigned frames)
{
    FILE *f = fopen(path, "wb"); unsigned bytes = frames * 4;
    unsigned hdr[] = {0x46464952, 36 + bytes, 0x45564157, 0x20746d66, 16, 0x00020001, 44100, 44100 * 4, 0x00100004, 0x61746164, bytes};
    fwrite(hdr, 4, 11, f);
    for (unsigned i = 0; i < frames * 2; ++i) { int v = pcm[i] >> 8; if (v > 32767) v = 32767; if (v < -32768) v = -32768; short s = (short)v; fwrite(&s, 2, 1, f); }
    fclose(f);
}
int main(int argc, char **argv)
{
    /* usage: lab patch note seconds_on seconds_total rev dly out.wav */
    unsigned patch = atoi(argv[1]), note = atoi(argv[2]);
    double on = atof(argv[3]), total = atof(argv[4]);
    unsigned frames = (unsigned)(total * 44100 / 128) * 128;
    int32_t *pcm = calloc(frames * 2, sizeof *pcm);
    uint8_t fx[4] = {110, 135, 58, 88};
    lush_layer(0, patch, 100, atoi(argv[5]), atoi(argv[6]));
    lush_effects(fx, 1000);
    lush_render(pcm);
    lush_note(0, note, 100);
    unsigned off = (unsigned)(on * 44100 / 128);
    for (unsigned b = 0; b < frames / 128; ++b) {
        if (b == off) lush_note(0, note, 0);
        lush_render(pcm + b * 256);
    }
    write_wav(argv[7], pcm, frames);
    return 0;
}
