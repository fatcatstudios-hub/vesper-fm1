/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* LUSH voice engine and effects. See lush.h.
 *
 * Signals are Q15 in int32 (1.0 = 32768) with headroom; envelopes are Q30.
 * Control-rate work (envelopes, filter cutoff, drift) runs every TICK
 * samples and is interpolated. No floating point, no allocation, no libc in
 * the audio path. Large delay memories are int16 and run at half rate. */
#include "lush.h"
#include "fm1.h"

#define LUSH_CODE FM1_AUDIO_CODE
/* FM1_AUDIO_CODE implies noinline. Per-sample helpers must inline into their
 * SRAM caller instead: on the FM-1 each call/return costs more than the math. */
#define LUSH_HOT __attribute__((always_inline))
/* Diagnostic switches for host measurements; the defaults are the shipped sound. */
#ifndef LUSH_DRIVE
#define LUSH_DRIVE 1
#endif
#ifndef LUSH_ANALOG
#define LUSH_ANALOG 1
#endif
#include "lush_tables.inc"
#include "lush_sea_dqt.inc"
#include "fm1_sea.h"
#include <string.h>
/* Phase increment for a MIDI note: the top octave (notes 120..131), shifted down. */
static inline LUSH_HOT uint32_t note_inc(unsigned note)
{
    if (note > 127) note = 127;
    return lush_octave[note % 12U] >> (10U - note / 12U);
}
__attribute__((minsize)) static void lush_tables_build(void)
{
    if (lush_env[0]) return;
    for (unsigned i = 0; i < 16; ++i) lush_env[i] = lush_env_head[i];
    uint64_t x = 133427561U; /* 1/tau at index 16, Q30 */
    for (unsigned i = 16; i < 64; ++i) {
        lush_env[i] = (uint32_t)(x - ((x * x) >> 31)); /* 1 - e^-x ~ x - x^2/2 (x <= 1/8) */
        x = (x * 912838509U) >> 30;
    }
    for (unsigned i = 0; i < 136; ++i) { /* sin(x) by series, x = pi * inc / 2^32 in Q16 */
        int32_t y = (int32_t)(((note_inc(i) >> 15) * 102948U + 32768U) >> 16);
        int32_t x2 = (y * y) >> 16, x3 = (x2 * y) >> 16, x5 = (x3 * x2) >> 16;
        int32_t c = y - x3 / 6 + x5 / 120; /* within 11 cents of the stored table above 60 Hz */
        lush_cutoff[i] = (uint16_t)(c > 31343 ? 31343 : c);
    }
}

enum { TICK = 16, VOICES = 16, KS_VOICES = 8, KS_LEN = 1024, TRACKS = 8 };
enum { STAGE_OFF, STAGE_ATTACK, STAGE_DECAY, STAGE_SUSTAIN, STAGE_RELEASE };
#define ONE30 (1 << 30)

struct patch {
    uint8_t ks;                     /* 0 analog oscillators, 1 plucked string, 2 sample */
    uint8_t saw, pulse, sine, noise;/* mix levels 0..127 */
    uint8_t unison, detune, drift;  /* saws 1..3, cents, cents */
    uint8_t pw;                     /* pulse width 0..127 (127 = square) */
    int8_t sine_octave;             /* -1 sub octave, 0 unison, 1 up */
    uint8_t cutoff, keytrack;       /* MIDI-note cutoff, 0..128 tracking */
    int8_t env_amount;              /* filter envelope depth, semitones */
    uint8_t resonance, drive;       /* 0..127 */
    uint8_t fenv[4];                /* filter env: attack, decay (time idx), sustain 0..127, release */
    uint8_t aenv[4];                /* amp env, same layout */
    uint8_t velocity, gain, chorus, spread;
    uint8_t bright, decay;          /* plucked string: excitation tone, sustain */
    uint8_t stereo;                 /* 1: unison saws split left/right, two filters */
    uint8_t slot, slots;            /* sample patches: first bank slot and zone count */
    uint8_t motion, motion_div;     /* tempo-synced cutoff LFO: semitones, 1=16th..7=4 bars */
    uint8_t pan_move;               /* slow 2-bar auto-pan depth 0..127 */
    uint8_t modal;                  /* ks 3: 1 handpan, 2 kalimba */
    uint8_t formant;                /* 1 ooh->aah, 2 ee->oh: vowel filters replace the low-pass */
    uint8_t reverse;                /* sample patches: swell length in beats, played backwards */
    int8_t pos;                     /* EDIT MIX PAN: -64 left .. 0 centre .. +63 right (0.8.8) */
};
/* Warm voicing: soft 5-10 ms attacks (no clicks), darker cutoffs, little
 * resonance, more saturation, analog drift on every oscillator voice. */
static FM1_AUDIO_DATA const struct patch patches[LUSH_PATCHES] = {
    /*                    ks saw pul sin noi uni det drf pw oct cut key env res drv  fenv              aenv               vel gain ch spr br dec st */
    [LUSH_GLASS_PLUCK]  = {0,  96, 44,  0,  0, 2,  8, 5, 64, 0, 50,100, 36, 26, 44, {18,72,10,76},   {24,88,0,80},      90, 62, 1, 70,  0,  0, 0, 0, 0, 10, 4, 40, 0,0,0,0},
    [LUSH_WARM_BASS]    = {0,  86,  0, 90,  0, 1,  0, 3,  0,-1, 44,100, 22, 14, 40, {14,70,30,64},   {22,80,124,68},    72, 64, 0,  0,  0,  0, 0, 0, 0, 0,0, 0, 0,0,0,0},
    [LUSH_SUB]          = {0,  22,  0,127,  0, 1,  0, 2,  0, 0, 48,100,  4,  0, 10, {60,80,64,80},   {70,90,127,94},    40, 70, 0,  0,  0,  0, 0, 0, 0, 0,0, 0, 0,0,0,0},
    [LUSH_DETUNE_PLUCK] = {0, 104, 30,  0,  0, 3, 12, 7, 60, 0, 52,104, 34, 22, 48, {20,74,12,76},   {24,90,0,82},      92, 64, 0,  0,  0,  0, 1, 0, 0, 12, 5, 0, 0,0,0,0},
    [LUSH_SUPERSAW]     = {0, 112,  0, 16,  0, 3, 15, 8,  0, 0, 64, 92, 24, 14, 46, {20,82,40,82},   {26,86,56,84},     80, 64, 0,  0,  0,  0, 1, 0, 0, 14, 6, 0, 0,0,0,0},
    [LUSH_JUNO_ARP]     = {0,  40,110,  0,  0, 2, 9, 7, 92, 0, 58,100, 32, 30, 44, {12,76,14,76},   {24,86,30,78},     88, 58, 1, 64,  0,  0, 0, 0, 0, 12, 2, 30, 0,0,0,0},
    /* Sampled: transient kept (fast attack), gentle velocity-tracked low-pass. sine_octave = transpose. */
    [LUSH_PIANO]        = {2,   0,  0,  0,  0, 1,  0, 0,  0, 0,108, 64,  0,  0,  8, {0,0,127,0},     {4,100,127,84},    72, 124, 0, 60,  0,  0, 0, 0, 4, 6, 6, 50, 0,0,0,0},
    [LUSH_MARIMBA]      = {2,   0,  0,  0,  0, 1,  0, 0,  0, 0,112, 64,  0,  0,  6, {0,0,127,0},     {4,100,127,78},    70, 122, 0, 70,  0,  0, 0, 4, 3, 8, 5, 60, 0,0,0,0},
    [LUSH_GLOCK]        = {2,   0,  0,  0,  0, 1,  0, 0,  0, 1,118, 64,  0,  0,  0, {0,0,127,0},     {4,100,127,92},    64, 92, 1, 90,  0,  0, 0, 7, 1, 0, 0, 80, 0,0,0,0},
    /* Omnisphere-style hybrids: the air halo is the point. */
    [LUSH_HALO_PLUCK]   = {0,  70, 40,  0,  0, 2, 10, 6, 70, 0, 48, 96, 40, 30, 40, {16,74,8,78},    {20,82,0,80},      86, 38, 1, 70,  0,  0, 0, 0, 0, 10,6, 70, 0,0,0,0},
    /* Beyond GM. ks 3 = modal (tuned partials), formant voices use the oscillator path. */
    [LUSH_HANDPAN]      = {3,   0,  0,  0,  0, 1,  0, 0,  0, 0,  0,  0,  0,  0,  0, {0,0,127,0},     {14,100,127,86},   70,54, 0, 70,  0,  0, 0, 0, 0, 0,0, 40, 1,0,0,0},
    [LUSH_KALIMBA]      = {3,   0,  0,  0,  0, 1,  0, 0,  0, 0,  0,  0,  0,  0,  0, {0,0,127,0},     {4,100,127,80},    76,62, 0, 80,  0,  0, 0, 0, 0, 0,0, 50, 2,0,0,0},
    [LUSH_VOX_OOH]      = {0,  96,  0,  0,  0, 2,  6, 3,  0, 0,  0,  0,  0,  0,  0, {78,96,100,84},  {40,90,110,84},    60,100, 1, 80,  0,  0, 0, 0, 0, 0,0, 40, 0,1,0,0},
    [LUSH_VOX_EE]       = {0,  96,  0,  0,  0, 2,  6, 3,  0, 0,  0,  0,  0,  0,  0, {80,96,100,84},  {40,90,110,84},    60,100, 1, 80,  0,  0, 0, 0, 0, 0,0, 40, 0,2,0,0},
    [LUSH_MUTED_GTR]    = {1,   0,  0,  0,  0, 1,  0, 2,  0, 0, 82, 64,  0, 40, 30, {0,0,127,0},     {6,100,0,64},      90,127, 0, 50, 76, 96, 0, 0, 0, 0,0, 30, 0,0,0,0},
    [LUSH_REV_PIANO]    = {2,   0,  0,  0,  0, 1,  0, 0,  0, 0,108, 64,  0,  0,  0, {0,0,127,0},     {40,100,127,70},   60,120, 0, 70,  0,  0, 0, 0, 4, 0,0, 60, 0,0,2,0},
    [LUSH_REV_GLOCK]    = {2,   0,  0,  0,  0, 1,  0, 0,  0, 1,118, 64,  0,  0,  0, {0,0,127,0},     {40,100,127,70},   60,100, 1, 80,  0,  0, 0, 7, 1, 0,0, 60, 0,0,2,0},
    [LUSH_DETUNE_BASS]  = {0, 104,  0, 70,  0, 2, 11, 4,  0,-1, 42,100, 26, 20, 46, {14,68,20,62},   {22,76,90,64},     84, 88, 0,  0,  0,  0, 0, 0, 0, 0,0, 0, 0,0,0,0},
};
static const char *const names[LUSH_PATCHES] = {
    "", "GLASS PLUCK", "WARM BASS", "SUB", "DETUNE PLUCK", "SUPERSAW", "JUNO ARP", "DETUNE BASS",
    "PIANO", "MARIMBA", "GLOCK", "HALO PLUCK",
    "HANDPAN", "KALIMBA", "VOX OOH", "VOX EE", "MUTED GTR", "REV PIANO", "REV GLOCK"
};
/* Modal voices: partial ratio (x256), level (0..127), decay time index. */
struct modal_voice { uint16_t ratio[4]; uint8_t level[4], decay[4]; };
static FM1_AUDIO_DATA const struct modal_voice modal_voices[3] = {
    [1] = {{256, 512, 770, 1331}, {100, 54, 30, 8}, {106, 98, 90, 72}},  /* handpan: 1, 2, 3 + a faint shimmer */
    [2] = {{256, 1382, 512, 3379}, {100, 26, 12, 16}, {94, 66, 84, 44}}, /* kalimba: tine 1, 5.4, 13.2 + body octave */
};
/* Vowel formants F1..F3 as MIDI note x256: from vowel, to vowel. */
static FM1_AUDIO_DATA const uint16_t vowels[3][2][3] = {
    [1] = {{15967, 20685, 24877}, {19908, 21684, 25256}},  /* ooh -> aah */
    [2] = {{15500, 24975, 26186}, {18811, 20530, 25201}},  /* ee  -> oh  */
};
const char *lush_patch_name(unsigned patch) { return patch < LUSH_PATCHES ? names[patch] : "?"; }

