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

enum { TICK = 16, VOICES = 16, KS_VOICES = 8, KS_LEN = 1024, TRACKS = 8 };
enum { STAGE_OFF, STAGE_ATTACK, STAGE_DECAY, STAGE_SUSTAIN, STAGE_RELEASE };
#define ONE30 (1 << 30)

struct patch {
    uint8_t ks;                     /* 0 analog oscillators, 1 plucked string */
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
};
static FM1_AUDIO_DATA const struct patch patches[LUSH_PATCHES] = {
    [LUSH_AIR_PAD] = {0, 100, 0, 18, 7, 3, 11, 5, 0, 0, 70, 64, 22, 18, 30, {96,104,40,100}, {92,100,108,103}, 40, 46, 1, 90, 0, 0},
    [LUSH_STRINGS] = {0, 88, 58, 0, 4, 2, 8, 4, 70, 0, 78, 64, 16, 12, 36, {85,98,52,96}, {84,96,112,99}, 50, 36, 1, 80, 0, 0},
    [LUSH_GLASS_PLUCK] = {0, 96, 52, 0, 3, 2, 7, 3, 64, 0, 55, 110, 46, 44, 24, {4,66,0,72}, {6,86,0,74}, 90, 58, 1, 70, 0, 0},
    [LUSH_HARP] = {1, 0, 0, 0, 0, 1, 0, 2, 0, 0, 0, 0, 0, 0, 0, {0,0,0,0}, {4,100,0,76}, 96, 70, 0, 70, 92, 124},
    [LUSH_FELT] = {1, 0, 0, 46, 0, 1, 0, 2, 0, 0, 0, 0, 0, 0, 0, {0,0,0,0}, {10,92,0,72}, 80, 62, 0, 60, 34, 122},
    [LUSH_WARM_BASS] = {0, 86, 0, 90, 0, 1, 0, 2, 0, -1, 46, 100, 26, 22, 16, {12,70,30,64}, {18,80,124,68}, 72, 64, 0, 0, 0, 0},
    [LUSH_SUB] = {0, 22, 0, 127, 0, 1, 0, 1, 0, 0, 50, 100, 4, 0, 0, {60,80,64,80}, {70,90,127,94}, 40, 70, 0, 0, 0, 0},
    [LUSH_AIR_BELL] = {1, 0, 0, 40, 0, 1, 0, 2, 0, 1, 0, 0, 0, 0, 0, {0,0,0,0}, {30,104,0,90}, 64, 40, 1, 100, 120, 126},
};
static const char *const names[LUSH_PATCHES] = {
    "FM ENGINE", "AIR PAD", "STRINGS", "GLASS PLUCK", "HARP", "FELT KEYS",
    "WARM BASS", "SUB", "AIR BELL"
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
    if (half > 17900) half = 17900;
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
    int32_t lp1, bp1, lp2, bp2;
    int32_t drift, drift_target;      /* cents, Q8 */
    uint32_t random;
    /* plucked string */
    uint32_t ks_delay;                /* Q16 samples */
    uint16_t ks_write;
    int32_t ks_lp, ks_gain, ks_coef;
};
static struct voice voices[VOICES];
static int16_t ks_memory[KS_VOICES][KS_LEN];
static uint16_t voice_clock;

