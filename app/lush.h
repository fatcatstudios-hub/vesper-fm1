/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* LUSH: analog-style and plucked-string voices for the WORSHIP firmware.
 * Integer-only DSP that runs in the APP output stage, after the closed FM1
 * engine. Tracks it owns are silenced in the engine (MIDI-only mode) and their
 * sequenced notes are played here instead, with LUSH's own chorus, plate reverb
 * and dark tempo delay, driven by the normal FX page and per-track sends. */
#ifndef LUSH_H
#define LUSH_H
#include <stdint.h>

enum {
    LUSH_NONE,
    LUSH_AIR_PAD,     /* 3 detuned saws, slow filter swell, chorus */
    LUSH_STRINGS,     /* saw + PWM pulse ensemble, chorus */
    LUSH_GLASS_PLUCK, /* analog pluck: saw/pulse, fast filter envelope */
    LUSH_HARP,        /* plucked string (Karplus-Strong), bright */
    LUSH_FELT,        /* plucked string, dark hammer, sine body */
    LUSH_WARM_BASS,   /* saw + sub, low filter */
    LUSH_SUB,         /* sine sub with a little warmth */
    LUSH_AIR_BELL,    /* bright string + octave sine shimmer */
    LUSH_DETUNE_PLUCK,/* stereo unison pluck, wide detune */
    LUSH_SUPERSAW,    /* stereo supersaw sequence voice */
    LUSH_JUNO_ARP,    /* detuned pulse pair, Juno-style arp */
    LUSH_DETUNE_BASS, /* two detuned saws, punchy sequenced bass */
    LUSH_PATCHES
};

/* Control task. Volume 0..127, sends 0..15. Patch 0 releases the track. */
void lush_layer(unsigned track, unsigned patch, unsigned volume, unsigned reverb, unsigned delay);
/* Control task. fx: reverb decay, delay byte, feedback, return level (0..127),
 * delay_ticks: applied delay time at 44.1 kHz / 8 (groove_delay_ticks()). */
void lush_effects(const uint8_t fx[4], unsigned delay_ticks);
const char *lush_patch_name(unsigned patch);

/* Audio context. */
unsigned lush_owns(unsigned track);
void lush_note(unsigned track, unsigned note, unsigned velocity);
void lush_track_reset(unsigned track);
/* Adds FM1_BLOCK_FRAMES stereo frames of signed Q23 audio into output. */
void lush_render(int32_t *output);
#endif