/* ---- layer and effect parameters (written by control, read by audio) ---- */
static struct {
    volatile uint8_t patch, applied;
    volatile int32_t gain, reverb, delay; /* Q15 */
    uint8_t pan_next;
} layer[TRACKS];
static uint8_t track_level[TRACKS]; /* live level per layer, for the home screen */
/* VESPER voices: each layer plays an editable copy of its patch. */
static struct patch voice_patch[TRACKS];
_Static_assert(LUSH_VOICE_BYTES == 4 + sizeof(struct patch), "voice blob layout");
static int8_t voice_octave[TRACKS];
static volatile int32_t fx_decay = 26000, fx_feedback = 12000, fx_wet = 20000, fx_damp = 9000;
static volatile uint32_t delay_q16 = 11025U << 16; /* half-rate samples, Q16 */
static volatile uint32_t beat_inc = 0;    /* per-sample step of a 16-beat (4-bar) phase */
static volatile int32_t fx_shimmer = 0;   /* Q15 */
static volatile int32_t fx_grains = 0;    /* CLOUD GRAINS amount, 0..127 */
/* VESPER master FX (0.8), set by lush_master(): CLOUD, TAPE and DIRT pages. */
static volatile int32_t fx_width = 32768, fx_chorus = 19661, fx_chorus_depth = 160;
static volatile int32_t fx_hp = 0, fx_lp = 32767;
static volatile int32_t fx_level_thr = 0, fx_out_gain = 4096; /* leveler threshold (Q15, 0 = off), OUTPUT (Q12) */
static volatile int32_t fx_drive = 0, fx_crush = 0, fx_rate_inc = 65536, fx_dirt_mix = 32767; /* rate: Q16 per sample, 65536 = full rate */
static volatile int32_t fx_drive_gain = 256, fx_drive_comp = 32767; /* gain Q8, make-up Q15 */
static uint32_t beat_phase;
static volatile int32_t grain_32nd = 1378; /* CLOUD GRAINS: a 32nd note in 22 kHz samples, from the tempo */

/* The LUSH sound on a track, or NULL when the track plays the FM1 engine. */
const char *lush_track_name(unsigned track)
{
    return track < TRACKS && layer[track].patch != LUSH_NONE && layer[track].patch < LUSH_PATCHES ? names[layer[track].patch] : 0;
}
unsigned lush_track_level(unsigned track) { return track < TRACKS ? track_level[track] : 0U; }
LUSH_CODE unsigned lush_owns(unsigned track) { return track < TRACKS && layer[track].patch != LUSH_NONE; }

__attribute__((minsize)) void lush_layer(unsigned track, unsigned patch, unsigned volume, unsigned reverb, unsigned delay)
{
    lush_tables_build();
    if (track >= TRACKS) return;
    if (patch >= LUSH_PATCHES) patch = LUSH_NONE;
    if (volume > 127) volume = 127;
    layer[track].gain = (int32_t)(volume * volume * 2U); /* gentle taper, 127 ~ 0.98 */
    layer[track].reverb = (int32_t)((reverb > 15 ? 15 : reverb) * 2184U);
    layer[track].delay = (int32_t)((delay > 15 ? 15 : delay) * 2184U);
    if (patch == LUSH_NONE) layer[track].patch = LUSH_NONE;
    else if (layer[track].patch == LUSH_NONE) lush_voice_set(track, patch);
}
/* ---- VESPER voice editing (control task) ---- */
__attribute__((minsize)) void lush_voice_set(unsigned track, unsigned patch)
{
    if (track >= TRACKS || patch == LUSH_NONE || patch >= LUSH_PATCHES) return;
    lush_tables_build();
    layer[track].patch = LUSH_NONE;   /* audio retires the old voices first */
    voice_patch[track] = patches[patch];
    voice_octave[track] = 0;
    layer[track].patch = (uint8_t)patch;
}
unsigned lush_voice_sound(unsigned track) { return track < TRACKS ? layer[track].patch : LUSH_NONE; }
/* Parameter table: byte offset into struct patch, range. */
#define PO(f) (uint8_t)__builtin_offsetof(struct patch, f)
static const struct { uint8_t at; int8_t min, max; uint8_t kinds; } params[LUSH_PARAMS] = {
    /* kinds: bit per ks type (1 synth, 2 string, 4 sample, 8 modal) */
    [LP_CUTOFF] = {PO(cutoff), 0, 127, 7},     [LP_RESO] = {PO(resonance), 0, 120, 3},
    [LP_ENV] = {PO(env_amount), -60, 60, 3},   [LP_DRIVE] = {PO(drive), 0, 127, 3},
    [LP_SAW] = {PO(saw), 0, 127, 1},           [LP_PULSE] = {PO(pulse), 0, 127, 1},
    [LP_SINE] = {PO(sine), 0, 127, 3},         [LP_NOISE] = {PO(noise), 0, 127, 1},
    [LP_UNISON] = {PO(unison), 1, 3, 1},       [LP_DETUNE] = {PO(detune), 0, 40, 1},
    [LP_PW] = {PO(pw), 0, 127, 1},             [LP_CHORUS] = {PO(chorus), 0, 1, 15},
    [LP_A] = {PO(aenv[0]), 0, 127, 15},        [LP_D] = {PO(aenv[1]), 0, 127, 15},
    [LP_S] = {PO(aenv[2]), 0, 127, 15},        [LP_R] = {PO(aenv[3]), 0, 127, 15},
    [LP_FA] = {PO(fenv[0]), 0, 127, 3},        [LP_FD] = {PO(fenv[1]), 0, 127, 3},
    [LP_FS] = {PO(fenv[2]), 0, 127, 3},        [LP_FR] = {PO(fenv[3]), 0, 127, 3},
    [LP_RATE] = {PO(motion_div), 0, 7, 7},     [LP_MOTION] = {PO(motion), 0, 24, 7},
    [LP_PAN] = {PO(pan_move), 0, 127, 15},     [LP_DRIFT] = {PO(drift), 0, 20, 3},
    [LP_SPREAD] = {PO(spread), 0, 127, 15},    [LP_BRIGHT] = {PO(bright), 0, 127, 2},
    [LP_DECAY] = {PO(decay), 0, 127, 2},       [LP_VELOCITY] = {PO(velocity), 0, 127, 15},
    [LP_GAIN] = {PO(gain), 0, 127, 15},        [LP_POS] = {PO(pos), -64, 63, 15},
};
int lush_param_applies(unsigned track, unsigned id)
{
    if (track >= TRACKS || layer[track].patch == LUSH_NONE) return 0;
    if (id == LP_OCTAVE) return 1;
    return id < LUSH_PARAMS && (params[id].kinds >> voice_patch[track].ks & 1U);
}
int lush_param_span(unsigned id) { return id < LUSH_PARAMS ? params[id].max - params[id].min : 4; }
int lush_param_get(unsigned track, unsigned id)
{
    if (track >= TRACKS) return 0;
    if (id == LP_OCTAVE) return voice_octave[track];
    if (id >= LUSH_PARAMS) return 0;
    const uint8_t *b = (const uint8_t *)&voice_patch[track] + params[id].at;
    return params[id].min < 0 ? (int)(int8_t)*b : (int)*b;
}
__attribute__((minsize)) int lush_param_edit(unsigned track, unsigned id, int delta)
{
    if (!delta || !lush_param_applies(track, id)) return lush_param_get(track, id);
    int lo = id == LP_OCTAVE ? -2 : params[id].min, hi = id == LP_OCTAVE ? 2 : params[id].max;
    int v = lush_param_get(track, id) + delta;
    v = v < lo ? lo : v > hi ? hi : v;
    if (id == LP_OCTAVE) {
        if (v != voice_octave[track]) { voice_octave[track] = (int8_t)v; lush_track_reset(track); }
    } else ((uint8_t *)&voice_patch[track])[params[id].at] = (uint8_t)v;
    return v;
}
/* Project storage: 'V', 1, sound, octave, then the patch bytes. */
unsigned lush_voice_export(unsigned track, uint8_t *out)
{
    if (track >= TRACKS || layer[track].patch == LUSH_NONE) return 0;
    out[0] = 'V'; out[1] = 2; out[2] = layer[track].patch; out[3] = (uint8_t)voice_octave[track];
    memcpy(out + 4, &voice_patch[track], sizeof(struct patch));
    return LUSH_VOICE_BYTES;
}
__attribute__((minsize)) int lush_voice_import(unsigned track, const uint8_t *in)
{
    if (track >= TRACKS || in[0] != 'V' || (in[1] != 1 && in[1] != 2) || in[2] == LUSH_NONE || in[2] >= LUSH_PATCHES) return -1;
    lush_voice_set(track, in[2]);
    layer[track].patch = LUSH_NONE;
    struct patch edited, *v = &voice_patch[track];
    memcpy(&edited, in + 4, sizeof edited);
    if (in[1] == 1) edited.pos = 0; /* 0.7 blobs had no PAN byte: centre */
    /* Structural fields always come from the sound itself. */
    edited.ks = v->ks; edited.slot = v->slot; edited.slots = v->slots; edited.modal = v->modal;
    edited.formant = v->formant; edited.reverse = v->reverse; edited.stereo = v->stereo;
    edited.sine_octave = v->sine_octave;
    for (unsigned id = 0; id < LUSH_PARAMS; ++id) { /* clamp anything out of range */
        uint8_t *b = (uint8_t *)&edited + params[id].at;
        int x = params[id].min < 0 ? (int)(int8_t)*b : (int)*b;
        if (x < params[id].min || x > params[id].max) *b = ((uint8_t *)v)[params[id].at];
    }
    *v = edited;
    int8_t oct = (int8_t)in[3];
    voice_octave[track] = oct < -2 || oct > 2 ? 0 : oct;
    layer[track].patch = in[2];
    return 0;
}
/* TAPE page filters: HI PASS 20 Hz .. 2.6 kHz, LO PASS 300 Hz .. open,
 * exponential across the knob. */
__attribute__((minsize)) unsigned lush_filter_hz(unsigned low_pass, unsigned v)
{
    uint32_t hz = low_pass ? 300U << 8 : 20U << 8, step = low_pass ? 265U : 266U; /* Q8 ratios ~1.035 / ~1.039 */
    for (unsigned i = 0; i < v && i < 127U; ++i) hz = (hz * step) >> 8;
    return hz >> 8;
}
/* DIRT BITS: bits taken away, 0 (16-bit) .. 12 (4-bit), one per ~10 knob values. */
unsigned lush_crush_bits(unsigned v) { return (v > 127U ? 127U : v) * 12U / 127U; }
/* DIRT RATE: sample rate in Hz, 44100 at 0 falling evenly (exponentially) to ~1.3 kHz. */
__attribute__((minsize)) unsigned lush_rate_hz(unsigned v)
{
    uint32_t hz = 44100U << 8;
    for (unsigned i = 0; i < v && i < 127U; ++i) hz = (hz * 249U) >> 8;
    return hz >> 8;
}
/* One-pole coefficient (Q15) for a cutoff at 44.1 kHz: ~2*pi*f/fs, eased near the top. */
static __attribute__((minsize)) int32_t filter_coefficient(unsigned hz)
{
    uint32_t a = hz * 4669U / 1000U;              /* 2*pi*32768/44100 = 4.669 */
    if (a > 16384U) a = 16384U + (a - 16384U) / 3U;
    return a > 32767U ? 32767 : (int32_t)a;
}
/* VESPER master FX. v: grains, shimmer, width, chorus | hi pass, lo pass, level, output |
 * age | drive, bits, rate, mix; each 0..127 (64 = neutral for tone, width, chorus). */
