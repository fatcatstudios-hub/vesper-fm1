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
/* Phase increment for a MIDI note: the top octave (notes 120..131), shifted down. */
static inline LUSH_CODE uint32_t note_inc(unsigned note)
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
};
/* Warm voicing: soft 5-10 ms attacks (no clicks), darker cutoffs, little
 * resonance, more saturation, analog drift on every oscillator voice. */
static FM1_AUDIO_DATA const struct patch patches[LUSH_PATCHES] = {
    /*                    ks saw pul sin noi uni det drf pw oct cut key env res drv  fenv              aenv               vel gain ch spr br dec st */
    [LUSH_GLASS_PLUCK]  = {0,  96, 44,  0,  0, 2,  8, 5, 64, 0, 50,100, 36, 26, 44, {18,72,10,76},   {24,88,0,80},      90, 62, 1, 70,  0,  0, 0, 0, 0, 10, 4, 40, 0,0,0},
    [LUSH_WARM_BASS]    = {0,  86,  0, 90,  0, 1,  0, 3,  0,-1, 44,100, 22, 14, 40, {14,70,30,64},   {22,80,124,68},    72, 64, 0,  0,  0,  0, 0, 0, 0, 0,0, 0, 0,0,0},
    [LUSH_SUB]          = {0,  22,  0,127,  0, 1,  0, 2,  0, 0, 48,100,  4,  0, 10, {60,80,64,80},   {70,90,127,94},    40, 70, 0,  0,  0,  0, 0, 0, 0, 0,0, 0, 0,0,0},
    [LUSH_DETUNE_PLUCK] = {0, 104, 30,  0,  0, 3, 12, 7, 60, 0, 52,104, 34, 22, 48, {20,74,12,76},   {24,90,0,82},      92, 64, 0,  0,  0,  0, 1, 0, 0, 12, 5, 0, 0,0,0},
    [LUSH_SUPERSAW]     = {0, 112,  0, 16,  0, 3, 15, 8,  0, 0, 64, 92, 24, 14, 46, {20,82,40,82},   {26,86,56,84},     80, 64, 0,  0,  0,  0, 1, 0, 0, 14, 6, 0, 0,0,0},
    [LUSH_JUNO_ARP]     = {0,  40,110,  0,  0, 2, 9, 7, 92, 0, 58,100, 32, 30, 44, {12,76,14,76},   {24,86,30,78},     88, 58, 1, 64,  0,  0, 0, 0, 0, 12, 2, 30, 0,0,0},
    /* Sampled: transient kept (fast attack), gentle velocity-tracked low-pass. sine_octave = transpose. */
    [LUSH_PIANO]        = {2,   0,  0,  0,  0, 1,  0, 0,  0, 0,108, 64,  0,  0,  8, {0,0,127,0},     {4,100,127,84},    72, 124, 0, 60,  0,  0, 0, 0, 4, 6, 6, 50, 0,0,0},
    [LUSH_MARIMBA]      = {2,   0,  0,  0,  0, 1,  0, 0,  0, 0,112, 64,  0,  0,  6, {0,0,127,0},     {4,100,127,78},    70, 122, 0, 70,  0,  0, 0, 4, 3, 8, 5, 60, 0,0,0},
    [LUSH_GLOCK]        = {2,   0,  0,  0,  0, 1,  0, 0,  0, 1,118, 64,  0,  0,  0, {0,0,127,0},     {4,100,127,92},    64, 92, 1, 90,  0,  0, 0, 7, 1, 0, 0, 80, 0,0,0},
    /* Omnisphere-style hybrids: the air halo is the point. */
    [LUSH_HALO_PLUCK]   = {0,  70, 40,  0,  0, 2, 10, 6, 70, 0, 48, 96, 40, 30, 40, {16,74,8,78},    {20,82,0,80},      86, 38, 1, 70,  0,  0, 0, 0, 0, 10,6, 70, 0,0,0},
    /* Beyond GM. ks 3 = modal (tuned partials), formant voices use the oscillator path. */
    [LUSH_HANDPAN]      = {3,   0,  0,  0,  0, 1,  0, 0,  0, 0,  0,  0,  0,  0,  0, {0,0,127,0},     {14,100,127,86},   70,54, 0, 70,  0,  0, 0, 0, 0, 0,0, 40, 1,0,0},
    [LUSH_KALIMBA]      = {3,   0,  0,  0,  0, 1,  0, 0,  0, 0,  0,  0,  0,  0,  0, {0,0,127,0},     {4,100,127,80},    76,62, 0, 80,  0,  0, 0, 0, 0, 0,0, 50, 2,0,0},
    [LUSH_VOX_OOH]      = {0,  96,  0,  0,  0, 2,  6, 3,  0, 0,  0,  0,  0,  0,  0, {78,96,100,84},  {40,90,110,84},    60,100, 1, 80,  0,  0, 0, 0, 0, 0,0, 40, 0,1,0},
    [LUSH_VOX_EE]       = {0,  96,  0,  0,  0, 2,  6, 3,  0, 0,  0,  0,  0,  0,  0, {80,96,100,84},  {40,90,110,84},    60,100, 1, 80,  0,  0, 0, 0, 0, 0,0, 40, 0,2,0},
    [LUSH_MUTED_GTR]    = {1,   0,  0,  0,  0, 1,  0, 2,  0, 0, 82, 64,  0, 40, 30, {0,0,127,0},     {6,100,0,64},      90,127, 0, 50, 76, 96, 0, 0, 0, 0,0, 30, 0,0,0},
    [LUSH_REV_PIANO]    = {2,   0,  0,  0,  0, 1,  0, 0,  0, 0,108, 64,  0,  0,  0, {0,0,127,0},     {40,100,127,70},   60,120, 0, 70,  0,  0, 0, 0, 4, 0,0, 60, 0,0,2},
    [LUSH_REV_GLOCK]    = {2,   0,  0,  0,  0, 1,  0, 0,  0, 1,118, 64,  0,  0,  0, {0,0,127,0},     {40,100,127,70},   60,100, 1, 80,  0,  0, 0, 7, 1, 0,0, 60, 0,0,2},
    [LUSH_DETUNE_BASS]  = {0, 104,  0, 70,  0, 2, 11, 4,  0,-1, 42,100, 26, 20, 46, {14,68,20,62},   {22,76,90,64},     84, 88, 0,  0,  0,  0, 0, 0, 0, 0,0, 0, 0,0,0},
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
static volatile int32_t fx_decay = 26000, fx_feedback = 12000, fx_wet = 20000, fx_damp = 9000;
static volatile uint32_t delay_q16 = 11025U << 16; /* half-rate samples, Q16 */
static volatile uint32_t beat_inc = 0;    /* per-sample step of a 16-beat (4-bar) phase */
static volatile int32_t fx_shimmer = 0;   /* Q15 */
static uint32_t beat_phase;

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
    layer[track].patch = (uint8_t)patch;
}
__attribute__((minsize)) void lush_effects(const uint8_t fx[4], unsigned delay_ticks, unsigned tempo, unsigned shimmer)
{
    lush_tables_build();
    if (tempo >= 30 && tempo <= 300)
        beat_inc = (uint32_t)(((uint64_t)tempo << 32) / (60ULL * 44100ULL * 16ULL));
    fx_shimmer = (int32_t)(shimmer > 127 ? 127 : shimmer) * 150;   /* 0 .. 0.58 */
    fx_decay = 15000 + (int32_t)fx[0] * 121;   /* 0.46 .. 0.93 */
    fx_damp = 13000 - (int32_t)fx[0] * 40;     /* longer tails are a touch brighter */
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
static struct voice voices[VOICES];
#if FM1_HOST_TEST
uint32_t lush_stat[2]; /* voice-blocks rendered: synth/string/modal, sample */
#endif
static int16_t ks_memory[KS_VOICES][KS_LEN];
static uint16_t voice_clock;

static inline LUSH_CODE int32_t clamp15(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }
static inline LUSH_CODE uint32_t rng(uint32_t *s) { *s = *s * 1664525U + 1013904223U; return *s; }
static inline LUSH_CODE int32_t noise15(uint32_t *s) { return (int32_t)(rng(s) >> 17) - 16384; }

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

static inline LUSH_CODE int32_t sine15(uint32_t phase)
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
static inline LUSH_CODE int32_t saw15(uint32_t ph, uint32_t inc)
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
static inline LUSH_CODE uint32_t detune(uint32_t inc, int32_t cents_q8)
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
static inline LUSH_CODE int32_t coef_q8(int32_t c)
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
    const struct patch *p = &patches[patch_id];
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
     * keep two notes, strings three, samples four (reverse swells two). Over the limit, the layer's
     * oldest note fades out in ~4 ms. */
    unsigned cap = patch_id == LUSH_WARM_BASS || patch_id == LUSH_SUB || patch_id == LUSH_DETUNE_BASS ? 1U
                 : p->ks == 2 ? (p->reverse ? 2U : 4U) : p->ks == 1 ? 3U : 2U;
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
    v->lp1 = v->bp1 = v->lp2 = v->bp2 = 0;
    v->rl1 = v->rb1 = v->rl2 = v->rb2 = 0;
    v->last_l = v->last_r = 0;
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
        v->sample_step = (uint32_t)((((uint64_t)note_inc(target) << 16) * 32000U) /
                                    ((uint64_t)note_inc(bank[zone].root) * 44100U));
        v->sample_pos = (rng(&v->random) >> 29);   /* 0..7 frames: no two hits identical */
        v->sample_frac = 0; v->sample_next = 0xffffffffU;
        for (unsigned i = 0; i < 8; ++i) v->sample_state[i] = 0;
        v->velocity -= (int32_t)((rng(&v->random) >> 28) * 160U); /* up to ~-0.7 dB */
        v->amp.level = ONE30 - (ONE30 >> 6); v->amp.stage = STAGE_DECAY; /* keep the recorded attack */
        if (p->reverse) {
            /* Start far enough into the sample that, played backwards, it reaches
             * the attack exactly `reverse` beats from now. */
            uint32_t span = beat_inc ? (uint32_t)((((uint64_t)1 << 28) * p->reverse) / beat_inc) : 35280U;
            uint64_t frames = ((uint64_t)span * v->sample_step) >> 16;
            uint32_t last = bank[zone].frames - 2U;
            v->sample_pos = frames < last ? (uint32_t)frames : last;
            v->reverse_chunk = -1;
            v->amp.level = 0; v->amp.stage = STAGE_ATTACK;
        }
    }
    if (p->ks == 1) {
        /* Plucked string: delay of one period, minus the loop filter's half sample. */
        uint32_t period = (uint32_t)(((uint64_t)1 << 48) / v->base);
        if (period > ((KS_LEN - 4U) << 16)) period = (KS_LEN - 4U) << 16;
        if (period < (3U << 16)) period = 3U << 16;
        v->ks_coef = 4000 + p->bright * 220;        /* loop low-pass: brightness */
        /* The one-pole loop filter delays by (1-a)/a samples; keep strings in tune. */
        uint32_t lag = (uint32_t)((((uint64_t)(32768 - v->ks_coef)) << 16) / (uint32_t)v->ks_coef);
        v->ks_delay = period > lag + (2U << 16) ? period - lag : 2U << 16;
        v->ks_write = 0;
        v->ks_gain = 32000 + p->decay * 6;          /* loop gain: sustain */
        v->ks_lp = 0;
        int16_t *line = ks_memory[index];
        unsigned length = (period >> 16) + 2U;
        int32_t tone = 3000 + p->bright * 230 + ((v->velocity * p->bright) >> 8), state = 0;
        if (tone > 32000) tone = 32000;
        for (unsigned i = 0; i < KS_LEN; ++i) line[i] = 0;
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
    unsigned patch_id = layer[track].patch;
    if (patch_id == LUSH_NONE) return;
    if (velocity) { start_voice(track, note, velocity, patch_id); return; }
    if (patches[patch_id].reverse) return; /* a swell always lands on its beat */
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
static inline LUSH_CODE int32_t warm_filter(int32_t osc, const struct patch *p, int32_t f, int32_t q,
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
static LUSH_CODE int32_t sample_at(struct voice *v, uint32_t position)
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
    const struct patch *p = &patches[v->patch];
    int32_t gain = (int32_t)(((int64_t)layer[v->track].gain * v->velocity) >> 15);
    gain = (gain * p->gain) >> 7;
    int32_t send_r = layer[v->track].reverb, send_d = layer[v->track].delay;
    int32_t pan = v->pan;
    if (p->pan_move) /* slow 2-bar drift across the field, each voice on its own phase */
        pan += (sine15((beat_phase << 1) + (uint32_t)v->age * 0x28000000U) * p->pan_move) >> 16;
    pan = pan < 0 ? 0 : pan > 128 ? 128 : pan;
    int32_t left = (int32_t)(128 - pan) * 256, right = (int32_t)pan * 256;
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
                        int32_t state[8] = {0}, failed = 0;  /* whole chunk, group by group */
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
                s = warm_filter(x, p, v->f, v->q, &v->lp1, &v->bp1, &v->lp2, &v->bp2);
                if (v->sample_next == 0xfffffffeU) { v->amp.stage = STAGE_RELEASE; v->amp.level >>= 2; }
            } else if (p->ks == 2) {
                int32_t a = sample_at(v, v->sample_pos), b = sample_at(v, v->sample_pos + 1U);
                int32_t x = a + (int32_t)(((int64_t)(b - a) * (int32_t)v->sample_frac) >> 16);
                v->sample_frac += v->sample_step;
                v->sample_pos += v->sample_frac >> 16; v->sample_frac &= 0xffffU;
                s = warm_filter(x, p, v->f, v->q, &v->lp1, &v->bp1, &v->lp2, &v->bp2);
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
                bus_dry[2 * j] += out;
                bus_dry[2 * j + 1] += outr;
                out = (out + outr) >> 1; /* sends are mono */
            } else if (p->chorus) bus_chorus[j] += out;
            else {
                bus_dry[2 * j] += (out * left) >> 15;
                bus_dry[2 * j + 1] += (out * right) >> 15;
            }
            bus_reverb[j] += (out * send_r) >> 15;
            bus_delay[j] += (out * send_d) >> 15;
        }
    }
}