static inline LUSH_CODE int32_t clamp15(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }
static inline LUSH_CODE uint32_t rng(uint32_t *s) { *s = *s * 1664525U + 1013904223U; return *s; }
static inline LUSH_CODE int32_t noise15(uint32_t *s) { return (int32_t)(rng(s) >> 17) - 16384; }

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
    if ((rng(&v->random) >> 24) < 6) v->drift_target = (int32_t)((rng(&v->random) >> 16) % (2U * p->drift * 256U + 1U)) - p->drift * 256;
    v->drift += (v->drift_target - v->drift) >> 6;
    envelope_tick(&v->amp, p->aenv);
    if (!p->ks) {
        envelope_tick(&v->filter, p->fenv);
        filter_tick(v, p);
        int32_t spread = p->detune * 256;
        uint32_t base = detune(v->base, v->drift);
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
    struct voice *v = allocate(p->ks);
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
    v->drift = v->drift_target = 0;
    for (unsigned i = 0; i < 4; ++i) v->phase[i] = rng(&v->random); /* free-running, like analog */
    unsigned slot = layer[track].pan_next++;
    int32_t offset = (slot & 1U) ? (int32_t)p->spread : -(int32_t)p->spread;
    if ((slot & 3U) >= 2) offset /= 2;
    v->pan = (uint8_t)(64 + (offset >> 1));
    if (p->ks) {
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
    int32_t saw_level = (p->saw * 258) / (p->unison ? p->unison : 1);
    for (unsigned at = 0; at < N; at += TICK) {
        int32_t from = v->amp_now;
        voice_tick(v, p);
        int32_t to = v->amp.level >> 15, step = (to - from) / TICK, amp = from;
        v->amp_now = to;
        for (unsigned k = 0; k < TICK; ++k) {
            int32_t s;
            if (p->ks) {
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
                int32_t osc = 0;
                if (p->saw) {
                    int32_t sum = saw15(v->phase[0], v->inc[0]);
                    if (p->unison >= 2) sum += saw15(v->phase[1], v->inc[1]);
                    if (p->unison >= 3) sum += saw15(v->phase[2], v->inc[2]);
                    osc += (sum * saw_level) >> 15;
                }
                if (p->pulse) {
                    uint32_t width = 0x18000000U + (uint32_t)p->pw * 0x00d00000U; /* 127 = square */
                    int32_t pulse = saw15(v->phase[0], v->inc[0]) - saw15(v->phase[0] + width, v->inc[0]);
                    osc += (pulse * p->pulse) >> 8;
                }
                if (p->sine) osc += (sine15(v->phase[3]) * p->sine) >> 7;
                if (p->noise) osc += (noise15(&v->random) * p->noise) >> 7;
                for (unsigned u = 0; u < 4; ++u) v->phase[u] += v->inc[u];
                /* Warm saturation, then a 4-pole state-variable low-pass. */
                int32_t x = clamp15(osc + ((osc * p->drive) >> 7));
                int32_t x2 = (x * x) >> 15;
                x = (3 * x - ((x2 * x) >> 15)) >> 1;
                v->lp1 += (int32_t)(((int64_t)v->f * v->bp1) >> 15);
                int32_t hp = x - v->lp1 - (int32_t)(((int64_t)v->q * v->bp1) >> 15);
                v->bp1 += (int32_t)(((int64_t)v->f * hp) >> 15);
                v->lp2 += (int32_t)(((int64_t)v->f * v->bp2) >> 15);
                hp = v->lp1 - v->lp2 - (int32_t)(((int64_t)44000 * v->bp2) >> 15);
                v->bp2 += (int32_t)(((int64_t)v->f * hp) >> 15);
                if (v->bp1 > (1 << 20) || v->bp1 < -(1 << 20)) v->bp1 >>= 1;
                if (v->bp2 > (1 << 20) || v->bp2 < -(1 << 20)) v->bp2 >>= 1;
                s = v->lp2;
            }
            amp += step;
            int32_t out = (int32_t)(((int64_t)s * amp) >> 15);
            out = (int32_t)(((int64_t)out * gain) >> 15);
            unsigned j = at + k;
            if (p->chorus) bus_chorus[j] += out;
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
enum { DELAY_LEN = 18000 };
static int16_t delay_line[DELAY_LEN];
static unsigned delay_write;
static int32_t delay_lp, delay_hp, delay_prev[2], delay_out[2];
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
    uint32_t dtime = delay_q16, haas = dtime + (290U << 16);
    if (haas > ((uint32_t)(DELAY_LEN - 2) << 16)) haas = dtime;
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
            int32_t echo = delay_tap(dtime), echo_r = delay_tap(haas);
            delay_lp += ((echo - delay_lp) * 9000) >> 15;              /* ~1.6 kHz: dark repeats */
            delay_hp += ((delay_lp - delay_hp) * 300) >> 15;           /* remove low mud */
            int32_t back = delay_lp - delay_hp;
            delay_line[delay_write] = (int16_t)clamp15(in + ((back * feedback) >> 15));
            if (++delay_write == DELAY_LEN) delay_write = 0;
            delay_prev[0] = delay_out[0]; delay_prev[1] = delay_out[1];
            delay_out[0] = echo; delay_out[1] = (echo_r * 3 + echo) >> 2;
            int32_t rin = ((bus_reverb[j] + bus_reverb[j + 1]) >> 2) + ((echo * 6000) >> 15);
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
        /* -4 dB bus trim keeps the limiter for rare peaks only. */
        output[2 * j] += soft_limit(dc_block(0, (l * 20650) >> 15)) << 8;
        output[2 * j + 1] += soft_limit(dc_block(1, (r * 20650) >> 15)) << 8;
    }
}