__attribute__((minsize)) void lush_master(const uint8_t v[LUSH_MASTER_PARAMS])
{
    fx_grains = v[0];                                   /* CLOUD GRAINS; v[1] SHIMMER goes through lush_effects */
    fx_width = (int32_t)v[2] * 512;                     /* 64 -> 1.0, 127 -> ~2.0 */
    /* CHORUS: 64 is the scene's chorus; above that it gets much deeper and wetter */
    fx_chorus = v[3] <= 64 ? (int32_t)v[3] * 307 : 19648 + ((int32_t)v[3] - 64) * 700;
    fx_chorus_depth = v[3] <= 64 ? 160 : 160 + ((int32_t)v[3] - 64) * 2;
    fx_hp = v[4] ? filter_coefficient(lush_filter_hz(0, v[4])) : 0;       /* HI PASS, 0 = off */
    fx_lp = v[5] < 127 ? filter_coefficient(lush_filter_hz(1, v[5])) : 32767; /* LO PASS, 127 = off */
    {   /* LEVEL: threshold falls ~0.17 dB a step to about -22 dB; OUTPUT ~0.19 dB a step around 0 dB */
        int32_t thr = 32768;
        for (unsigned i = 0; i < v[6]; ++i) thr = (thr * 251) >> 8;
        fx_level_thr = v[6] ? thr : 0;
        int32_t out = 4096;
        for (unsigned i = 64; i < v[7]; ++i) out = (out * 262) >> 8;
        for (unsigned i = v[7]; i < 64; ++i) out = (out * 251) >> 8;
        fx_out_gain = out;
    }
    fx_drive = (int32_t)v[8] * 12;                      /* pre-gain into the clipper */
    fx_drive_gain = 256 + fx_drive * 2;                 /* 1x .. ~13x, Q8 */
    fx_drive_comp = (int32_t)((32767L * 256) / (256 + fx_drive)); /* hold the level as it clips */
    fx_crush = (int32_t)lush_crush_bits(v[9]);            /* bits removed: 0..12, even steps */
    fx_rate_inc = (int32_t)(((uint32_t)lush_rate_hz(v[10]) << 16) / 44100U); /* RATE: 44.1 kHz down to ~1.3 kHz */
    fx_dirt_mix = (int32_t)v[11] * 258;
}
__attribute__((minsize)) void lush_effects(const uint8_t fx[4], unsigned delay_ticks, unsigned tempo, unsigned shimmer)
{
    lush_tables_build();
    if (tempo >= 30 && tempo <= 300)
        beat_inc = (uint32_t)(((uint64_t)tempo << 32) / (60ULL * 44100ULL * 16ULL));
    if (tempo >= 30 && tempo <= 300) grain_32nd = (int32_t)(165375U / tempo); /* 22050 * 60 / (tempo * 8) */
    fx_shimmer = (int32_t)(shimmer > 127 ? 127 : shimmer) * 180;   /* 0 .. 0.70: band-limited, so it can bloom */
    fx_decay = 15000 + (int32_t)fx[0] * 121;   /* 0.46 .. 0.93 */
    fx_damp = 13000 - 3960 - (int32_t)fx[0] * 40; /* longer tails are a touch brighter; 0.8.3 TONE +33 baked in */
    fx_feedback = (int32_t)fx[2] * 214;        /* 0 .. 0.83 */
    fx_wet = (int32_t)fx[3] * 240;             /* 0 .. 0.93 */
    uint32_t half = delay_ticks * 4U;          /* 44.1k/8 ticks -> 22.05k samples */
    if (half < 64) half = 64;
    if (half > 17900) half = 17900; /* the right tap runs at 4/3 of this */
    delay_q16 = half << 16;
}

/* ---- voices ---- */
struct envelope { int32_t level; uint8_t stage; };
struct voice {
    uint8_t active, track, note, patch, pan;
    uint8_t kill;                     /* stolen: fading out in a few ms */
    uint16_t age;
    int32_t velocity;                 /* Q15 */
    uint32_t phase[4], base;          /* saw 0..2, sine 3; base increment */
    uint32_t inc[4];
    struct envelope amp, filter;
    int32_t amp_now;                  /* Q15, interpolated per sample */
    int32_t f, q;                     /* filter coefficients, Q15 */
    int32_t lp1, bp1, lp2, bp2;       /* left / mono filter */
    int32_t rl1, rb1, rl2, rb2;       /* right filter for stereo unison */
    int32_t last_l, last_r;           /* half-rate synth: previous computed sample */
    int32_t drift, drift_target;      /* slow wander, cents Q8 */
    int32_t offset;                   /* fixed per-note tuning offset, cents Q8 */
    uint16_t drift_timer;             /* ticks until the wander picks a new target */
    uint32_t random;
    /* plucked string */
    uint32_t ks_delay;                /* Q16 samples */
    uint16_t ks_write;
    int32_t ks_lp, ks_gain, ks_coef;
    /* sample */
    uint8_t zone;
    uint32_t sample_step, sample_pos, sample_frac, sample_next; /* step and fraction Q16 */
    int32_t sample_state[8];
    int16_t sample_pcm[2 * FM1_SEA_GROUP];
    int32_t modal_level[4];           /* Q30 */
    int32_t formant_f[3];             /* Q15 coefficients */
    uint32_t vibrato_phase;
    int32_t vibrato_depth;            /* cents Q8, ramps in */
    int32_t reverse_chunk;            /* decoded chunk cached in ks_memory, -1 none */
};
_Static_assert(__builtin_offsetof(struct voice, drift_target) == __builtin_offsetof(struct voice, lp1) + 11 * 4,
               "start_voice clears lp1..drift_target as one block");
static struct voice voices[VOICES] VESPER_BIG;
static uint16_t voice_clock;
/* Adaptive voice budget: the control task lowers it when the FM-1 reports audio
 * overruns and raises it again after a calm stretch, so busy passages trim
 * overlapping tails instead of glitching. Every voice type counts, so sample-
 * heavy scenes are covered too. */
static volatile uint8_t synth_budget = 10;
void lush_budget(unsigned voices_allowed) { synth_budget = (uint8_t)(voices_allowed < 3U ? 3U : voices_allowed > VOICES ? VOICES : voices_allowed); }
static LUSH_CODE struct voice *oldest_synth(unsigned *count)
{
    struct voice *oldest = 0; unsigned n = 0;
    for (unsigned i = 0; i < VOICES; ++i) {
        struct voice *o = &voices[i];
        if (!o->active || o->kill) continue;
        ++n;
        /* Prefer a released note; among equals, the oldest. */
        if (!oldest || (o->amp.stage == STAGE_RELEASE) > (oldest->amp.stage == STAGE_RELEASE) ||
            ((o->amp.stage == STAGE_RELEASE) == (oldest->amp.stage == STAGE_RELEASE) &&
             (uint16_t)(voice_clock - o->age) > (uint16_t)(voice_clock - oldest->age))) oldest = o;
    }
    *count = n;
    return oldest;
}
#if FM1_HOST_TEST
uint32_t lush_stat[2]; /* voice-blocks rendered: synth/string/modal, sample */
#endif
static int16_t ks_memory[KS_VOICES][KS_LEN] __attribute__((aligned(4))) VESPER_BIG; /* cleared as words */

static inline LUSH_HOT int32_t clamp15(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }
static inline LUSH_HOT uint32_t rng(uint32_t *s) { *s = *s * 1664525U + 1013904223U; return *s; }
static inline LUSH_HOT int32_t noise15(uint32_t *s) { return (int32_t)(rng(s) >> 17) - 16384; }

/* ---- shared sample bank (bound by app/samples.c) ---- */
enum { BANK_SLOTS = 8, SEA_CHUNK_BYTES = 228 };
static struct { const uint8_t *volatile data; uint32_t bytes, frames; uint8_t root; } bank[BANK_SLOTS];
static unsigned root_from_name(const char *name)
{
    /* Last "<A-G>[#|b]<octave>" token in the name, C4 = 60. */
    static const uint8_t base[7] = {9, 11, 0, 2, 4, 5, 7};
    int found = -1;
    for (unsigned i = 0; name && name[i]; ++i) {
        char c = name[i];
        if (c >= 'a' && c <= 'g') c = (char)(c - 32);
        if (c < 'A' || c > 'G' || (i && name[i - 1] != ' ' && name[i - 1] != '_' && name[i - 1] != '-')) continue;
        unsigned j = i + 1; int pc = base[c - 'A'];
        if (name[j] == '#') { ++pc; ++j; } else if (name[j] == 'b') { --pc; ++j; }
        int sign = 1, octave = 0, digits = 0;
        if (name[j] == '-') { sign = -1; ++j; }
        while (name[j] >= '0' && name[j] <= '9') { octave = octave * 10 + (name[j++] - '0'); ++digits; }
        if (digits && (!name[j] || name[j] == ' ' || name[j] == '_'))
            found = (sign * octave + 1) * 12 + pc;
    }
    return found >= 0 && found <= 127 ? (unsigned)found : 60U;
}
__attribute__((minsize)) void lush_sample_bind(unsigned slot, const uint8_t *data, unsigned bytes, unsigned frames, const char *name)
{
    if (slot >= BANK_SLOTS) return;
    lush_dqt_build();
    bank[slot].data = 0; /* voices on this slot stop at their next decode */
    if (!data || !frames || bytes < (frames + 319U) / 320U * SEA_CHUNK_BYTES) return;
    bank[slot].bytes = bytes; bank[slot].frames = frames; bank[slot].root = (uint8_t)root_from_name(name);
    bank[slot].data = data;
}

/* Audio code must not call into Flash. Plain clearing loops become memset
 * calls (memset lives in Flash), so clear with a barrier the compiler keeps. */
static LUSH_CODE void sram_clear(void *at, unsigned words)
{
    int32_t *w = (int32_t *)at;
    for (unsigned i = 0; i < words; ++i) { w[i] = 0; __asm__ volatile("" ::: "memory"); }
}
static inline LUSH_HOT int32_t sine15(uint32_t phase)
{
    /* 256-step cycle from a quarter-wave table, mirrored by quadrant, interpolated. */
    unsigned i = phase >> 24, frac = (phase >> 8) & 0xffffU, k = i & 63U;
    int32_t a, b;
    switch (i >> 6) {
    case 0:  a = lush_quarter[k];       b = lush_quarter[k + 1];  break;
    case 1:  a = lush_quarter[64 - k];  b = lush_quarter[63 - k]; break;
    case 2:  a = -lush_quarter[k];      b = -lush_quarter[k + 1]; break;
    default: a = -lush_quarter[64 - k]; b = -lush_quarter[63 - k]; break;
    }
    return a + (((b - a) * (int32_t)frac) >> 16);
}
/* Band-limited sawtooth (polyBLEP), Q15. */
static inline LUSH_HOT int32_t saw15(uint32_t ph, uint32_t inc)
{
    int32_t s = (int32_t)(ph >> 16) - 32768;
    if (ph < inc) {
        int32_t x = (int32_t)(((uint64_t)ph << 15) / inc);
        s -= 2 * x - ((x * x) >> 15) - 32768;
    } else if (ph > 0xffffffffU - inc) {
        int32_t u = (int32_t)(((uint64_t)(0xffffffffU - ph) << 15) / inc);
        int32_t a = 32768 - u;
        s -= (a * a) >> 15;
    }
    return s;
}
static inline LUSH_HOT uint32_t detune(uint32_t inc, int32_t cents_q8)
{
    /* 2^(c/1200) ~ 1 + c*ln2/1200 for the few tens of cents used here. */
    int64_t ratio = (int64_t)ONE30 + (((int64_t)cents_q8 * 620213) >> 8);
    return (uint32_t)(((uint64_t)inc * (uint64_t)ratio) >> 30);
}
static LUSH_CODE void envelope_tick(struct envelope *e, const uint8_t t[4])
{
    switch (e->stage) {
    case STAGE_ATTACK: {
        int32_t target = ONE30 + (ONE30 >> 2); /* aim past 1.0 for an analog-shaped rise */
        e->level += (int32_t)(((int64_t)(target - e->level) * lush_env[t[0] >> 1]) >> 30);
        if (e->level >= ONE30) { e->level = ONE30; e->stage = STAGE_DECAY; }
        break;
    }
    case STAGE_DECAY: {
        int32_t target = (int32_t)t[2] << 23;
        e->level += (int32_t)(((int64_t)(target - e->level) * lush_env[t[1] >> 1]) >> 30);
        break;
    }
    case STAGE_RELEASE:
        e->level -= (int32_t)(((int64_t)e->level * lush_env[t[3] >> 1]) >> 30);
        break;
    default: break;
    }
}
static inline LUSH_HOT int32_t coef_q8(int32_t c)
{
    if (c < 0) c = 0;
    if (c > (134 << 8)) c = 134 << 8;
    unsigned i = (unsigned)c >> 8, frac = (unsigned)c & 255U;
    return lush_cutoff[i] + (((lush_cutoff[i + 1] - lush_cutoff[i]) * (int32_t)frac) >> 8);
}
static LUSH_CODE void filter_tick(struct voice *v, const struct patch *p)
{
    /* Cutoff in Q8 semitones: base + key tracking + envelope + drift. */
    int32_t c = ((int32_t)p->cutoff << 8) + ((((int32_t)v->note - 60) * p->keytrack) << 1)
              + (int32_t)(((int64_t)p->env_amount * (v->filter.level >> 14)) >> 8)
              + (v->drift >> 1) + ((v->velocity >> 7) * p->velocity >> 6);
    if (p->motion && p->motion_div)  /* tempo-locked filter movement */
        c += (int32_t)p->motion * (sine15(beat_phase << (7U - p->motion_div)) >> 7);
    if (p->ks == 0) c += 12 << 8;   /* synth voices run at half rate: same cutoff in Hz */
    if (c < 0) c = 0;
    if (c > (134 << 8)) c = 134 << 8;
    unsigned i = (unsigned)c >> 8, frac = (unsigned)c & 255U;
    v->f = lush_cutoff[i] + (((lush_cutoff[i + 1] - lush_cutoff[i]) * (int32_t)frac) >> 8);
    v->q = 42000 - p->resonance * 290;
}
static LUSH_CODE void voice_tick(struct voice *v, const struct patch *p)
{
    /* Slow random pitch drift, like an analog oscillator settling. */
    /* A new target every 0.6..2 s, approached over ~0.4 s: wander, not warble. */
    if (!v->drift_timer--) {
        v->drift_target = (int32_t)((rng(&v->random) >> 16) % (2U * p->drift * 256U + 1U)) - p->drift * 256;
        v->drift_timer = (uint16_t)(1650U + (rng(&v->random) >> 20) % 3850U);
    }
    v->drift += (v->drift_target - v->drift) >> 10;
    envelope_tick(&v->amp, p->aenv);
    if (p->ks != 1) {
        envelope_tick(&v->filter, p->fenv);
        filter_tick(v, p);
        int32_t spread = p->detune * 256;
        int32_t vibrato = 0;
        if (p->formant) {
            /* Singer vibrato: 5.2 Hz, fading in to about +-14 cents over ~0.4 s. */
            v->vibrato_phase += 1931000U * TICK;
            if (v->vibrato_depth < 14 * 256) v->vibrato_depth += 3;
            vibrato = (sine15(v->vibrato_phase) * v->vibrato_depth) >> 15;
            /* Vowel morph follows the filter envelope; motion adds movement. */
            int32_t morph = v->filter.level >> 15; /* 0..32768 */
            const uint16_t (*vw)[3] = vowels[p->formant];
            for (unsigned k = 0; k < 3; ++k) {
                int32_t c = vw[0][k] + ((((int32_t)vw[1][k] - vw[0][k]) * morph) >> 15);
                if (p->motion && p->motion_div) c += (int32_t)p->motion * (sine15(beat_phase << (7U - p->motion_div)) >> 7);
                v->formant_f[k] = coef_q8(c + (12 << 8)); /* half rate */
            }
        }
        uint32_t base = detune(v->base, v->drift + v->offset + vibrato);
        if (p->unison >= 3) {
            v->inc[0] = detune(base, -spread); v->inc[1] = base; v->inc[2] = detune(base, spread);
        } else if (p->unison == 2) {
            v->inc[0] = detune(base, -spread / 2); v->inc[1] = detune(base, spread / 2);
        } else v->inc[0] = base;
        v->inc[3] = p->sine_octave < 0 ? base >> 1 : p->sine_octave > 0 ? base << 1 : base;
    } else {
        v->inc[3] = p->sine_octave > 0 ? v->base << 1 : v->base;
        if (p->cutoff) { envelope_tick(&v->filter, p->fenv); filter_tick(v, p); } /* string tone stage */
    }
    if (p->ks == 3) {
        const struct modal_voice *m = &modal_voices[p->modal];
        for (unsigned k = 0; k < 4; ++k)
            v->modal_level[k] -= (int32_t)(((int64_t)v->modal_level[k] * lush_env[m->decay[k] >> 1]) >> 30);
        if (v->modal_level[0] < (ONE30 >> 13) && v->amp.stage != STAGE_RELEASE) v->amp.stage = STAGE_RELEASE;
    }
    if (v->kill) { /* stolen: ~4 ms fade (the per-sample ramp keeps it click-free) */
        v->amp.stage = STAGE_RELEASE;
        v->amp.level -= (int32_t)(((int64_t)v->amp.level * lush_env[4]) >> 30);
    }
    /* Retire at -60 dB: below that the reverb and delay carry the tail. */
    if (v->amp.stage == STAGE_RELEASE && v->amp.level < (ONE30 >> 10)) v->active = 0;
}