/* ---- chorus: Juno-style, mono in, stereo out ---- */
enum { CHORUS_LEN = 2048 };
static int16_t chorus_line[CHORUS_LEN];
static unsigned chorus_write;
static uint32_t chorus_lfo;
static inline LUSH_CODE int32_t chorus_tap(uint32_t delay_q16)
{
    uint32_t read = (((uint32_t)chorus_write << 16) - delay_q16) & ((CHORUS_LEN << 16) - 1U);
    unsigned i0 = read >> 16, i1 = (i0 + 1U) & (CHORUS_LEN - 1U);
    int32_t frac = (int32_t)((read >> 1) & 32767U);
    return chorus_line[i0] + (((chorus_line[i1] - chorus_line[i0]) * frac) >> 15);
}

/* ---- delay: dark, tempo-synced, half rate ---- */
enum { DELAY_LEN = 24000 }; /* 1.09 s at 22.05 kHz: a quarter note down to 55 BPM */
static int16_t delay_line[DELAY_LEN];
static unsigned delay_write;
static int32_t delay_lp, delay_hp, delay_prev[2], delay_out[2];
static uint32_t tape_wow, tape_flutter;
static inline LUSH_CODE int32_t delay_tap(uint32_t delay_q16)
{
    uint32_t pos = ((uint32_t)delay_write << 16) - delay_q16;
    if ((int32_t)pos < 0) pos += (uint32_t)DELAY_LEN << 16;
    unsigned i0 = pos >> 16, i1 = i0 + 1U == DELAY_LEN ? 0U : i0 + 1U;
    int32_t frac = (int32_t)((pos >> 1) & 32767U);
    return delay_line[i0] + (((delay_line[i1] - delay_line[i0]) * frac) >> 15);
}

