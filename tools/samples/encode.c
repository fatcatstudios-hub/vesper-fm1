/* Encode 32 kHz mono int16 raw PCM into the FM-1 SEA5 sample format.
 * Usage: encode in.raw out.sea  (prints frame count) */
#include <stdio.h>
#include <stdlib.h>
#define FM1_DSP_DATA
#include "sampledrum.h"
#include "sea_encode.h"
#include "sample_metadata.inc"
int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f) / 2; fseek(f, 0, SEEK_SET);
    int16_t *pcm = malloc(n * 2); if (fread(pcm, 2, n, f) != (size_t)n) return 1; fclose(f);
    uint8_t *sea = fm1_sea_encode(pcm, (unsigned)n, sample_dqt);
    if (!sea) return 1;
    unsigned bytes = (unsigned)((n + 319) / 320) * 228;
    f = fopen(argv[2], "wb"); fwrite(sea, 1, bytes, f); fclose(f);
    printf("%ld\n", n);
    return 0;
}