static LUSH_CODE __attribute__((minsize)) struct voice *allocate(unsigned want_ks)
{
    struct voice *best = 0;
    unsigned best_score = 0;
    unsigned limit = want_ks ? KS_VOICES : VOICES;
    /* Analog voices fill from the top so string voices keep their buffers. */
    for (unsigned n = 0; n < limit; ++n) {
        struct voice *v = &voices[want_ks ? n : VOICES - 1U - n];
        if (!v->active) return v;
        /* Prefer stealing released, quiet, old voices. */
        unsigned score = (uint16_t)(voice_clock - v->age) + (v->amp.stage == STAGE_RELEASE ? 40000U : 0U) + (v->kill ? 80000U : 0U)
                       + (unsigned)((ONE30 - v->amp.level) >> 20);
        if (!best || score > best_score) { best = v; best_score = score; }
    }
    return best;
}
/* Per note, not per sample: built for size. */
static LUSH_CODE __attribute__((minsize)) void start_voice(unsigned track, unsigned note, unsigned velocity, unsigned patch_id)
{
    const struct patch *p = &voice_patch[track];
    unsigned zone = 0, target = note;
    if (p->ks == 2) {
        /* Nearest sampled zone to the (transposed) note. */
        int t = (int)note + 12 * p->sine_octave, best = 1000;
        target = t < 0 ? 0U : t > 127 ? 127U : (unsigned)t;
        for (unsigned z = p->slot; z < p->slot + p->slots && z < BANK_SLOTS; ++z) {
            if (!bank[z].data) continue;
            int d = (int)target - bank[z].root; d = d < 0 ? -d : d;
            if (d < best) { best = d; zone = z; }
        }
        if (best == 1000) return; /* no samples loaded */
    }
    /* Per-layer polyphony keeps the CPU bounded: basses are mono, synth layers
     * keep two notes, strings three, samples three (reverse swells two). Over the limit, the layer's
     * oldest note fades out in ~4 ms. */
    unsigned cap = patch_id == LUSH_WARM_BASS || patch_id == LUSH_SUB || patch_id == LUSH_DETUNE_BASS ? 1U
                 : p->ks == 2 ? (p->reverse ? 2U : 3U) : p->ks == 1 ? 3U : 2U;
    for (;;) {
        struct voice *oldest = 0; unsigned count = 0;
        for (unsigned i = 0; i < VOICES; ++i) {
            struct voice *o = &voices[i];
            if (!o->active || o->kill || o->track != track) continue;
            ++count;
            if (!oldest || (uint16_t)(voice_clock - o->age) > (uint16_t)(voice_clock - oldest->age)) oldest = o;
        }
        if (count < cap || !oldest) break;
        oldest->kill = 1; oldest->filter.stage = STAGE_RELEASE;
    }
    {
        unsigned n; struct voice *o = oldest_synth(&n);
        if (o && n >= synth_budget) { o->kill = 1; o->filter.stage = STAGE_RELEASE; }
    }
    struct voice *v = allocate(p->ks == 1 || p->reverse); /* reverse reuses string memory as its chunk cache */
    if (!v) return;
    unsigned index = (unsigned)(v - voices);
    uint32_t seed = v->random ^ (note * 2654435761U) ^ voice_clock;
    v->active = 1; v->kill = 0; v->track = (uint8_t)track; v->note = (uint8_t)note; v->patch = (uint8_t)patch_id;
    v->age = ++voice_clock; v->random = seed | 1U;
    int32_t vel = (int32_t)velocity * 258;                                  /* Q15 */
    v->velocity = 32768 - (((32768 - vel) * p->velocity) >> 7);
    v->base = note_inc(note);
    v->amp.level = 0; v->amp.stage = STAGE_ATTACK; v->amp_now = 0;
    v->filter.level = 0; v->filter.stage = STAGE_ATTACK;
    sram_clear(&v->lp1, 12); /* lp1..bp2, rl1..rb2, last_l/r, drift, drift_target: contiguous */
    v->drift = v->drift_target = 0; v->drift_timer = 0;
    /* Each note lands slightly differently, like an analog poly's voice cards. */
    v->offset = (int32_t)((rng(&v->random) >> 16) % (p->drift * 256U + 1U)) - (int32_t)(p->drift * 128U);
    for (unsigned i = 0; i < 4; ++i) v->phase[i] = rng(&v->random); /* free-running, like analog */
    unsigned slot = layer[track].pan_next++;
    int32_t offset = (slot & 1U) ? (int32_t)p->spread : -(int32_t)p->spread;
    if ((slot & 3U) >= 2) offset /= 2;
    v->pan = (uint8_t)(64 + (offset >> 1));
    v->vibrato_phase = 0; v->vibrato_depth = 0;
    if (p->ks == 3) {
        const struct modal_voice *m = &modal_voices[p->modal];
        for (unsigned k = 0; k < 4; ++k) {
            v->phase[k] = 0; /* struck: every partial starts together */
            v->inc[k] = (uint32_t)(((uint64_t)v->base * m->ratio[k]) >> 8);
            v->modal_level[k] = (int32_t)m->level[k] << 23;
        }
    }
    if (p->ks == 2) {
        v->zone = (uint8_t)zone;
        /* 32 kHz samples at 44.1 kHz, Q16: 65536 * 32000 / 44100 = 47554.8. A 64-by-32
         * division uses the CPU's divider, never the Flash helper for 64-by-64. */
        v->sample_step = (uint32_t)(((uint64_t)note_inc(target) * 47555U) / (uint32_t)note_inc(bank[zone].root));
        v->sample_pos = (rng(&v->random) >> 29);   /* 0..7 frames: no two hits identical */
        v->sample_frac = 0; v->sample_next = 0xffffffffU;
        sram_clear(v->sample_state, 8);
        v->velocity -= (int32_t)((rng(&v->random) >> 28) * 160U); /* up to ~-0.7 dB */
        v->amp.level = ONE30 - (ONE30 >> 6); v->amp.stage = STAGE_DECAY; /* keep the recorded attack */
        if (p->reverse) {
            /* Start far enough into the sample that, played backwards, it reaches
             * the attack exactly `reverse` beats from now. */
            uint32_t span = beat_inc ? (uint32_t)((((uint64_t)1 << 28) * p->reverse) / (uint32_t)beat_inc) : 35280U;
            uint64_t frames = ((uint64_t)span * v->sample_step) >> 16;
            uint32_t last = bank[zone].frames - 2U;
            v->sample_pos = frames < last ? (uint32_t)frames : last;
            v->reverse_chunk = -1;
            v->amp.level = 0; v->amp.stage = STAGE_ATTACK;
        }
    }
    if (p->ks == 1) {
        /* Plucked string: delay of one period, minus the loop filter's half sample. */
        uint32_t period = (uint32_t)(((uint64_t)1 << 48) / (uint32_t)v->base);
        if (period > ((KS_LEN - 4U) << 16)) period = (KS_LEN - 4U) << 16;
        if (period < (3U << 16)) period = 3U << 16;
        v->ks_coef = 4000 + p->bright * 220;        /* loop low-pass: brightness */
        /* The one-pole loop filter delays by (1-a)/a samples; keep strings in tune. */
        uint32_t lag = ((uint32_t)(32768 - v->ks_coef) << 16) / (uint32_t)v->ks_coef; /* < 2^31: 32-bit */
        v->ks_delay = period > lag + (2U << 16) ? period - lag : 2U << 16;
        v->ks_write = 0;
        v->ks_gain = 32000 + p->decay * 6;          /* loop gain: sustain */
        v->ks_lp = 0;
        int16_t *line = ks_memory[index];
        unsigned length = (period >> 16) + 2U;
        int32_t tone = 3000 + p->bright * 230 + ((v->velocity * p->bright) >> 8), state = 0;
        if (tone > 32000) tone = 32000;
        sram_clear(line, KS_LEN / 2);
        int32_t sum = 0;
        for (unsigned i = 0; i < length; ++i) {
            state += ((noise15(&v->random) * 2 - state) * tone) >> 15;  /* hammer / pick tone */
            line[i] = (int16_t)clamp15((state * v->velocity) >> 15);
            sum += line[i];
        }
        /* A string can't hold DC: remove the excitation's mean or it rings forever below 20 Hz. */
        int32_t mean = sum / (int32_t)length;
        for (unsigned i = 0; i < length; ++i) line[i] = (int16_t)clamp15(line[i] - mean);
        v->ks_write = (uint16_t)length;
    }
    voice_tick(v, p);
}
__attribute__((minsize)) LUSH_CODE void lush_note(unsigned track, unsigned note, unsigned velocity)
{
    if (track >= TRACKS || note > 127) return;
    int shifted = (int)note + 12 * voice_octave[track];
    note = shifted < 0 ? 0U : shifted > 127 ? 127U : (unsigned)shifted;
    unsigned patch_id = layer[track].patch;
    if (patch_id == LUSH_NONE) return;
    if (velocity) { start_voice(track, note, velocity, patch_id); return; }
    if (voice_patch[track].reverse) return; /* a swell always lands on its beat */
    for (unsigned i = 0; i < VOICES; ++i) {
        struct voice *v = &voices[i];
        if (v->active && v->track == track && v->note == note && v->amp.stage != STAGE_RELEASE) {
            v->amp.stage = STAGE_RELEASE; v->filter.stage = STAGE_RELEASE;
        }
    }
}
__attribute__((minsize)) LUSH_CODE void lush_track_reset(unsigned track)
{
    for (unsigned i = 0; i < VOICES; ++i)
        if (voices[i].active && voices[i].track == track) {
            voices[i].amp.stage = STAGE_RELEASE; voices[i].filter.stage = STAGE_RELEASE;
        }
}

