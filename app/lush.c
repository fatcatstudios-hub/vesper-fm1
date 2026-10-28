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
#include "lush_tables.inc"
#include "lush_sea_dqt.inc"
#include "fm1_sea.h"

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
};
/* Warm voicing: soft 5-10 ms attacks (no clicks), darker cutoffs, little
 * resonance, more saturation, analog drift on every oscillator voice. */
static FM1_AUDIO_DATA const struct patch patches[LUSH_PATCHES] = {
    /*                    ks saw pul sin noi uni det drf pw oct cut key env res drv  fenv              aenv               vel gain ch spr br dec st */
    [LUSH_AIR_PAD]      = {0, 100,  0, 18,  4, 3, 11, 6,  0, 0, 66, 64, 20, 10, 40, {96,104,40,100}, {92,100,108,103},  40, 46, 1, 90,  0,  0, 0, 0, 0},
    [LUSH_STRINGS]      = {0,  88, 58,  0,  2, 2,  9, 6, 70, 0, 72, 64, 14,  8, 44, {85,98,52,96},   {84,96,112,99},    50, 36, 1, 80,  0,  0, 0, 0, 0},
    [LUSH_GLASS_PLUCK]  = {0,  96, 44,  0,  2, 2,  8, 5, 64, 0, 50,100, 36, 26, 44, {18,72,10,76},   {24,88,0,80},      90, 62, 1, 70,  0,  0, 0, 0, 0},
    [LUSH_HARP]         = {1,   0,  0, 20,  0, 1,  0, 2,  0, 0,  0,  0,  0,  0,  0, {0,0,0,0},       {22,100,0,80},     96, 96, 0, 70, 58,124, 0, 0, 0},
    [LUSH_FELT]         = {1,   0,  0, 52,  0, 1,  0, 2,  0, 0,  0,  0,  0,  0,  0, {0,0,0,0},       {24,94,0,76},      80, 90, 0, 60, 22,123, 0, 0, 0},
    [LUSH_WARM_BASS]    = {0,  86,  0, 90,  0, 1,  0, 3,  0,-1, 44,100, 22, 14, 40, {14,70,30,64},   {22,80,124,68},    72, 64, 0,  0,  0,  0, 0, 0, 0},
    [LUSH_SUB]          = {0,  22,  0,127,  0, 1,  0, 2,  0, 0, 48,100,  4,  0, 10, {60,80,64,80},   {70,90,127,94},    40, 70, 0,  0,  0,  0, 0, 0, 0},
    [LUSH_AIR_BELL]     = {1,   0,  0, 34,  0, 1,  0, 2,  0, 0,  0,  0,  0,  0,  0, {0,0,0,0},       {30,104,0,90},     64, 48, 1,100, 70,125, 0, 0, 0},
    [LUSH_DETUNE_PLUCK] = {0, 104, 30,  0,  2, 3, 16, 7, 60, 0, 52,104, 34, 22, 48, {20,74,12,76},   {24,90,0,82},      92, 64, 0,  0,  0,  0, 1, 0, 0},
    [LUSH_SUPERSAW]     = {0, 112,  0, 16,  2, 3, 22, 8,  0, 0, 64, 92, 24, 14, 46, {20,82,40,82},   {26,86,56,84},     80, 64, 0,  0,  0,  0, 1, 0, 0},
    [LUSH_JUNO_ARP]     = {0,  40,110,  0,  2, 2, 12, 7, 92, 0, 58,100, 32, 30, 44, {12,76,14,76},   {24,86,30,78},     88, 58, 1, 64,  0,  0, 0, 0, 0},
    /* Sampled: transient kept (fast attack), gentle velocity-tracked low-pass. sine_octave = transpose. */
    [LUSH_PIANO]        = {2,   0,  0,  0,  0, 1,  0, 0,  0, 0,108, 64,  0,  0,  8, {0,0,127,0},     {4,100,127,84},    72, 96, 0, 60,  0,  0, 0, 0, 4},
    [LUSH_MARIMBA]      = {2,   0,  0,  0,  0, 1,  0, 0,  0, 0,112, 64,  0,  0,  6, {0,0,127,0},     {4,100,127,78},    70, 96, 0, 70,  0,  0, 0, 4, 3},
    [LUSH_GLOCK]        = {2,   0,  0,  0,  0, 1,  0, 0,  0, 1,118, 64,  0,  0,  0, {0,0,127,0},     {4,100,127,92},    64, 66, 1, 90,  0,  0, 0, 7, 1},
    [LUSH_DETUNE_BASS]  = {0, 104,  0, 70,  0, 2, 11, 4,  0,-1, 42,100, 26, 20, 46, {14,68,20,62},   {22,76,90,64},     84, 88, 0,  0,  0,  0, 0, 0, 0},
};
static const char *const names[LUSH_PATCHES] = {
    "FM ENGINE", "AIR PAD", "STRINGS", "GLASS PLUCK", "HARP", "FELT KEYS",
    "WARM BASS", "SUB", "AIR BELL", "DETUNE PLUCK", "SUPERSAW", "JUNO ARP", "DETUNE BASS",
    "PIANO", "MARIMBA", "GLOCK"
};
const char *lush_patch_name(unsigned patch) { return patch < LUSH_PATCHES ? names[patch] : "?"; }

