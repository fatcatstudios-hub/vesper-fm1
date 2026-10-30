/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "fm_patch.h"
#include "fm_factory.inc"

/* Expand once during startup, before the audio owners are running. MIDI
 * program changes then read SRAM without decompression or heap allocation. */
#ifndef WORSHIP_SLIM
#define WORSHIP_SLIM 1
#endif
#if WORSHIP_SLIM
/* VESPER (1.1) never plays the DX7 factory voices. Keep only the first one
 * (any program number returns it) and give the other 16 KB of RAM to the
 * screen. The decode stops once one voice is out; a copy run can overshoot by
 * at most 130 bytes, hence the margin. */
enum { FACTORY_KEEP = 128, FACTORY_SPACE = 128 + 132 };
#else
enum { FACTORY_KEEP = FM1_FM_PROGRAMS*128, FACTORY_SPACE = FM1_FM_PROGRAMS*128 };
#endif
static uint8_t factory_raw[FACTORY_SPACE];
static unsigned factory_ready;
static FM1_DSP_CODE void factory_init(void)
{
    if (factory_ready) return;
    uint8_t *out=factory_raw;
    for (unsigned at=0;at<sizeof factory_packed && out<factory_raw+FACTORY_KEEP;) {
        unsigned token=factory_packed[at++];
        if (token<128) *out++=(uint8_t)token;
        else {
            unsigned low=factory_packed[at++],distance=low&127U;
            if (low&128U) distance|=(unsigned)factory_packed[at++]<<7;
            for (unsigned n=token-128+3;n;--n) { *out=out[-(int)distance]; ++out; }
        }
    }
    factory_ready=1;
}
FM1_DSP_CODE const uint8_t *groove_fm_factory_raw(unsigned program)
{
    factory_init();
#if WORSHIP_SLIM
    (void)program;
    return factory_raw;
#else
    return factory_raw+(program%FM1_FM_PROGRAMS)*128;
#endif
}
FM1_DSP_CODE void groove_fm_factory(uint8_t p[156], unsigned program)
{
    groove_fm_unpack(p,groove_fm_factory_raw(program));
}