__attribute__((minsize)) LUSH_CODE void lush_transport(uint8_t status)
{
    if (status == 0xfa) beat_phase = 0;
    /* Stop/pause sends no note-offs: release every note so nothing hangs. */
    if (status == 0xfc || status == 0xff)
        for (unsigned i = 0; i < VOICES; ++i) { voices[i].amp.stage = STAGE_RELEASE; voices[i].filter.stage = STAGE_RELEASE; }
}

/* Saturation then a 4-pole state-variable low-pass (two Chamberlin stages). */
static inline LUSH_HOT int32_t warm_filter(int32_t osc, const struct patch *p, int32_t f, int32_t q,
                                            int32_t *lp1, int32_t *bp1, int32_t *lp2, int32_t *bp2)
{
    int32_t x = clamp15(osc + ((osc * p->drive * LUSH_DRIVE) >> 7));
#if LUSH_DRIVE
    int32_t x2 = (x * x) >> 15;
    x = (3 * x - ((x2 * x) >> 15)) >> 1;
#endif
    *lp1 += (int32_t)(((int64_t)f * *bp1) >> 15);
    int32_t hp = x - *lp1 - (int32_t)(((int64_t)q * *bp1) >> 15);
    *bp1 += (int32_t)(((int64_t)f * hp) >> 15);
    *lp2 += (int32_t)(((int64_t)f * *bp2) >> 15);
    hp = *lp1 - *lp2 - (int32_t)(((int64_t)44000 * *bp2) >> 15);
    *bp2 += (int32_t)(((int64_t)f * hp) >> 15);
    if (*bp1 > (1 << 20) || *bp1 < -(1 << 20)) *bp1 >>= 1;
    if (*bp2 > (1 << 20) || *bp2 < -(1 << 20)) *bp2 >>= 1;
    return *lp2;
}

/* Decoded sample at a source frame, caching two SEA groups so interpolation
 * across a group boundary never re-decodes. Returns 0 at the end or on error
 * and marks the voice finished. */
static inline LUSH_HOT int32_t sample_at(struct voice *v, uint32_t position)
{
    const uint8_t *data = bank[v->zone].data;
    if (!data || position >= bank[v->zone].frames) { v->sample_next = 0xfffffffeU; return 0; }
    uint32_t group = position / FM1_SEA_GROUP, next = v->sample_next;
    if (next == 0xfffffffeU) return 0;
    if (next == 0xffffffffU || group >= next || next - group > 2U) {
        uint32_t chunk = group / 16U, last = group % 16U;
        uint32_t first = next != 0xffffffffU && next <= group && next / 16U == chunk ? next % 16U : 0U;
        uint32_t offset = chunk * SEA_CHUNK_BYTES;
        if (offset + SEA_CHUNK_BYTES > bank[v->zone].bytes ||
            fm1_sea_decode_groups(data + offset, SEA_CHUNK_BYTES, 5, lush_dqt, first, last,
                                  v->sample_state, v->sample_pcm + (group & 1U) * FM1_SEA_GROUP)) {
            v->sample_next = 0xfffffffeU;
            return 0;
        }
        v->sample_next = group + 1U;
    }
    return v->sample_pcm[(group & 1U) * FM1_SEA_GROUP + position % FM1_SEA_GROUP];
}

/* ---- buses ---- */
enum { N = FM1_BLOCK_FRAMES };
static int32_t bus_dry[2 * N], bus_chorus[N], bus_reverb[N], bus_delay[N];

static LUSH_CODE void render_voice(struct voice *v)
{
    const struct patch *p = &voice_patch[v->track];
    int32_t gain = (int32_t)(((int64_t)layer[v->track].gain * v->velocity) >> 15);
    gain = (gain * p->gain) >> 7;
    int32_t send_r = layer[v->track].reverb, send_d = layer[v->track].delay;
    int32_t pan = v->pan + p->pos;
    if (p->pan_move) /* slow 2-bar drift across the field, each voice on its own phase */
        pan += (sine15((beat_phase << 1) + (uint32_t)v->age * 0x28000000U) * p->pan_move) >> 16;
    pan = pan < 0 ? 0 : pan > 128 ? 128 : pan;
    int32_t left = (int32_t)(128 - pan) * 256, right = (int32_t)pan * 256;
    /* PAN on a chorused voice pulls it out of the (centred) chorus toward its
     * side; on a stereo voice it works as balance. */
    int32_t off = p->pos < 0 ? -p->pos : p->pos;
    int32_t chorus_keep = (64 - off) * 512;
    int32_t bal_l = p->pos > 0 ? (64 - p->pos) * 512 : 32768, bal_r = p->pos < 0 ? (64 + p->pos) * 512 : 32768;
    unsigned index = (unsigned)(v - voices);

    int32_t saw_level = p->stereo ? (p->unison >= 3 ? p->saw * 172 : p->saw * 258)
                                  : (p->saw * 258) / (p->unison ? p->unison : 1);
    for (unsigned at = 0; at < N; at += TICK) {
        int32_t from = v->amp_now;
        voice_tick(v, p);
        int32_t to = v->amp.level >> 15, step = (to - from) / TICK, amp = from;
        v->amp_now = to;
        for (unsigned k = 0; k < TICK; ++k) {
            int32_t s, s_right = 0;
            if (p->ks == 3) {
                int32_t sum = 0;
                for (unsigned q = 0; q < 4; ++q) {
                    sum += (int32_t)(((int64_t)sine15(v->phase[q]) * (v->modal_level[q] >> 15)) >> 15);
                    v->phase[q] += v->inc[q];
                }
                s = sum;
            } else if (p->ks == 2 && p->reverse) {
                /* Backwards: decode whole chunks into this voice's string memory. */
                int32_t ab[2];
                for (unsigned e = 0; e < 2; ++e) {
                    uint32_t pos = v->sample_pos - e, chunk = pos / FM1_SEA_FRAMES;
                    const uint8_t *data = bank[v->zone].data;
                    if (!data || v->sample_pos < 2U) { ab[e] = 0; v->sample_next = 0xfffffffeU; continue; }
                    if ((int32_t)chunk != v->reverse_chunk) {
                        int32_t state[8], failed = 0;  /* whole chunk, group by group */
                        sram_clear(state, 8);
                        for (unsigned g = 0; g < 16U && !failed; ++g)
                            failed = fm1_sea_decode_groups(data + chunk * SEA_CHUNK_BYTES, SEA_CHUNK_BYTES, 5, lush_dqt,
                                                           g, g, state, ks_memory[index] + g * FM1_SEA_GROUP);
                        if (failed) { ab[e] = 0; v->sample_next = 0xfffffffeU; continue; }
                        v->reverse_chunk = (int32_t)chunk;
                    }
                    ab[e] = ks_memory[index][pos % FM1_SEA_FRAMES];
                }
                int32_t x = ab[0] + (int32_t)(((int64_t)(ab[1] - ab[0]) * (int32_t)v->sample_frac) >> 16);
                v->sample_frac += v->sample_step;
                uint32_t back = v->sample_frac >> 16; v->sample_frac &= 0xffffU;
                v->sample_pos = v->sample_pos > back ? v->sample_pos - back : 0U;
                v->lp1 += (int32_t)(((int64_t)(x - v->lp1) * (v->f - (v->f >> 2))) >> 15); /* two-pole tone */
                v->lp2 += (int32_t)(((int64_t)(v->lp1 - v->lp2) * (v->f - (v->f >> 2))) >> 15);
                s = v->lp2 + (v->lp2 >> 1);
                if (v->sample_next == 0xfffffffeU) { v->amp.stage = STAGE_RELEASE; v->amp.level >>= 2; }
            } else if (p->ks == 2) {
                int32_t a = sample_at(v, v->sample_pos), b = sample_at(v, v->sample_pos + 1U);
                int32_t x = a + (int32_t)(((int64_t)(b - a) * (int32_t)v->sample_frac) >> 16);
                v->sample_frac += v->sample_step;
                v->sample_pos += v->sample_frac >> 16; v->sample_frac &= 0xffffU;
                /* Samples run at full rate, so a two-pole tone stage stands in for
                 * the 4-pole ladder: the velocity-tracked darkening stays, at a
                 * fraction of the cost (sample-heavy scenes were the ones overloading). */
                v->lp1 += (int32_t)(((int64_t)(x - v->lp1) * (v->f - (v->f >> 2))) >> 15);
                v->lp2 += (int32_t)(((int64_t)(v->lp1 - v->lp2) * (v->f - (v->f >> 2))) >> 15);
                s = v->lp2 + (v->lp2 >> 1); /* the ladder's saturator had 1.5x small-signal gain */
                if (v->sample_next == 0xfffffffeU) { v->amp.stage = STAGE_RELEASE; v->amp.level >>= 1; }
            } else if (p->ks == 1) {
                int16_t *line = ks_memory[index];
                uint32_t read = (((uint32_t)v->ks_write << 16) - v->ks_delay) & ((KS_LEN << 16) - 1U);
                unsigned i0 = read >> 16, i1 = (i0 + 1U) & (KS_LEN - 1U);
                int32_t frac = (int32_t)((read >> 1) & 32767U);
                int32_t y = line[i0] + (((line[i1] - line[i0]) * frac) >> 15);
                v->ks_lp += ((y - v->ks_lp) * v->ks_coef) >> 15;
                line[v->ks_write] = (int16_t)clamp15((v->ks_lp * v->ks_gain) >> 15);
                v->ks_write = (uint16_t)((v->ks_write + 1U) & (KS_LEN - 1U));
                s = y * 2; /* strings excite quieter than oscillators */
                if (p->cutoff) /* amp-style tone stage (muted guitar) */
                    s = warm_filter(s, p, v->f, v->q, &v->lp1, &v->bp1, &v->lp2, &v->bp2);
                if (p->sine) {
                    s += (sine15(v->phase[3]) * p->sine) >> 8;
                    v->phase[3] += v->inc[3];
                }
            } else if (k & 1U) {
                /* Synth voices run at 22.05 kHz: odd samples repeat the last
                 * computed one; even ones (below) interpolate across the gap. */
                s = v->last_l; s_right = v->last_r;
            } else {
                int32_t a = 0, b = 0, c = 0, common = 0;
                uint32_t i0 = v->inc[0] << 1, i1 = v->inc[1] << 1, i2 = v->inc[2] << 1;
                if (p->saw) {
                    a = saw15(v->phase[0], i0);
                    if (p->unison >= 2) b = saw15(v->phase[1], i1);
                    if (p->unison >= 3) c = saw15(v->phase[2], i2);
                }
                if (p->pulse) {
                    uint32_t width = 0x18000000U + (uint32_t)p->pw * 0x00d00000U; /* 127 = square */
                    int32_t pulse = saw15(v->phase[0], i0) - saw15(v->phase[0] + width, i0);
                    if (p->unison >= 2) /* detuned second pulse for the classic chorused arp */
                        pulse += saw15(v->phase[1], i1) - saw15(v->phase[1] + width, i1);
                    common += (pulse * p->pulse) >> (p->unison >= 2 ? 9 : 8);
                }
                if (p->sine) common += (sine15(v->phase[3]) * p->sine) >> 7;
                if (p->noise) common += (noise15(&v->random) * p->noise) >> 7;
                for (unsigned u = 0; u < 4; ++u) v->phase[u] += v->inc[u] << 1;
                if (p->stereo) {
                    /* Unison split across the field: outer saws hard left/right, centre shared. */
                    int32_t l = p->unison >= 3 ? a + (b >> 1) : a, r = p->unison >= 3 ? c + (b >> 1) : b;
                    l = ((l * saw_level) >> 15) + common;
                    r = ((r * saw_level) >> 15) + common;
                    s = warm_filter(l, p, v->f, v->q, &v->lp1, &v->bp1, &v->lp2, &v->bp2);
                    s_right = warm_filter(r, p, v->f, v->q, &v->rl1, &v->rb1, &v->rl2, &v->rb2);
                } else if (p->formant) {
                    /* Three vowel formants in parallel (band-passes, q ~ 0.18). */
                    int32_t x = (((a + b + c) * saw_level) >> 15) + common, y = 0;
                    static FM1_AUDIO_DATA const int32_t mix[3] = {11000, 7700, 3300};
                    int32_t *lp[3] = {&v->lp1, &v->lp2, &v->rl1}, *bp[3] = {&v->bp1, &v->bp2, &v->rb1};
                    for (unsigned k = 0; k < 3; ++k) {
                        *lp[k] += (int32_t)(((int64_t)v->formant_f[k] * *bp[k]) >> 15);
                        int32_t h = x - *lp[k] - ((5900 * *bp[k]) >> 15);
                        *bp[k] += (int32_t)(((int64_t)v->formant_f[k] * h) >> 15);
                        if (*bp[k] > (1 << 20) || *bp[k] < -(1 << 20)) *bp[k] >>= 1;
                        y += (int32_t)(((int64_t)*bp[k] * mix[k]) >> 15);
                    }
                    s = y;
                } else {
                    int32_t osc = (((a + b + c) * saw_level) >> 15) + common;
                    s = warm_filter(osc, p, v->f, v->q, &v->lp1, &v->bp1, &v->lp2, &v->bp2);
                }
                int32_t now_l = s, now_r = s_right;
                s = (v->last_l + now_l) >> 1; s_right = (v->last_r + now_r) >> 1;
                v->last_l = now_l; v->last_r = now_r;
            }
            amp += step;
            int32_t out = (int32_t)(((int64_t)s * amp) >> 15);
            out = (int32_t)(((int64_t)out * gain) >> 15);
            unsigned j = at + k;
            if (p->stereo) {
                int32_t outr = (int32_t)(((int64_t)s_right * amp) >> 15);
                outr = (int32_t)(((int64_t)outr * gain) >> 15);
                bus_dry[2 * j] += (out * bal_l) >> 15;
                bus_dry[2 * j + 1] += (outr * bal_r) >> 15;
                out = (out + outr) >> 1; /* sends are mono */
            } else if (p->chorus) {
                int32_t kept = (out * chorus_keep) >> 15, moved = out - kept;
                bus_chorus[j] += kept;
                if (moved) { bus_dry[2 * j] += (moved * left) >> 15; bus_dry[2 * j + 1] += (moved * right) >> 15; }
            } else {
                bus_dry[2 * j] += (out * left) >> 15;
                bus_dry[2 * j + 1] += (out * right) >> 15;
            }
            bus_reverb[j] += (out * send_r) >> 15;
            bus_delay[j] += (out * send_d) >> 15;
        }
    }
}