/* ---- layer and effect parameters (written by control, read by audio) ---- */
static struct {
    volatile uint8_t patch, applied;
    volatile int32_t gain, reverb, delay; /* Q15 */
    uint8_t pan_next;
} layer[TRACKS];
static volatile int32_t fx_decay = 26000, fx_feedback = 12000, fx_wet = 20000, fx_damp = 9000;
static volatile uint32_t delay_q16 = 11025U << 16; /* half-rate samples, Q16 */

LUSH_CODE unsigned lush_owns(unsigned track) { return track < TRACKS && layer[track].patch != LUSH_NONE; }

void lush_layer(unsigned track, unsigned patch, unsigned volume, unsigned reverb, unsigned delay)
{
    if (track >= TRACKS) return;
    if (patch >= LUSH_PATCHES) patch = LUSH_NONE;
    if (volume > 127) volume = 127;
    layer[track].gain = (int32_t)(volume * volume * 2U); /* gentle taper, 127 ~ 0.98 */
    layer[track].reverb = (int32_t)((reverb > 15 ? 15 : reverb) * 2184U);
    layer[track].delay = (int32_t)((delay > 15 ? 15 : delay) * 2184U);
    layer[track].patch = (uint8_t)patch;
}
void lush_effects(const uint8_t fx[4], unsigned delay_ticks)
{
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
    uint16_t age;
    int32_t velocity;                 /* Q15 */
    uint32_t phase[4], base;          /* saw 0..2, sine 3; base increment */
    uint32_t inc[4];
    struct envelope amp, filter;
    int32_t amp_now;                  /* Q15, interpolated per sample */
    int32_t f, q;                     /* filter coefficients, Q15 */
    int32_t lp1, bp1, lp2, bp2;       /* left / mono filter */
    int32_t rl1, rb1, rl2, rb2;       /* right filter for stereo unison */
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
};
static struct voice voices[VOICES];
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
void lush_sample_bind(unsigned slot, const uint8_t *data, unsigned bytes, unsigned frames, const char *name)
{
    if (slot >= BANK_SLOTS) return;
    bank[slot].data = 0; /* voices on this slot stop at their next decode */
    if (!data || !frames || bytes < (frames + 319U) / 320U * SEA_CHUNK_BYTES) return;
    bank[slot].bytes = bytes; bank[slot].frames = frames; bank[slot].root = (uint8_t)root_from_name(name);
    bank[slot].data = data;
}

static inline LUSH_CODE int32_t sine15(uint32_t phase)
{
    unsigned i = phase >> 22, frac = (phase >> 12) & 1023U;
    int32_t a = lush_sine[i], b = lush_sine[i + 1];
    return a + (((b - a) * (int32_t)frac) >> 10);
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
        e->level += (int32_t)(((int64_t)(target - e->level) * lush_env[t[0]]) >> 30);
        if (e->level >= ONE30) { e->level = ONE30; e->stage = STAGE_DECAY; }
        break;
    }
    case STAGE_DECAY: {
        int32_t target = (int32_t)t[2] << 23;
        e->level += (int32_t)(((int64_t)(target - e->level) * lush_env[t[1]]) >> 30);
        break;
    }
    case STAGE_RELEASE:
        e->level -= (int32_t)(((int64_t)e->level * lush_env[t[3]]) >> 30);
        break;
    default: break;
    }
}
static LUSH_CODE void filter_tick(struct voice *v, const struct patch *p)
{
    /* Cutoff in Q8 semitones: base + key tracking + envelope + drift. */
    int32_t c = ((int32_t)p->cutoff << 8) + ((((int32_t)v->note - 60) * p->keytrack) << 1)
              + (int32_t)(((int64_t)p->env_amount * (v->filter.level >> 14)) >> 8)
              + (v->drift >> 1) + ((v->velocity >> 7) * p->velocity >> 6);
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
        uint32_t base = detune(v->base, v->drift + v->offset);
        if (p->unison >= 3) {
            v->inc[0] = detune(base, -spread); v->inc[1] = base; v->inc[2] = detune(base, spread);
        } else if (p->unison == 2) {
            v->inc[0] = detune(base, -spread / 2); v->inc[1] = detune(base, spread / 2);
        } else v->inc[0] = base;
        v->inc[3] = p->sine_octave < 0 ? base >> 1 : p->sine_octave > 0 ? base << 1 : base;
    } else {
        v->inc[3] = p->sine_octave > 0 ? v->base << 1 : v->base;
    }
    if (v->amp.stage == STAGE_RELEASE && v->amp.level < (ONE30 >> 13)) v->active = 0;
}