/* ---- plate reverb (Dattorro), half rate ---- */
struct line { int16_t *buf; uint16_t length, write; };
#define LINE(name, n) static int16_t name##_mem[n]; static struct line name = {name##_mem, n, 0}
LINE(in1, 105); LINE(in2, 79); LINE(in3, 281); LINE(in4, 205);
LINE(l_ap1, 512); LINE(l_d1, 3299); LINE(l_ap2, 1334); LINE(l_d2, 2756);
LINE(r_ap1, 688); LINE(r_d1, 3124); LINE(r_ap2, 1968); LINE(r_d2, 2344);
static int32_t rev_bandwidth, rev_lp_l, rev_lp_r, rev_prev[2], rev_out[2], rev_low;
/* Shimmer: an octave-up granular pitch shifter in the reverb's feedback, so
 * the tail keeps blooming upward. Two read heads sweep a 46 ms window at
 * double speed, crossfaded with triangles (half rate, 22.05 kHz). */
enum { SHIMMER_LEN = 2048, SHIMMER_WINDOW = 1024 };
static int16_t shimmer_line[SHIMMER_LEN];
static unsigned shimmer_write, shimmer_phase;
static int32_t shimmer_lp, shimmer_hp;
static LUSH_CODE int32_t shimmer_sample(int32_t input)
{
    shimmer_line[shimmer_write] = (int16_t)clamp15(input);
    unsigned p1 = shimmer_phase, p2 = (shimmer_phase + SHIMMER_WINDOW / 2U) % SHIMMER_WINDOW;
    unsigned d1 = SHIMMER_WINDOW - p1 + 2U, d2 = SHIMMER_WINDOW - p2 + 2U;
    int32_t a = shimmer_line[(shimmer_write + SHIMMER_LEN - d1) & (SHIMMER_LEN - 1U)];
    int32_t b = shimmer_line[(shimmer_write + SHIMMER_LEN - d2) & (SHIMMER_LEN - 1U)];
    int32_t sn = sine15((uint32_t)p1 << 21);                      /* sin(pi * p1 / W) */
    int32_t g1 = (sn * sn) >> 15;                                  /* sin^2: smooth grain edges */
    int32_t y = (a * g1 + b * (32768 - g1)) >> 15;
    shimmer_write = (shimmer_write + 1U) & (SHIMMER_LEN - 1U);
    if (++shimmer_phase == SHIMMER_WINDOW) shimmer_phase = 0;
    shimmer_lp += ((y - shimmer_lp) * 16000) >> 15;          /* soften grain edges */
    shimmer_hp += ((shimmer_lp - shimmer_hp) * 2600) >> 15;  /* keep only the sparkle */
    return shimmer_lp - shimmer_hp;
}
static uint32_t rev_lfo;
static inline LUSH_CODE int32_t tap(const struct line *l, unsigned n)
{
    int i = (int)l->write - (int)n;
    if (i < 0) i += l->length;
    return l->buf[i];
}
static inline LUSH_CODE void push(struct line *l, int32_t v)
{
    l->buf[l->write] = (int16_t)clamp15(v);
    if (++l->write == l->length) l->write = 0;
}
static inline LUSH_CODE int32_t allpass(struct line *l, unsigned n, int32_t x, int32_t g)
{
    int32_t d = tap(l, n);
    int32_t v = x - ((d * g) >> 15);
    push(l, v);
    return d + ((v * g) >> 15);
}
static inline LUSH_CODE int32_t allpass_mod(struct line *l, uint32_t n_q16, int32_t x, int32_t g)
{
    unsigned n = n_q16 >> 16;
    int32_t frac = (int32_t)((n_q16 >> 1) & 32767U);
    int32_t a = tap(l, n), b = tap(l, n + 1U);
    int32_t d = a + (((b - a) * frac) >> 15);
    int32_t v = x - ((d * g) >> 15);
    push(l, v);
    return d + ((v * g) >> 15);
}
static LUSH_CODE void reverb_sample(int32_t input, int32_t out[2])
{
    int32_t decay = fx_decay, damp = fx_damp;
    if (fx_shimmer) /* feed the octave-up tail back in */
        input += (int32_t)(((int64_t)shimmer_sample((rev_out[0] + rev_out[1]) >> 1) * fx_shimmer) >> 15);
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
    int32_t left = x + ((tap(&r_d2, r_d2.length - 1U) * decay) >> 15);
    int32_t right = x + ((tap(&l_d2, l_d2.length - 1U) * decay) >> 15);
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
           - tap(&l_d1, 1474) - tap(&l_ap2, 139) - tap(&l_d2, 790);
    out[1] = tap(&l_d1, 262) + tap(&l_d1, 2687) - tap(&l_ap2, 910) + tap(&l_d2, 1981)
           - tap(&r_d1, 1564) - tap(&r_ap2, 248) - tap(&r_d2, 90);
}