/* ---- chorus: Juno-style, mono in, stereo out ---- */
enum { CHORUS_LEN = 512 }; /* the sweep reads at most ~340 samples back */
static int16_t chorus_line[CHORUS_LEN] VESPER_BIG;
static unsigned chorus_write;
static uint32_t chorus_lfo;
static inline LUSH_HOT int32_t chorus_tap(uint32_t delay_q16)
{
    uint32_t read = (((uint32_t)chorus_write << 16) - delay_q16) & ((CHORUS_LEN << 16) - 1U);
    unsigned i0 = read >> 16, i1 = (i0 + 1U) & (CHORUS_LEN - 1U);
    int32_t frac = (int32_t)((read >> 1) & 32767U);
    return chorus_line[i0] + (((chorus_line[i1] - chorus_line[i0]) * frac) >> 15);
}
/* CLOUD before SPACE: the delay and reverb sends pass through their own
 * half-rate chorus lines, swept by the same LFO, so echoes and tails shimmer
 * with the chorus. 256 samples at 22 kHz covers the ~170-sample sweep. */
static int16_t send_line[2][256];
static int32_t send_shelf[2]; /* low-mid cut on the delay and reverb inputs */
static uint8_t send_write;
static inline LUSH_HOT int32_t send_chorus(unsigned which, int32_t x, uint32_t delay_q16, int32_t wet)
{
    int16_t *line = send_line[which];
    line[send_write] = (int16_t)clamp15(x);
    uint32_t read = (((uint32_t)send_write << 16) - delay_q16) & ((256U << 16) - 1U);
    unsigned i0 = read >> 16, i1 = (i0 + 1U) & 255U;
    int32_t frac = (int32_t)((read >> 1) & 32767U);
    int32_t tap = line[i0] + (((line[i1] - line[i0]) * frac) >> 15);
    return x + (((tap - x) * wet) >> 15);
}

/* ---- delay: dark, tempo-synced, half rate ---- */
enum { DELAY_LEN = 24000 }; /* 1.09 s at 22.05 kHz: a quarter note down to 55 BPM */
static int16_t delay_line[DELAY_LEN] VESPER_BIG;
static unsigned delay_write;
static int32_t delay_lp, delay_hp, delay_prev[2], delay_out[2];
static uint32_t tape_wow, tape_flutter;
static inline LUSH_HOT int32_t delay_tap(uint32_t delay_q16)
{
    uint32_t pos = ((uint32_t)delay_write << 16) - delay_q16;
    if ((int32_t)pos < 0) pos += (uint32_t)DELAY_LEN << 16;
    unsigned i0 = pos >> 16, i1 = i0 + 1U == DELAY_LEN ? 0U : i0 + 1U;
    int32_t frac = (int32_t)((pos >> 1) & 32767U);
    return delay_line[i0] + (((delay_line[i1] - delay_line[i0]) * frac) >> 15);
}

/* ---- plate reverb (Dattorro), half rate ---- */
struct line { int16_t *buf; uint16_t length, write; };
#define LINE(name, n) static int16_t name##_mem[n] VESPER_BIG; static struct line name = {name##_mem, n, 0}
LINE(in1, 105); LINE(in2, 79); LINE(in3, 281); LINE(in4, 205);
LINE(l_ap1, 512); LINE(l_d1, 3299); LINE(l_ap2, 1334); LINE(l_d2, 2756);
LINE(r_ap1, 688); LINE(r_d1, 3124); LINE(r_ap2, 1968); LINE(r_d2, 2344);
static int32_t rev_bandwidth, rev_lp_l, rev_lp_r, rev_prev[2], rev_out[2], rev_low;
/* Shimmer: an octave-up granular pitch shifter in the reverb's feedback, so
 * the tail keeps blooming upward. Two read heads sweep a 46 ms window at
 * double speed, crossfaded with triangles (half rate, 22.05 kHz). */
/* Shimmer, in the spirit of the big studio shimmers: the reverb tail is
 * band-limited (so nothing aliases or fizzes), shifted up an octave with long
 * 93 ms grains at 11 kHz (smooth, no warble), and fed back into the tank, so
 * octaves bloom slowly out of the tail and each pass gets softer, not harsher. */
enum { SHIMMER_LEN = 2048, SHIMMER_WINDOW = 1024 };
static int16_t shimmer_line[SHIMMER_LEN] VESPER_BIG;
static unsigned shimmer_write, shimmer_phase, shimmer_odd;
static int32_t shimmer_lp1, shimmer_lp2, shimmer_hp, shimmer_held, shimmer_prev, shimmer_y, shimmer_soft, shimmer_soft2;
static inline LUSH_HOT int32_t shimmer_sample(int32_t input)
{
    /* 22 kHz in: ~2.5 kHz 12 dB low pass, ~300 Hz high pass, then every other sample. */
    shimmer_lp1 += ((input - shimmer_lp1) * 16700) >> 15;
    shimmer_lp2 += ((shimmer_lp1 - shimmer_lp2) * 16700) >> 15;
    shimmer_hp += ((shimmer_lp2 - shimmer_hp) * 2700) >> 15;
    int32_t x = shimmer_lp2 - shimmer_hp;
    int32_t up;
    if ((shimmer_odd ^= 1U)) { shimmer_held = x; up = shimmer_y; } /* linear interpolation, one sample late */
    else {
        shimmer_line[shimmer_write] = (int16_t)clamp15((shimmer_held + x) >> 1);
        unsigned p1 = shimmer_phase, p2 = (shimmer_phase + SHIMMER_WINDOW / 2U) % SHIMMER_WINDOW;
    unsigned d1 = SHIMMER_WINDOW - p1 + 2U, d2 = SHIMMER_WINDOW - p2 + 2U;
    int32_t a = shimmer_line[(shimmer_write + SHIMMER_LEN - d1) & (SHIMMER_LEN - 1U)];
    int32_t b = shimmer_line[(shimmer_write + SHIMMER_LEN - d2) & (SHIMMER_LEN - 1U)];
    int32_t sn = sine15((uint32_t)p1 << 21);                      /* sin(pi * p1 / W) */
    int32_t g1 = (sn * sn) >> 15;                                  /* sin^2 grains always sum to one */
    int32_t y = (a * g1 + b * (32768 - g1)) >> 15;
    shimmer_write = (shimmer_write + 1U) & (SHIMMER_LEN - 1U);
    if (++shimmer_phase == SHIMMER_WINDOW) shimmer_phase = 0;
    shimmer_prev = shimmer_y; shimmer_y = y;
    up = (shimmer_prev + y) >> 1;
    }
    /* back at 22 kHz: ~4 kHz 12 dB low pass removes the images of the 11 kHz shifter */
    shimmer_soft += ((up - shimmer_soft) * 22300) >> 15;
    shimmer_soft2 += ((shimmer_soft - shimmer_soft2) * 22300) >> 15;
    return shimmer_soft2;
}

/* ---- CLOUD GRAINS: a granular cloud in the spirit of hardware loopers.
 * The sound is recorded (u-law, 11 kHz, 0.28 s) and up to four overlapping
 * grains of 60-180 ms are played back from random places with smooth windows
 * and random pan. Turning GRAINS up makes them denser and brings in octave-up
 * and reversed grains. The cloud feeds SPACE, and a little reverb feeds back
 * into the cloud, so it keeps blooming. Integer only, no divides, all in SRAM. */
