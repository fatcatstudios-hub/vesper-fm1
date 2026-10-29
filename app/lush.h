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
    LUSH_GLASS_PLUCK, /* analog pluck: saw/pulse, fast filter envelope */
    LUSH_WARM_BASS,   /* saw + sub, low filter */
    LUSH_SUB,         /* sine sub with a little warmth */
    LUSH_DETUNE_PLUCK,/* stereo unison pluck, wide detune */
    LUSH_SUPERSAW,    /* stereo supersaw sequence voice */
    LUSH_JUNO_ARP,    /* detuned pulse pair, Juno-style arp */
    LUSH_DETUNE_BASS, /* two detuned saws, punchy sequenced bass */
    LUSH_PIANO,       /* sampled soft upright piano (bank slots 1-4) */
    LUSH_MARIMBA,     /* sampled marimba (slots 5-7) */
    LUSH_GLOCK,       /* sampled glockenspiel, an octave up (slot 8) */
    LUSH_HALO_PLUCK,  /* soft synth pluck inside a big airy halo */
    LUSH_HANDPAN,     /* modal: tuned 1:2:3 metal tongue, long ring */
    LUSH_KALIMBA,     /* modal: tine with inharmonic overtones and a bright click */
    LUSH_VOX_OOH,     /* formant voice, "ooh" opening to "aah", delayed vibrato */
    LUSH_VOX_EE,      /* formant voice, "ee" rounding to "oh" */
    LUSH_MUTED_GTR,   /* palm-muted plucked string through an amp-style filter */
    LUSH_REV_PIANO,   /* sampled piano played backwards, swells into the next beat */
    LUSH_REV_GLOCK,   /* glockenspiel backwards */
    LUSH_PATCHES
};

/* Control task. Volume 0..127, sends 0..15. Patch 0 releases the track. */
void lush_layer(unsigned track, unsigned patch, unsigned volume, unsigned reverb, unsigned delay);
/* Control task. fx: reverb decay, delay byte, feedback, return level (0..127),
 * delay_ticks: applied delay time at 44.1 kHz / 8 (groove_delay_ticks()). */
void lush_effects(const uint8_t fx[4], unsigned delay_ticks, unsigned tempo, unsigned shimmer);
/* VESPER master FX (control task): grains, shimmer, width, chorus | hi pass, lo pass,
 * saturate, age | drive, bits, rate, mix; 0..127, 64 neutral for tone/width/chorus. */
enum { LUSH_MASTER_PARAMS = 12 };
void lush_master(const uint8_t v[LUSH_MASTER_PARAMS]);
unsigned lush_filter_hz(unsigned low_pass, unsigned v); /* TAPE HI/LO PASS cutoff for a knob value */
const char *lush_patch_name(unsigned patch);
const char *lush_track_name(unsigned track);
unsigned lush_track_level(unsigned track);   /* 0..127, live */
void lush_budget(unsigned voices_allowed);

/* VESPER voice editing (control task). Each layer plays an editable copy of
 * its sound; lush_voice_set picks the sound and resets the copy to its defaults. */
enum {
    LP_CUTOFF, LP_RESO, LP_ENV, LP_DRIVE, LP_SAW, LP_PULSE, LP_SINE, LP_NOISE,
    LP_UNISON, LP_DETUNE, LP_PW, LP_CHORUS, LP_A, LP_D, LP_S, LP_R,
    LP_FA, LP_FD, LP_FS, LP_FR, LP_RATE, LP_MOTION, LP_PAN, LP_DRIFT,
    LP_SPREAD, LP_BRIGHT, LP_DECAY, LP_VELOCITY, LP_GAIN,
    LUSH_PARAMS,
    LP_OCTAVE = LUSH_PARAMS, /* -2..+2 octaves, kept beside the patch */
    LUSH_VOICE_BYTES = 42
};
/* Control task: play a key live on a LUSH layer (velocity 0 = release). */
void lush_live_note(unsigned track, unsigned note, unsigned velocity);
void lush_voice_set(unsigned track, unsigned patch);
unsigned lush_voice_sound(unsigned track);
int lush_param_applies(unsigned track, unsigned id);
int lush_param_get(unsigned track, unsigned id);
int lush_param_edit(unsigned track, unsigned id, int delta);  /* returns the new value */
unsigned lush_voice_export(unsigned track, uint8_t *out);     /* LUSH_VOICE_BYTES, or 0 */
int lush_voice_import(unsigned track, const uint8_t *in);

/* Control task, from the sample bank loader: bind one of the eight shared
 * sample slots (memory-mapped SEA5 data, 32 kHz). NULL data unbinds; voices
 * playing that slot stop at their next decode. The zone's root note is read
 * from the end of its name ("PIANO C4"); without one it is C4. */
void lush_sample_bind(unsigned slot, const uint8_t *data, unsigned bytes, unsigned frames, const char *name);

/* Audio context. */
unsigned lush_owns(unsigned track);
void lush_note(unsigned track, unsigned note, unsigned velocity);
void lush_track_reset(unsigned track);
/* Transport messages (0xfa start, 0xfc stop): motion LFOs restart on the downbeat. */
void lush_transport(uint8_t status);
/* Adds FM1_BLOCK_FRAMES stereo frames of signed Q23 audio into output. */
void lush_render(int32_t *output);
#endif