static int32_t dc_in[2], dc_out[2]; /* dc_out is Q8 above the signal */
static int32_t analog_lp[2];
/* Analog output stage: gentle even+odd saturation, then a soft roll-off near
 * 9 kHz so nothing reads as glassy. Its small DC is removed by dc_block. */
static inline LUSH_CODE int32_t analog(unsigned ch, int32_t x)
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
static inline LUSH_CODE int32_t dc_block(unsigned ch, int32_t x)
{
    int32_t y = ((x - dc_in[ch]) << 8) + (int32_t)(((int64_t)dc_out[ch] * 32675) >> 15);
    dc_in[ch] = x; dc_out[ch] = y;
    return y >> 8;
}
static inline LUSH_CODE int32_t soft_limit(int32_t x)
{
    /* Transparent below ~0.7, rounds off peaks instead of hard clipping. */
    const int32_t knee = 23000;
    int32_t over = (x < 0 ? -x : x) - knee;
    if (over <= 0) return x;
    over = knee + (over * 9830) / (9830 + over);
    return x < 0 ? -over : over;
}

LUSH_CODE void lush_render(int32_t *output)
{
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
    for (unsigned i = 0; i < 2 * N; ++i) bus_dry[i] = 0;
    for (unsigned i = 0; i < N; ++i) bus_chorus[i] = bus_reverb[i] = bus_delay[i] = 0;
    unsigned voicing = 0;
    for (unsigned t = 0; t < TRACKS; ++t) if (track_level[t]) --track_level[t]; /* ~0.4 s fall */
    for (unsigned i = 0; i < VOICES; ++i)
        if (voices[i].active) {
            unsigned l = (unsigned)(((int64_t)voices[i].amp_now * voices[i].velocity) >> 23); /* 0..127 */
            if (l > 127U) l = 127U;
            if (l > track_level[voices[i].track]) track_level[voices[i].track] = (uint8_t)l;
#if FM1_HOST_TEST
            ++lush_stat[patches[voices[i].patch].ks == 2 ? 1 : 0];
#endif
            render_voice(&voices[i]); voicing = 1;
        }
    if (!any && !voicing) return; /* effects tails are allowed to stop with the engine */

    int32_t wet = fx_wet, feedback = fx_feedback;
    /* Dual-tap stereo delay, the classic two-delay worship guitar rig:
     * left repeats at the FX time (dotted eighth by default), right at 4/3 of
     * it (a quarter), and both feed back, so echoes interlock across the field. */
    uint32_t dtime = delay_q16, dtime_r = (uint32_t)(((uint64_t)dtime * 4U) / 3U);
    if (dtime_r > ((uint32_t)(DELAY_LEN - 2) << 16)) dtime_r = (uint32_t)(((uint64_t)dtime * 2U) / 3U);
    for (unsigned j = 0; j < N; ++j) {
        /* chorus */
        int32_t mono = bus_chorus[j];
        chorus_line[chorus_write] = (int16_t)clamp15(mono >> 1);
        chorus_lfo += 53500U; /* ~0.55 Hz */
        int32_t tri = (int32_t)(chorus_lfo >> 16); tri = tri < 32768 ? tri : 65535 - tri;
        uint32_t base = 260U << 16, depth = 160U;
        int32_t cl = chorus_tap(base + (uint32_t)tri * depth);
        int32_t cr = chorus_tap(base + (uint32_t)(32767 - tri) * depth);
        chorus_write = (chorus_write + 1U) & (CHORUS_LEN - 1U);
        /* dry centre plus two counter-swept copies, Juno style */
        int32_t l = bus_dry[2 * j] + (((mono + 2 * cl) * 19661) >> 15);
        int32_t r = bus_dry[2 * j + 1] + (((mono + 2 * cr) * 19661) >> 15);
        /* half-rate delay and reverb, linearly interpolated back up */
        if (!(j & 1U)) {
            int32_t in = (bus_delay[j] + bus_delay[j + 1]) >> 1;
            /* Tape echo: slow wow plus a little flutter bend the repeat time
             * (about +-0.3 %), and each pass is saturated and darkened. */
            tape_wow += 68000U;      /* ~0.35 Hz at 22.05 kHz */
            tape_flutter += 1150000U; /* ~5.9 Hz */
            int32_t wobble = sine15(tape_wow) + (sine15(tape_flutter) >> 3);
            int32_t bend = (int32_t)(((int64_t)(dtime >> 9) * wobble) >> 15); /* +-dtime/512 */
            int32_t echo = delay_tap((uint32_t)((int32_t)dtime + bend));
            int32_t echo_r = delay_tap((uint32_t)((int32_t)dtime_r + bend));
            int32_t mix = (echo + echo_r) >> 1;
            delay_lp += ((mix - delay_lp) * 9500) >> 15;               /* ~1.6 kHz: dark repeats */
            delay_hp += ((delay_lp - delay_hp) * 300) >> 15;           /* remove low mud */
            int32_t back = clamp15(delay_lp - delay_hp);
            int32_t b2 = (back * back) >> 15;
            back = back - (((b2 * back) >> 15) * 10923 >> 15);        /* tape saturation: x - x^3/3 */
            delay_line[delay_write] = (int16_t)clamp15(in + ((back * feedback) >> 15));
            if (++delay_write == DELAY_LEN) delay_write = 0;
            delay_prev[0] = delay_out[0]; delay_prev[1] = delay_out[1];
            delay_out[0] = echo; delay_out[1] = echo_r;
            int32_t rin = ((bus_reverb[j] + bus_reverb[j + 1]) >> 2) + ((mix * 6000) >> 15);
            rev_prev[0] = rev_out[0]; rev_prev[1] = rev_out[1];
            reverb_sample(rin, rev_out);
        }
        int32_t half = j & 1U;
        int32_t dl = half ? delay_out[0] : (delay_prev[0] + delay_out[0]) >> 1;
        int32_t dr = half ? delay_out[1] : (delay_prev[1] + delay_out[1]) >> 1;
        int32_t rl = half ? rev_out[0] : (rev_prev[0] + rev_out[0]) >> 1;
        int32_t rr = half ? rev_out[1] : (rev_prev[1] + rev_out[1]) >> 1;
        l += ((dl + ((rl * 9) >> 3)) * wet) >> 15;
        r += ((dr + ((rr * 9) >> 3)) * wet) >> 15;
        /* -1 dB bus trim keeps the limiter for rare peaks only. */
        output[2 * j] += soft_limit(dc_block(0, analog(0, (l * 29200) >> 15))) << 8;
        output[2 * j + 1] += soft_limit(dc_block(1, analog(1, (r * 29200) >> 15))) << 8;
    }
}