enum { GRAIN_BUF = 3072, GRAIN_HALF = 2 * GRAIN_BUF, GRAINS = 4 }; /* 0.28 s */
static uint8_t grain_buf[GRAIN_BUF] VESPER_BIG;
static unsigned grain_w;                     /* write head in half-samples (22 kHz ticks) */
static int32_t grain_in, grain_lp, grain_hp, grain_soft[4], grain_out[2], grain_prev[2];
static uint32_t grain_seed = 0x2545f491U, grain_tick = ~0U;
static struct grain { int32_t pos, step, left; uint32_t phase, inc; int32_t gl, gr; } grains[GRAINS];
static inline LUSH_HOT uint8_t mu_encode(int32_t x)
{
    unsigned sign = x < 0 ? 0x80U : 0U, m = (unsigned)(x < 0 ? -x : x);
    if (m > 32635U) m = 32635U;
    m += 132U;
    unsigned e = (m >= 256U) + (m >= 512U) + (m >= 1024U) + (m >= 2048U) + (m >= 4096U) + (m >= 8192U) + (m >= 16384U);
    return (uint8_t)(sign | (e << 4) | ((m >> (e + 3U)) & 15U));
}
static inline LUSH_HOT int32_t mu_decode(unsigned b)
{
    unsigned e = (b >> 4) & 7U;
    int32_t t = ((int32_t)(((b & 15U) << 3) + 132U) << e) - 132;
    return b & 0x80U ? -t : t;
}
static inline LUSH_HOT uint32_t grain_random(void)
{
    grain_seed = grain_seed * 1664525U + 1013904223U;
    return grain_seed >> 8;
}
static inline LUSH_HOT void grain_spawn(struct grain *g, int32_t amount, int32_t span)
{
    /* span: the grid step in samples. Grains last one and a half steps, a
     * quarter of them half a step (a stutter). Octave-up and reversed grains come in as GRAINS rises. */
    uint32_t r = grain_random();
    unsigned kind = (r >> 3) & 127U;
    g->step = amount > 40 && kind < (unsigned)amount / 3U ? 2               /* octave up */
            : amount > 64 && kind > 127U - (unsigned)amount / 6U ? -1       /* reversed */
            : 1;
    int32_t len = (r & 3U) ? span + (span >> 1) : span >> 1;          /* overlap the next grid step by half */
    int32_t cap = g->step == -1 ? 2900 : 3900;                           /* must fit the 0.28 s memory */
    if (len > cap) len = cap;
    if (len < 400) len = 400;
    int32_t lo = g->step == 2 ? len + 64 : 64;
    int32_t hi = g->step == -1 ? GRAIN_HALF - 2 * len - 64 : GRAIN_HALF - 64;
    int32_t back = lo + (int32_t)((((r >> 10) & 0xffffU) * (uint32_t)(hi - lo)) >> 16);
    int32_t pos = (int32_t)grain_w - back;
    g->pos = pos < 0 ? pos + GRAIN_HALF : pos;
    g->left = len; g->phase = 0; g->inc = 0x80000000U / (uint32_t)len;   /* half a sine across the grain */
    int32_t pan = 9000 + (int32_t)(grain_random() & 16383U) + (int32_t)(grain_random() & 4095U); /* 9000..29500 */
    g->gl = 46000 - (pan * 46000 >> 15) + 4000; g->gr = (pan * 46000 >> 15) + 4000;               /* random, never hard */
}
static inline LUSH_HOT void cloud_sample(int32_t input, int32_t amount, int32_t out[2])
{
    grain_hp += ((input - grain_hp) * 2600) >> 15;                       /* ~280 Hz: no mud, no DC loop */
    grain_lp += ((input - grain_hp - grain_lp) * 23000) >> 15;           /* ~4.3 kHz before 11 kHz storage */
    if (grain_w & 1U) grain_buf[grain_w >> 1] = mu_encode((grain_in + grain_lp) >> 1);
    else grain_in = grain_lp;
    /* On the tempo grid: eighths when GRAINS is low, sixteenths in the middle,
     * 32nds (with an extra grain now and then) at the top. */
    uint32_t tick = beat_phase >> 25;                                    /* 32nd-note counter */
    if (tick != grain_tick) {
        grain_tick = tick;
        unsigned every = amount < 43 ? 4U : amount < 86 ? 2U : 1U; /* in 32nds */
        if (!(tick & (every - 1U))) {
            unsigned spawn = 1U + (amount > 100 && (grain_random() & 1U));
            for (unsigned g = 0; g < GRAINS && spawn; ++g)
                if (!grains[g].left) { grain_spawn(&grains[g], amount, grain_32nd * (int32_t)every); --spawn; }
        }
    }
    int32_t l = 0, r = 0;
    for (unsigned g = 0; g < GRAINS; ++g) {
        struct grain *q = &grains[g];
        if (!q->left) continue;
        int32_t p = q->pos;
        unsigned i = (unsigned)p >> 1;
        int32_t v = mu_decode(grain_buf[i]);
        if (p & 1) v = (v + mu_decode(grain_buf[i + 1U == GRAIN_BUF ? 0U : i + 1U])) >> 1;
        int32_t w = sine15(q->phase);
        v = (v * ((w * w) >> 15)) >> 15;                                  /* sin^2 window */
        l += (v * q->gl) >> 15; r += (v * q->gr) >> 15;
        q->phase += q->inc;
        p += q->step;
        q->pos = p >= GRAIN_HALF ? p - GRAIN_HALF : p < 0 ? p + GRAIN_HALF : p;
        --q->left;
    }
    if (++grain_w == GRAIN_HALF) grain_w = 0;
    /* 12 dB smoothing removes the 11 kHz storage images */
    grain_soft[0] += ((l - grain_soft[0]) * 22300) >> 15; grain_soft[1] += ((grain_soft[0] - grain_soft[1]) * 22300) >> 15;
    grain_soft[2] += ((r - grain_soft[2]) * 22300) >> 15; grain_soft[3] += ((grain_soft[2] - grain_soft[3]) * 22300) >> 15;
    out[0] = grain_soft[1]; out[1] = grain_soft[3];
}
static uint32_t rev_lfo;
static inline LUSH_HOT int32_t tap(const struct line *l, unsigned n)
{
    int i = (int)l->write - (int)n;
    if (i < 0) i += l->length;
    return l->buf[i];
}
static inline LUSH_HOT void push(struct line *l, int32_t v)
{
    l->buf[l->write] = (int16_t)clamp15(v);
    if (++l->write == l->length) l->write = 0;
}
static inline LUSH_HOT int32_t allpass(struct line *l, unsigned n, int32_t x, int32_t g)
{
    int32_t d = tap(l, n);
    int32_t v = x - ((d * g) >> 15);
    push(l, v);
    return d + ((v * g) >> 15);
}
static inline LUSH_HOT int32_t allpass_mod(struct line *l, uint32_t n_q16, int32_t x, int32_t g)
{
    unsigned n = n_q16 >> 16;
    int32_t frac = (int32_t)((n_q16 >> 1) & 32767U);
    int32_t a = tap(l, n), b = tap(l, n + 1U);
    int32_t d = a + (((b - a) * frac) >> 15);
    int32_t v = x - ((d * g) >> 15);
    push(l, v);
    return d + ((v * g) >> 15);
}
static inline LUSH_HOT void reverb_sample(int32_t input, int32_t out[2])
{
    int32_t decay = fx_decay, damp = fx_damp;
    /* SHIMMER goes deep into the tank: a quarter through the input diffusers,
     * the rest straight into the modulated, damped loop, so every pass of the
     * octave is smeared and darkened by the reverb itself. */
    int32_t shim = 0, shim_direct = 0;
    if (fx_shimmer) {
        shim = (int32_t)(((int64_t)shimmer_sample((rev_out[0] + rev_out[1]) >> 1) * fx_shimmer) >> 15);
        shim_direct = (shim * 3) >> 2; /* 1.0: louder shimmer straight to the reverb output too */
        input += shim >> 2;
        shim = (shim * 3) >> 3; /* three eighths into each side of the loop */
    }
    rev_low += ((input - rev_low) * 1100) >> 15;           /* keep lows out of the tail */
    input -= rev_low;
    rev_bandwidth += ((input - rev_bandwidth) * 26000) >> 15;
    int32_t x = rev_bandwidth;
    x = allpass(&in1, in1.length - 1U, x, 24576);
    x = allpass(&in2, in2.length - 1U, x, 24576);
    x = allpass(&in3, in3.length - 1U, x, 20480);
    x = allpass(&in4, in4.length - 1U, x, 20480);
    rev_lfo += 104000U; /* ~0.53 Hz at 22.05 kHz */
    int32_t tri = (int32_t)(rev_lfo >> 16); tri = tri < 32768 ? tri : 65535 - tri; /* 0..32767 */
    uint32_t mod_l = ((uint32_t)(l_ap1.length - 26U) << 16) + (uint32_t)tri * 24U;
    uint32_t mod_r = ((uint32_t)(r_ap1.length - 26U) << 16) + (uint32_t)(32767 - tri) * 24U;
    int32_t left = x + shim + ((tap(&r_d2, r_d2.length - 1U) * decay) >> 15);
    int32_t right = x + shim + ((tap(&l_d2, l_d2.length - 1U) * decay) >> 15);
    left = allpass_mod(&l_ap1, mod_l, left, -22938);
    push(&l_d1, left);
    left = tap(&l_d1, l_d1.length - 1U);
    rev_lp_l += ((left - rev_lp_l) * (32767 - damp)) >> 15;
    left = (rev_lp_l * decay) >> 15;
    left = allpass(&l_ap2, l_ap2.length - 1U, left, 16384);
    push(&l_d2, left);
    right = allpass_mod(&r_ap1, mod_r, right, -22938);
    push(&r_d1, right);
    right = tap(&r_d1, r_d1.length - 1U);
    rev_lp_r += ((right - rev_lp_r) * (32767 - damp)) >> 15;
    right = (rev_lp_r * decay) >> 15;
    right = allpass(&r_ap2, r_ap2.length - 1U, right, 16384);
    push(&r_d2, right);
    out[0] = tap(&r_d1, 197) + tap(&r_d1, 2204) - tap(&r_ap2, 1417) + tap(&r_d2, 1479)
           - tap(&l_d1, 1474) - tap(&l_ap2, 139) - tap(&l_d2, 790) + shim_direct;
    out[1] = tap(&l_d1, 262) + tap(&l_d1, 2687) - tap(&l_ap2, 910) + tap(&l_d2, 1981)
           - tap(&r_d1, 1564) - tap(&r_ap2, 248) - tap(&r_d2, 90) + shim_direct;
}

static int32_t dc_in[2], dc_out[2]; /* dc_out is Q8 above the signal */
static int32_t analog_lp[2];
/* Analog output stage: gentle even+odd saturation, then a soft roll-off near
 * 9 kHz so nothing reads as glassy. Its small DC is removed by dc_block. */
static inline LUSH_HOT int32_t analog(unsigned ch, int32_t x)
{
    x = clamp15(x);
#if LUSH_ANALOG
    int32_t x2 = (x * x) >> 15;
    int32_t y = x + ((x2 * 2949) >> 15) - ((((x2 * x) >> 15) * 3932) >> 15); /* +0.09x^2 -0.12x^3 */
#else
    int32_t y = x;
#endif
    analog_lp[ch] += ((y - analog_lp[ch]) * 23500) >> 15;
    return analog_lp[ch];
}
static inline LUSH_HOT int32_t dc_block(unsigned ch, int32_t x)
{
    int32_t y = ((x - dc_in[ch]) << 8) + (int32_t)(((int64_t)dc_out[ch] * 32675) >> 15);
    dc_in[ch] = x; dc_out[ch] = y;
    return y >> 8;
}
static inline LUSH_HOT int32_t soft_limit(int32_t x)
{
    /* Transparent below ~0.7, rounds off peaks instead of hard clipping. */
    const int32_t knee = 23000;
    int32_t over = (x < 0 ? -x : x) - knee;
    if (over <= 0) return x;
    over = knee + (over * 9830) / (9830 + over);
    return x < 0 ? -over : over;
}

/* ---- VESPER master FX, in chain order DIRT -> TAPE -> CLOUD -> SPACE (CLOUD width last).
 * DIRT (drive, bit crush, sample hold) and TAPE (hi pass, lo pass, saturation,
 * age) run on every bus before the chorus, delay and reverb see it, so echoes
 * and tails repeat the dirt and the filtering. Each part is skipped at its
 * neutral value. */
static inline LUSH_HOT int32_t soft_clip(int32_t x)
{   /* 1.5x - 0.5x^3 inside +-1 (Q15), flat outside */
    x = clamp15(x);
    int32_t x2 = (x * x) >> 15;
    return (3 * x - ((x2 * x) >> 15)) >> 1;
}
enum { PRE_BUSES = 5 }; /* dry L, dry R, chorus, delay send, reverb send */
static int32_t hold_v[PRE_BUSES], filt[PRE_BUSES][4];
static uint8_t pre_take[N];
static int32_t level_env, level_gain = 32767, level_target = 32767; /* LEVEL leveler state */
static uint32_t hold_phase;
/* Knob moves glide over ~10 ms instead of stepping, so turning DIRT and TONE is smooth. */
static int32_t glide_g = 256, glide_dc = 32767, glide_mix = 32767, glide_hp, glide_lp = 32767;
static inline LUSH_HOT int32_t glide(int32_t now, int32_t target)
{
    int32_t d = target - now;
    return d > -8 && d < 8 ? target : now + (d >> 2);
}
static inline LUSH_HOT void pre_fx(void)
{
    glide_g = glide(glide_g, fx_drive_gain); glide_dc = glide(glide_dc, fx_drive_comp);
    glide_mix = glide(glide_mix, fx_dirt_mix); glide_hp = glide(glide_hp, fx_hp); glide_lp = glide(glide_lp, fx_lp);
    const int32_t g = glide_g, dc = glide_dc, crush = fx_crush, rate = fx_rate_inc, mixd = glide_mix;
    const int32_t hp = glide_hp, lp = glide_lp;
    const int drive_on = g != 256, held_on = rate < 65536, dirt_on = drive_on || crush || held_on, filter_on = hp || lp < 32767;
    if (!dirt_on && !filter_on) return;
    if (held_on)
        for (unsigned j = 0; j < N; ++j) { /* RATE: take a new sample at the chosen rate, hold it between */
            hold_phase += (uint32_t)rate;
            pre_take[j] = hold_phase >= 65536U;
            if (pre_take[j]) hold_phase -= 65536U;
        }
    for (unsigned c = 0; c < PRE_BUSES; ++c) {
        int32_t *p = c < 2 ? bus_dry + c : c == 2 ? bus_chorus : c == 3 ? bus_delay : bus_reverb;
        const unsigned stride = c < 2 ? 2U : 1U;
        int32_t held = hold_v[c];
        int32_t h1 = filt[c][0], h2 = filt[c][1], l1 = filt[c][2], l2 = filt[c][3];
        for (unsigned j = 0; j < N; ++j, p += stride) {
            int32_t x = *p;
            if (dirt_on) {
                int32_t d = x;
                if (drive_on) d = (soft_clip((d * g) >> 8) * dc) >> 15;
                if (crush) d = (clamp15(d) >> crush) << crush;
                if (held_on) { if (pre_take[j]) held = d; d = held; }
                x += ((d - x) * mixd) >> 15;
            }
            if (hp) { /* 12 dB/oct: two one-pole high passes */
                h1 += ((x - h1) * hp) >> 15; x -= h1;
                h2 += ((x - h2) * hp) >> 15; x -= h2;
            }
            if (lp < 32767) { l1 += ((x - l1) * lp) >> 15; l2 += ((l1 - l2) * lp) >> 15; x = l2; }
            *p = x;
        }
        hold_v[c] = held;
        filt[c][0] = h1; filt[c][1] = h2; filt[c][2] = l1; filt[c][3] = l2;
    }
}