static LUSH_CODE struct voice *allocate(unsigned want_ks)
{
    struct voice *best = 0;
    unsigned best_score = 0;
    unsigned limit = want_ks ? KS_VOICES : VOICES;
    /* Analog voices fill from the top so string voices keep their buffers. */
    for (unsigned n = 0; n < limit; ++n) {
        struct voice *v = &voices[want_ks ? n : VOICES - 1U - n];
        if (!v->active) return v;
        /* Prefer stealing released, quiet, old voices. */
        unsigned score = (uint16_t)(voice_clock - v->age) + (v->amp.stage == STAGE_RELEASE ? 40000U : 0U)
                       + (unsigned)((ONE30 - v->amp.level) >> 20);
        if (!best || score > best_score) { best = v; best_score = score; }
    }
    return best;
}
static LUSH_CODE void start_voice(unsigned track, unsigned note, unsigned velocity, unsigned patch_id)
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
    struct voice *v = allocate(p->ks == 1);
    if (!v) return;
    unsigned index = (unsigned)(v - voices);
    uint32_t seed = v->random ^ (note * 2654435761U) ^ voice_clock;
    v->active = 1; v->track = (uint8_t)track; v->note = (uint8_t)note; v->patch = (uint8_t)patch_id;
    v->age = ++voice_clock; v->random = seed | 1U;
    int32_t vel = (int32_t)velocity * 258;                                  /* Q15 */
    v->velocity = 32768 - (((32768 - vel) * p->velocity) >> 7);
    v->base = lush_inc[note];
    v->amp.level = 0; v->amp.stage = STAGE_ATTACK; v->amp_now = 0;
    v->filter.level = 0; v->filter.stage = STAGE_ATTACK;
    v->lp1 = v->bp1 = v->lp2 = v->bp2 = 0;
    v->rl1 = v->rb1 = v->rl2 = v->rb2 = 0;
    v->drift = v->drift_target = 0; v->drift_timer = 0;
    /* Each note lands slightly differently, like an analog poly's voice cards. */
    v->offset = (int32_t)((rng(&v->random) >> 16) % (p->drift * 256U + 1U)) - (int32_t)(p->drift * 128U);
    for (unsigned i = 0; i < 4; ++i) v->phase[i] = rng(&v->random); /* free-running, like analog */
    unsigned slot = layer[track].pan_next++;
    int32_t offset = (slot & 1U) ? (int32_t)p->spread : -(int32_t)p->spread;
    if ((slot & 3U) >= 2) offset /= 2;
    v->pan = (uint8_t)(64 + (offset >> 1));
    if (p->ks == 2) {
        v->zone = (uint8_t)zone;
        v->sample_step = (uint32_t)((((uint64_t)lush_inc[target] << 16) * 32000U) /
                                    ((uint64_t)lush_inc[bank[zone].root] * 44100U));
        v->sample_pos = (rng(&v->random) >> 29);   /* 0..7 frames: no two hits identical */
        v->sample_frac = 0; v->sample_next = 0xffffffffU;
        for (unsigned i = 0; i < 8; ++i) v->sample_state[i] = 0;
        v->velocity -= (int32_t)((rng(&v->random) >> 28) * 160U); /* up to ~-0.7 dB */
        v->amp.level = ONE30 - (ONE30 >> 6); v->amp.stage = STAGE_DECAY; /* keep the recorded attack */
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
LUSH_CODE void lush_note(unsigned track, unsigned note, unsigned velocity)
{
    if (track >= TRACKS || note > 127) return;
    unsigned patch_id = layer[track].patch;
    if (patch_id == LUSH_NONE) return;
    if (velocity) { start_voice(track, note, velocity, patch_id); return; }
    for (unsigned i = 0; i < VOICES; ++i) {
        struct voice *v = &voices[i];
        if (v->active && v->track == track && v->note == note && v->amp.stage != STAGE_RELEASE) {
            v->amp.stage = STAGE_RELEASE; v->filter.stage = STAGE_RELEASE;
        }
    }
}
LUSH_CODE void lush_track_reset(unsigned track)
{
    for (unsigned i = 0; i < VOICES; ++i)
        if (voices[i].active && voices[i].track == track) {
            voices[i].amp.stage = STAGE_RELEASE; voices[i].filter.stage = STAGE_RELEASE;
        }
}

/* Saturation then a 4-pole state-variable low-pass (two Chamberlin stages). */
static inline LUSH_CODE int32_t warm_filter(int32_t osc, const struct patch *p, int32_t f, int32_t q,
                                            int32_t *lp1, int32_t *bp1, int32_t *lp2, int32_t *bp2)
{
    int32_t x = clamp15(osc + ((osc * p->drive) >> 7));
    int32_t x2 = (x * x) >> 15;
    x = (3 * x - ((x2 * x) >> 15)) >> 1;
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
    int32_t left = (int32_t)(128 - v->pan) * 256, right = (int32_t)v->pan * 256;
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
            if (p->ks == 2) {
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
                if (p->sine) {
                    s += (sine15(v->phase[3]) * p->sine) >> 8;
                    v->phase[3] += v->inc[3];
                }
            } else {
                int32_t a = 0, b = 0, c = 0, common = 0;
                if (p->saw) {
                    a = saw15(v->phase[0], v->inc[0]);
                    if (p->unison >= 2) b = saw15(v->phase[1], v->inc[1]);
                    if (p->unison >= 3) c = saw15(v->phase[2], v->inc[2]);
                }
                if (p->pulse) {
                    uint32_t width = 0x18000000U + (uint32_t)p->pw * 0x00d00000U; /* 127 = square */
                    int32_t pulse = saw15(v->phase[0], v->inc[0]) - saw15(v->phase[0] + width, v->inc[0]);
                    if (p->unison >= 2) /* detuned second pulse for the classic chorused arp */
                        pulse += saw15(v->phase[1], v->inc[1]) - saw15(v->phase[1] + width, v->inc[1]);
                    common += (pulse * p->pulse) >> (p->unison >= 2 ? 9 : 8);
                }
                if (p->sine) common += (sine15(v->phase[3]) * p->sine) >> 7;
                if (p->noise) common += (noise15(&v->random) * p->noise) >> 7;
                for (unsigned u = 0; u < 4; ++u) v->phase[u] += v->inc[u];
                if (p->stereo) {
                    /* Unison split across the field: outer saws hard left/right, centre shared. */
                    int32_t l = p->unison >= 3 ? a + (b >> 1) : a, r = p->unison >= 3 ? c + (b >> 1) : b;
                    l = ((l * saw_level) >> 15) + common;
                    r = ((r * saw_level) >> 15) + common;
                    s = warm_filter(l, p, v->f, v->q, &v->lp1, &v->bp1, &v->lp2, &v->bp2);
                    s_right = warm_filter(r, p, v->f, v->q, &v->rl1, &v->rb1, &v->rl2, &v->rb2);
                } else {
                    int32_t osc = (((a + b + c) * saw_level) >> 15) + common;
                    s = warm_filter(osc, p, v->f, v->q, &v->lp1, &v->bp1, &v->lp2, &v->bp2);
                }
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
    int32_t x2 = (x * x) >> 15;
    int32_t y = x + ((x2 * 2949) >> 15) - ((((x2 * x) >> 15) * 3932) >> 15); /* +0.09x^2 -0.12x^3 */
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
    if (x > knee) { int32_t over = x - knee; return knee + (over * 9830) / (9830 + over); }
    if (x < -knee) { int32_t over = -knee - x; return -knee - (over * 9830) / (9830 + over); }
    return x;
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
    for (unsigned i = 0; i < 2 * N; ++i) bus_dry[i] = 0;
    for (unsigned i = 0; i < N; ++i) bus_chorus[i] = bus_reverb[i] = bus_delay[i] = 0;
    unsigned voicing = 0;
    for (unsigned i = 0; i < VOICES; ++i)
        if (voices[i].active) { render_voice(&voices[i]); voicing = 1; }
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