/* Keys played live (VESPER play mode): queued by the control task, started by
 * the audio context at the top of the next block, so voices only ever change
 * in one context. */
static volatile uint32_t live_queue[16];
static volatile uint8_t live_write, live_read;
void lush_live_note(unsigned track, unsigned note, unsigned velocity)
{
    uint8_t w = live_write;
    if ((uint8_t)(w - live_read) >= 16U || track >= TRACKS || note > 127) return;
    live_queue[w & 15U] = track | note << 8 | (velocity > 127 ? 127U : velocity) << 16;
    live_write = (uint8_t)(w + 1U);
}
LUSH_CODE void lush_render(int32_t *output)
{
    while (live_read != live_write) {
        uint32_t e = live_queue[live_read & 15U];
        lush_note(e & 255U, (e >> 8) & 255U, e >> 16);
        live_read = (uint8_t)(live_read + 1U);
    }
    unsigned any = 0;
    for (unsigned t = 0; t < TRACKS; ++t) {
        if (layer[t].applied != layer[t].patch) {
            for (unsigned i = 0; i < VOICES; ++i)
                if (voices[i].active && voices[i].track == t) voices[i].active = 0;
            layer[t].applied = layer[t].patch;
        }
        any |= layer[t].patch;
    }
    beat_phase += beat_inc * N;
    { unsigned n; struct voice *o = oldest_synth(&n); if (o && n > synth_budget) { o->kill = 1; o->filter.stage = STAGE_RELEASE; } }
    sram_clear(bus_dry, 2 * N); sram_clear(bus_chorus, N); sram_clear(bus_reverb, N); sram_clear(bus_delay, N);
    unsigned voicing = 0;
    for (unsigned t = 0; t < TRACKS; ++t) if (track_level[t]) --track_level[t]; /* ~0.4 s fall */
    for (unsigned i = 0; i < VOICES; ++i)
        if (voices[i].active) {
            unsigned l = (unsigned)(((int64_t)voices[i].amp_now * voices[i].velocity) >> 23); /* 0..127 */
            if (l > 127U) l = 127U;
            if (l > track_level[voices[i].track]) track_level[voices[i].track] = (uint8_t)l;
#if FM1_HOST_TEST
            ++lush_stat[voice_patch[voices[i].track].ks == 2 ? 1 : 0];
#endif
            render_voice(&voices[i]); voicing = 1;
        }
    if (!any && !voicing) return; /* effects tails are allowed to stop with the engine */

    int32_t wet = fx_wet, feedback = fx_feedback;
    /* Dual-tap stereo delay, the classic two-delay worship guitar rig:
     * left repeats at the FX time (dotted eighth by default), right at 4/3 of
     * it (a quarter), and both feed back, so echoes interlock across the field. */
    static uint32_t delay_now;
    if (!delay_now) delay_now = delay_q16;
    /* A tempo change glides the delay time (tape-style) instead of jumping and clicking. */
    delay_now += (uint32_t)((int32_t)(delay_q16 - delay_now) >> 5);
    uint32_t dtime = delay_now, dtime_r = (uint32_t)(((uint64_t)dtime * 4U) / 3U);
    if (dtime_r > ((uint32_t)(DELAY_LEN - 2) << 16)) dtime_r = (uint32_t)(((uint64_t)dtime * 2U) / 3U);
    pre_fx(); /* DIRT and TAPE first, on every bus */
    const int32_t width = fx_width, chorus_mix = fx_chorus, chorus_depth = fx_chorus_depth, grains_on = fx_grains; /* read once per block */
    const int32_t level_thr = fx_level_thr, out_gain = fx_out_gain;
    const int32_t grain_wet = grains_on * 420 > 29500 ? 29500 : grains_on * 420, grain_feedback = grains_on * 170;
    for (unsigned j = 0; j < N; ++j) {
        /* chorus */
        int32_t mono = bus_chorus[j];
        chorus_line[chorus_write] = (int16_t)clamp15(mono >> 1);
        chorus_lfo += 53500U; /* ~0.55 Hz */
        int32_t tri = (int32_t)(chorus_lfo >> 16); tri = tri < 32768 ? tri : 65535 - tri;
        uint32_t base = 260U << 16, depth = (uint32_t)chorus_depth;
        int32_t cl = chorus_tap(base + (uint32_t)tri * depth);
        int32_t cr = chorus_tap(base + (uint32_t)(32767 - tri) * depth);
        chorus_write = (chorus_write + 1U) & (CHORUS_LEN - 1U);
        /* dry centre plus two counter-swept copies, Juno style */
        int32_t l = bus_dry[2 * j] + (((mono + 2 * cl) * chorus_mix) >> 15);
        int32_t r = bus_dry[2 * j + 1] + (((mono + 2 * cr) * chorus_mix) >> 15);
        /* half-rate delay and reverb, linearly interpolated back up */
        if (!(j & 1U)) {
            int32_t in = (bus_delay[j] + bus_delay[j + 1]) >> 1;
            int32_t rsend = (bus_reverb[j] + bus_reverb[j + 1]) >> 2;
            if (chorus_mix) { /* CLOUD chorus feeds SPACE; half the sweep at half rate */
                int32_t send_wet = chorus_mix >> 1; /* 30 % at the scene's setting */
                in = send_chorus(0, in, (base + (uint32_t)tri * depth) >> 1, send_wet);
                rsend = send_chorus(1, rsend, (base + (uint32_t)(32767 - tri) * depth) >> 1, send_wet);
                ++send_write;
            }
            /* Low-mid cut on what goes into the delay: everything below ~400 Hz
             * about 8 dB down, so stacked effects do not build up mud (0.8.9). */
            send_shelf[0] += ((in - send_shelf[0]) * 3526) >> 15;
            in -= (send_shelf[0] * 5) >> 3;
            /* Tape echo: slow wow plus a little flutter bend the repeat time
             * (about +-0.3 %), and each pass is saturated and darkened. */
            tape_wow += 68000U;      /* ~0.35 Hz at 22.05 kHz */
            tape_flutter += 1150000U; /* ~5.9 Hz */
            int32_t wobble = sine15(tape_wow) + (sine15(tape_flutter) >> 3);
            int32_t bend = (int32_t)(((int64_t)(dtime >> 9) * wobble) >> 15); /* +-dtime/512 */
            int32_t echo = delay_tap((uint32_t)((int32_t)dtime + bend));
            int32_t echo_r = delay_tap((uint32_t)((int32_t)dtime_r + bend));
            int32_t mix = (echo + echo_r) >> 1;
            delay_lp += ((mix - delay_lp) * 13500) >> 15;              /* ~2.4 kHz: warm, not muddy, repeats */
            delay_hp += ((delay_lp - delay_hp) * 1100) >> 15;          /* ~120 Hz: no low build-up */
            int32_t back = clamp15(delay_lp - delay_hp);
            int32_t b2 = (back * back) >> 15;
            back = back - (((b2 * back) >> 15) * 10923 >> 15);        /* tape saturation: x - x^3/3 */
            delay_line[delay_write] = (int16_t)clamp15(in + ((back * feedback) >> 15));
            if (++delay_write == DELAY_LEN) delay_write = 0;
            delay_prev[0] = delay_out[0]; delay_prev[1] = delay_out[1];
            delay_out[0] = echo; delay_out[1] = echo_r;
            if (grains_on) { /* CLOUD GRAINS: the whole sound (in series, like a pedal), its own
                              * output and a little reverb go in; the cloud feeds SPACE too */
                int32_t whole = (bus_dry[2 * j] + bus_dry[2 * j + 1] + bus_dry[2 * j + 2] + bus_dry[2 * j + 3]) >> 2;
                whole += (bus_chorus[j] + bus_chorus[j + 1]) >> 1;
                int32_t own = (grain_out[0] + grain_out[1]) >> 1;
                grain_prev[0] = grain_out[0]; grain_prev[1] = grain_out[1];
                cloud_sample(whole + in + ((own * grain_feedback) >> 15)
                             + ((((rev_out[0] + rev_out[1]) >> 1) * grains_on * 60) >> 15), grains_on, grain_out);
                rsend += (((grain_out[0] + grain_out[1]) >> 1) * grains_on * 100) >> 15;
            }
            int32_t rin = rsend + ((mix * 6000) >> 15);
            send_shelf[1] += ((rin - send_shelf[1]) * 3526) >> 15;    /* same low-mid cut into the reverb */
            rin -= (send_shelf[1] * 5) >> 3;
            rev_prev[0] = rev_out[0]; rev_prev[1] = rev_out[1];
            reverb_sample(rin, rev_out);
        }
        int32_t half = j & 1U;
        int32_t dl = half ? delay_out[0] : (delay_prev[0] + delay_out[0]) >> 1;
        int32_t dr = half ? delay_out[1] : (delay_prev[1] + delay_out[1]) >> 1;
        int32_t rl = half ? rev_out[0] : (rev_prev[0] + rev_out[0]) >> 1;
        int32_t rr = half ? rev_out[1] : (rev_prev[1] + rev_out[1]) >> 1;
        l += ((((dl * 13) >> 3) + ((rl * 9) >> 3)) * wet) >> 15; /* echoes 1.6x louder than 0.8 */
        r += ((((dr * 13) >> 3) + ((rr * 9) >> 3)) * wet) >> 15;
        if (grains_on) {
            int32_t gl = half ? grain_out[0] : (grain_prev[0] + grain_out[0]) >> 1;
            int32_t gr = half ? grain_out[1] : (grain_prev[1] + grain_out[1]) >> 1;
            l += (gl * grain_wet) >> 15; r += (gr * grain_wet) >> 15;
        }
        if (width != 32768) { /* CLOUD width, last in the chain */
            int32_t m = (l + r) >> 1, side = (int32_t)(((int64_t)((l - r) >> 1) * width) >> 15);
            l = m + side; r = m - side;
        }
        if (level_thr) { /* LEVEL: smooth 2:1 leveler, linked stereo, ~1 ms attack, ~0.25 s release */
            int32_t a = l < 0 ? -l : l, b = r < 0 ? -r : r;
            if (b > a) a = b;
            level_env += a > level_env ? ((a - level_env) * 700) >> 15 : ((a - level_env) * 3) >> 15;
            if (!(j & 15U)) { /* new gain every 16 samples: sqrt(threshold / level) above the threshold */
                int32_t target = 32767;
                if (level_env > level_thr) {
                    int32_t x = (level_thr << 15) / level_env;                     /* Q15, < 1; 32-bit hardware divide */
                    int32_t root = (x + 32768) >> 1;                             /* Newton sqrt in Q15 */
                    for (unsigned k = 0; k < 4; ++k) root = (root + (x << 15) / root) >> 1;
                    target = root;
                }
                level_target = target;
            }
            level_gain += (level_target - level_gain) >> 6;
            l = (int32_t)(((int64_t)l * level_gain) >> 15); r = (int32_t)(((int64_t)r * level_gain) >> 15);
        }
        if (out_gain != 4096) { l = (l * out_gain) >> 12; r = (r * out_gain) >> 12; }
        /* -1 dB bus trim keeps the limiter for rare peaks only. */
        output[2 * j] += soft_limit(dc_block(0, analog(0, (l * 29200) >> 15))) << 8;
        output[2 * j + 1] += soft_limit(dc_block(1, analog(1, (r * 29200) >> 15))) << 8;
    }
}
