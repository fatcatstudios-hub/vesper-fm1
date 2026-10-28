/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "instrument.h"
#include "fm_patch.h"
#include "drum808.h"
#include "sampledrum.h"
#include <string.h>

/* WORSHIP firmware scenes (Fat Cat Studios fork of FM1 Open).
 * Sixteen ambient worship/CCM scenes, each four layers: PAD, ARP, BASS and
 * a fourth layer (soft beat or sparse bell). Patterns are written as chord
 * DEGREES, not pitches, so CHORD FOLLOW can re-pitch every layer to whatever
 * chord is held on the keys (major, minor, sus2, sus4). Recipes expand into
 * the normal editable project format; no engine state is touched here. */
enum { KEYS, BASS, ARP, LEAD, PAD, BEAT, PERC, STAB };
enum { ELECTRIC, BELL, ORGAN, ROUND_BASS, WARM_PAD, REED,
       SOFT_EP, CLAV, GUITAR, MARIMBA, VIBES, BRASS, STRINGS, SUB_BASS, METAL, SOFT_LEAD };
struct part {
    uint8_t source : 3, role : 3;
    uint8_t tone, level, sends;
};
enum { LAYERS = 4 };
struct scene {
    char name[13];
    uint8_t bpm, root, decay, feedback, fx_level;
    struct part part[LAYERS];
    uint8_t pattern[LAYERS];
};
/* sends: reverb 0..15 + 16 * delay 0..15 */
#define FM(tone, role, level, rev, dly) {GROOVE_DX7, role, tone, level, rev + 16*dly}
#define PL(tone, role, level, rev, dly) {GROOVE_PLUCK, role, tone, level, rev + 16*dly}
#define DR(level) {GROOVE_808, BEAT, 0, level, 0}
/* Pattern ids: ARP/LEAD use arp_patterns, BASS uses bass_patterns,
 * BEAT uses beat_patterns; PAD ignores it (sustained open chord). */
enum { A_8THS, A_ROLL16, A_DOTTED, A_BELLS, A_UPDOWN, A_SPARSE, A_PULSE16, A_PAIRS };
enum { B_DRONE, B_PULSE8, B_QUARTER, B_HEART };
enum { D_HEART, D_FLOOR, D_HALF };
/* Roots are pitch classes (C=0). Tempos sit in the usual worship range. */
static const struct scene scenes[GROOVE_FACTORY_PROJECTS] = {
    {"DOTTED 8TH",  72, 4,110,58,88, {FM(WARM_PAD,PAD,78,13,0), FM(SOFT_EP,ARP,92,8,11), FM(SUB_BASS,BASS,84,2,0), FM(BELL,LEAD,58,13,9)},   {0,A_8THS,B_DRONE,A_SPARSE}},
    {"OPEN HEAVEN", 68, 7,115,52,90, {FM(STRINGS,PAD,76,14,0), FM(BELL,ARP,70,12,9), FM(SUB_BASS,BASS,82,2,0), PL(1,ARP,74,11,10)},      {0,A_BELLS,B_DRONE,A_DOTTED}},
    {"RISING TIDE", 76, 2,104,46,84, {FM(WARM_PAD,PAD,76,12,0), FM(SOFT_EP,ARP,84,7,8), FM(ROUND_BASS,BASS,84,1,0), DR(68)},           {0,A_ROLL16,B_PULSE8,D_HEART}},
    {"STILL WATER", 64, 9,118,56,92, {FM(WARM_PAD,PAD,80,15,0), FM(VIBES,ARP,74,11,9), FM(SUB_BASS,BASS,80,2,0), FM(BELL,LEAD,54,14,10)}, {0,A_UPDOWN,B_DRONE,A_SPARSE}},
    {"PULSE KEYS",  80,11,100,44,82, {FM(STRINGS,PAD,74,12,0), FM(SOFT_EP,ARP,86,6,8), FM(ROUND_BASS,BASS,86,1,0), DR(66)},            {0,A_PULSE16,B_PULSE8,D_FLOOR}},
    {"MORNING",     70, 0,108,54,88, {FM(WARM_PAD,PAD,78,13,0), PL(0,ARP,86,9,11), FM(SUB_BASS,BASS,82,2,0), FM(VIBES,LEAD,62,12,8)},  {0,A_8THS,B_DRONE,A_BELLS}},
    {"SANCTUARY",   66, 5,116,50,90, {FM(ORGAN,PAD,64,14,0), FM(BELL,ARP,68,12,10), FM(SUB_BASS,BASS,80,2,0), FM(STRINGS,PAD,58,14,0)}, {0,A_DOTTED,B_DRONE,0}},
    {"HEARTBEAT",   74, 4,106,48,86, {FM(WARM_PAD,PAD,76,12,0), FM(SOFT_EP,ARP,84,8,9), FM(ROUND_BASS,BASS,86,1,0), DR(70)},           {0,A_PAIRS,B_HEART,D_HEART}},
    {"GLORY BUILD", 84, 7, 98,42,80, {FM(STRINGS,PAD,74,11,0), FM(ELECTRIC,ARP,82,6,8), FM(ROUND_BASS,BASS,88,1,0), DR(72)},           {0,A_ROLL16,B_PULSE8,D_FLOOR}},
    {"WIDE OPEN",   72, 2,112,54,88, {FM(WARM_PAD,PAD,78,13,0), PL(1,ARP,82,10,10), FM(SUB_BASS,BASS,82,2,0), FM(BELL,LEAD,60,13,9)},  {0,A_UPDOWN,B_DRONE,A_BELLS}},
    {"ANTHEM 8THS", 88, 9, 96,40,80, {FM(STRINGS,PAD,72,11,0), FM(GUITAR,ARP,80,6,9), FM(ROUND_BASS,BASS,88,1,0), DR(72)},             {0,A_PULSE16,B_PULSE8,D_FLOOR}},
    {"QUIET PLACE", 60,10,120,56,92, {FM(WARM_PAD,PAD,82,15,0), FM(VIBES,ARP,70,12,10), FM(SUB_BASS,BASS,78,2,0), FM(SOFT_EP,LEAD,64,12,8)}, {0,A_SPARSE,B_DRONE,A_BELLS}},
    {"CASCADE",     76, 0,108,50,86, {FM(STRINGS,PAD,74,13,0), FM(BELL,ARP,70,10,9), FM(SUB_BASS,BASS,82,2,0), PL(0,ARP,72,10,10)},     {0,A_ROLL16,B_QUARTER,A_DOTTED}},
    {"SPIRIT MOVE", 78,11,104,48,84, {FM(WARM_PAD,PAD,76,12,0), FM(SOFT_EP,ARP,84,8,11), FM(ROUND_BASS,BASS,86,1,0), DR(66)},          {0,A_DOTTED,B_HEART,D_HALF}},
    {"ALTAR CALL",  62, 4,118,54,92, {FM(ORGAN,PAD,62,15,0), FM(VIBES,ARP,72,11,9), FM(SUB_BASS,BASS,80,2,0), FM(BELL,LEAD,56,14,10)},   {0,A_UPDOWN,B_DRONE,A_SPARSE}},
    {"FOREVER",     70, 8,110,52,88, {FM(STRINGS,PAD,76,13,0), FM(ELECTRIC,ARP,82,8,10), FM(SUB_BASS,BASS,82,2,0), FM(BELL,LEAD,58,13,9)}, {0,A_8THS,B_DRONE,A_BELLS}}
};
#undef FM
#undef PL
#undef DR
static FM1_DSP_DATA const char fm_names[FM1_FM_FACTORY_PROGRAMS][11] = {
    "NEON KEYS ","ORBIT BELL","SOFT ORGAN","ROUND BASS","WARM AIR  ","WOOD REED ",
    "SOFT EP   ","DRY CLAV  ","FM GUITAR ","MARIMBA   ","VIBES     ","SOFT BRASS",
    "FM STRINGS","SUB BASS  ","METAL HIT ","SOFT LEAD "
};
/* Three independently voiced operator pairs: body, colour and transient.
 * Unequal carrier levels avoid three identical full-level voices stacking. */
/* Pack recipe fields without changing the serialized patch layout. */
struct fm_pair { uint8_t level, coarse : 2, detune : 4, mod, ratio, fine, decay, body; };
_Static_assert(sizeof(struct part) == 4 && sizeof(struct fm_pair) == 7, "compact recipes");
struct fm_voice {
    uint8_t attack, decay, settle, release, body, sustain, mod_attack, mod_release, feedback;
    struct fm_pair pair[3];
};
static FM1_DSP_DATA const struct fm_voice voices[FM1_FM_FACTORY_PROGRAMS] = {
    {96,52,30,62,84,0,99,66,0, {{98,1,7,54,1,0,65,38},{91,1,8,60,1,0,57,58},{79,1,6,48,7,0,80,0}}},
    {99,50,28,53,70,0,99,62,0, {{96,1,7,58,3,50,59,0},{82,2,7,48,7,0,68,0},{69,3,7,35,11,0,80,0}}},
    {95,65,45,78,99,99,95,78,0, {{90,1,7,0,1,0,65,0},{87,2,7,0,1,0,65,0},{74,3,7,0,1,0,65,0}}},
    {97,72,46,82,91,84,99,86,0, {{99,1,7,42,1,0,76,32},{74,2,7,32,1,0,82,20},{0,1,7,0,1,0,80,0}}},
    {53,47,36,52,96,92,49,55,0, {{90,1,5,42,1,0,55,82},{84,1,9,34,2,0,53,76},{73,2,7,27,1,0,52,78}}},
    {84,61,46,72,94,89,78,77,1, {{95,1,7,61,2,0,63,86},{82,1,8,38,1,0,68,75},{70,3,7,29,1,0,72,60}}},
    {94,49,28,60,86,0,96,64,0, {{98,1,7,44,1,0,60,35},{86,1,8,38,3,0,66,20},{72,2,6,31,1,0,72,0}}},
    {99,75,47,80,42,0,99,83,1, {{96,1,7,67,1,0,82,18},{81,2,7,55,2,0,86,0},{0,1,7,0,1,0,80,0}}},
    {97,57,32,66,69,0,99,74,0, {{97,1,7,55,1,0,72,18},{83,2,7,46,3,0,81,0},{66,3,7,38,5,0,85,0}}},
    {99,67,37,72,34,0,99,81,0, {{98,1,7,60,3,0,85,0},{74,2,7,46,5,0,88,0},{58,3,7,32,7,0,91,0}}},
    {98,52,25,55,78,0,99,65,0, {{94,1,7,56,4,0,76,0},{76,2,8,39,3,0,81,0},{66,3,6,28,2,0,84,0}}},
    {81,62,42,72,96,90,67,75,1, {{95,1,7,67,1,0,62,90},{84,1,8,46,1,0,65,83},{70,2,6,40,2,0,68,75}}},
    {61,48,39,54,96,94,57,58,0, {{89,1,5,54,1,0,55,89},{83,1,9,36,2,0,55,83},{72,2,7,30,4,0,58,76}}},
    {95,72,44,84,96,94,99,86,0, {{99,1,7,0,1,0,80,0},{0,1,7,0,1,0,80,0},{0,1,7,0,1,0,80,0}}},
    {99,65,40,72,48,0,99,76,0, {{89,1,7,65,7,31,67,0},{70,2,7,58,11,47,73,0},{56,3,7,48,13,79,78,0}}},
    {88,60,40,71,96,90,82,75,1, {{96,1,7,45,1,0,65,83},{80,1,8,31,2,0,70,72},{71,2,6,24,1,0,74,66}}}
};
static void demo_voice(uint8_t p[156], unsigned tone)
{
    const struct fm_voice *sound = &voices[tone];
    memset(p, 0, 156);
    memcpy(p + 145, fm_names[tone], 10);
    for (unsigned op = 0; op < 6; ++op) {
        uint8_t *v = p + 21 * op;
        const struct fm_pair *pair = &sound->pair[op / 2];
        unsigned carrier = op & 1U;
        v[0] = carrier ? sound->attack : sound->mod_attack;
        v[1] = carrier ? sound->decay : pair->decay;
        v[2] = sound->settle; v[3] = carrier ? sound->release : sound->mod_release;
        v[4] = 99; v[5] = carrier ? sound->body : pair->body;
        v[6] = carrier ? sound->sustain : sound->sustain ? pair->body : 0;
        v[8] = 50; v[10] = carrier ? 8 : 18; v[13] = 1;
        v[15] = carrier ? 2 : 4;
        v[16] = carrier ? pair->level : pair->mod;
        v[18] = carrier ? pair->coarse : pair->ratio;
        v[19] = carrier ? 0 : pair->fine; v[20] = pair->detune;
        if (tone == ORGAN) {
            static FM1_DSP_DATA const uint8_t levels[6] = {89,77,84,68,59,52};
            static FM1_DSP_DATA const uint8_t ratios[6] = {1,2,3,4,6,8};
            v[16] = levels[op]; v[18] = ratios[op]; v[19] = 0;
            v[5] = v[6] = 99; v[15] = 1; v[20] = 7;
        }
    }
    for (unsigned i = 126; i < 130; ++i) p[i] = 99;
    for (unsigned i = 130; i < 134; ++i) p[i] = 50;
    p[134] = tone == ORGAN ? 31 : 4; p[135] = sound->feedback;
    p[136] = 1; p[137] = 28; p[138] = 35; p[141] = 1; p[142] = 4;
    if (tone == WARM_PAD || tone == STRINGS || tone == REED) { p[139] = 2; p[143] = 2; }
    p[144] = 24; p[155] = 63;
}
static void preset(struct groove_preset *p, const struct part *v)
{
    p->type = v->source; p->volume = v->level; p->modified = 1;
    p->controls.envelope[2] = 100;
    if (v->source == GROOVE_DX7) {
        demo_voice(p->data, v->tone); p->program = v->tone; p->modified = 0;
    }
    else if (v->source == GROOVE_303) {
        static const uint8_t bass[4][BASS303_NPARAMS] = {
            {45,24,32,52,62,0,64,96,0,1,14,5},
            {60,66,73,56,74,0,64,94,6,1,26,10},
            {65,44,58,39,65,0,64,92,0,1,14,8},
            {34,18,20,62,42,1,64,100,0,1,20,5}
        };
        memcpy(p->data, bass[v->tone], BASS303_NPARAMS);
        p->program = p->data[BASS303_WAVE]; p->controls.mono = 1;
    } else if (v->source == GROOVE_808) {
        /* Public D8_PRESET_BYTES format: selectors, then two parameter sets.
         * Sound selectors must match each set for a lossless store/load. */
        unsigned at = D8_NUM;
        for (unsigned variant = 0; variant < 2; ++variant)
            for (unsigned part = 0; part <= D8_NUM; ++part)
                for (int i = 0; i < drum808_nparams((int)part); ++i) {
                    const fm1_param_t *d = drum808_param((int)part, i);
                    static const uint8_t levels[D8_NUM] = {70,52,46,42,38,36,46,28,25,30,29};
                    static const uint8_t pans[D8_NUM] = {64,64,49,64,79,54,72,80,47,77,72};
                    unsigned value = d->def;
                    if (!strcmp(d->name, "Sound")) value = variant;
                    if (part < D8_NUM) {
                        if (!strcmp(d->name, "Level")) value = levels[part];
                        else if (!strcmp(d->name, "Pan")) value = pans[part];
                        else if (!strcmp(d->name, "Rev")) value = part == D8_SD || part == D8_CP ? 24 : 9;
                        else if (!strcmp(d->name, "Dly")) value = part == D8_RS ? 12 : 0;
                        else if (!strcmp(d->name, "Decay") && part == D8_SD) value = 84;
                    }
                    p->data[at++] = (uint8_t)value;
                }
    } else if (v->source == GROOVE_SAMPLE) {
        p->sampler_pads = SAMPLE_DRUM_PADS;
        for (unsigned pad = 0; pad < SAMPLE_DRUM_PADS; ++pad) {
            uint8_t *pot = p->data + pad * SAMPLE_DRUM_PARAMS;
            for (unsigned i = 0; i < SAMPLE_DRUM_PARAMS; ++i) pot[i] = sampledrum_param(i)->def;
            static const uint8_t levels[SAMPLE_DRUM_PADS] = {105,83,75,83,72,73,58,62};
            pot[SAMPLE_DRUM_LEVEL] = levels[pad];
            pot[SAMPLE_DRUM_VELOCITY_LPF] = pad == 0 ? 12 : 30;
            if (pad == 6 || pad == 7) pot[SAMPLE_DRUM_CHOKE] = 1;
        }
    } else if (v->source == GROOVE_PLUCK) {
        static const uint8_t pluck[3][4] = {{88,70,24,48},{103,52,24,66},{79,89,24,42}};
        memcpy(p->data, pluck[v->tone], 4);
    } else {
        p->program = p->data[0] = v->tone; p->data[1] = 16;
        p->data[2] = 24; p->data[3] = v->role == BASS ? 20 : 38;
    }
    p->data[209] = v->sends;
}

/* ---- Worship patterns -------------------------------------------------
 * Arp/lead degrees index the chord-tone table below; -1 is a rest.
 * Shapes favour root/5th/9th/3rd voicings, which read as "worship" rather
 * than the plain 1-3-5 triad climb that tends to sound cheesy. */
static const int8_t arp_patterns[8][16] = {
    [A_8THS]    = {0,-1,1,-1,3,-1,4,-1, 2,-1,1,-1,3,-1,4,-1},
    [A_ROLL16]  = {0,1,3,4, 2,1,3,4, 0,1,3,4, 5,4,3,1},
    [A_DOTTED]  = {0,-1,-1,3,-1,-1,4,-1, -1,1,-1,-1,3,-1,2,-1},
    [A_BELLS]   = {3,-1,-1,-1,4,-1,-1,-1, 5,-1,-1,-1,4,-1,-1,-1},
    [A_UPDOWN]  = {0,-1,1,-1,2,-1,3,-1, 4,-1,3,-1,2,-1,1,-1},
    [A_SPARSE]  = {6,-1,-1,-1,-1,-1,4,-1, -1,-1,3,-1,-1,-1,-1,-1},
    [A_PULSE16] = {2,2,2,2, 2,2,2,2, 2,2,2,2, 2,2,2,2},
    [A_PAIRS]   = {3,-1,4,-1,3,-1,1,-1, 3,-1,4,-1,5,-1,4,-1},
};
static const uint8_t arp_gates[8] = {10, 7, 13, 15, 10, 15, 5, 9};
/* Bass: 0 root, 1 fifth, 2 octave, -1 rest. */
static const int8_t bass_patterns[4][16] = {
    [B_DRONE]   = {0,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1},
    [B_PULSE8]  = {0,-1,0,-1,0,-1,0,-1, 0,-1,0,-1,0,-1,0,-1},
    [B_QUARTER] = {0,-1,-1,-1,0,-1,-1,-1, 0,-1,-1,-1,1,-1,-1,-1},
    [B_HEART]   = {0,-1,-1,0,-1,-1,-1,-1, 0,-1,-1,0,-1,-1,-1,-1},
};
/* 808 kick (bit 0) and closed hat (bit 10) masks, read from the first 16th. */
static const uint16_t beat_kick[3] = {[D_HEART] = 0x0909, [D_FLOOR] = 0x1111, [D_HALF] = 0x0401};
static const uint16_t beat_hat[3]  = {[D_HEART] = 0x0000, [D_FLOOR] = 0x4444, [D_HALF] = 0x1111};
/* Small fixed velocity offsets keep repeated notes from sounding machine-gunned
 * while staying identical every loop, so the groove never wanders. */
static const int8_t human[16] = {0,-6,3,-9,2,-4,5,-8,1,-7,4,-5,0,-3,6,-10};

enum { MAJ, MIN, SUS2, SUS4, QUALITIES };
static const uint8_t chord_tones[QUALITIES][7] = {
    [MAJ]  = {0, 7, 12, 14, 16, 19, 24},
    [MIN]  = {0, 7, 12, 14, 15, 19, 24},
    [SUS2] = {0, 7, 12, 14, 19, 26, 24},
    [SUS4] = {0, 7, 12, 14, 17, 19, 24},
};
/* Open pad voicings stored in custom chord slots 13..16. */
static const uint8_t pad_chords[QUALITIES][8] = {
    {4, 7, 14, 16}, {4, 7, 14, 15}, {4, 7, 14, 19}, {4, 7, 12, 17}
};
enum { PAD_CHORD_SLOT = 12 };

static unsigned clamp_velocity(int v) { return v < 24 ? 24U : v > 120 ? 120U : (unsigned)v; }
static unsigned accent(unsigned i) { return i % 4 == 0 ? 100U : i % 2 == 0 ? 86U : 72U; }
/* Keep every layer in a comfortable register whatever the key. */
static unsigned register_base(unsigned low, unsigned root) { return low + (root + 12U - low % 12U) % 12U; }

static void layer_cells(const struct scene *s, unsigned layer, unsigned root, unsigned quality,
                        struct groove_step cells[GROOVE_STEPS])
{
    const struct part *v = &s->part[layer];
    unsigned pattern = s->pattern[layer];
    memset(cells, 0, sizeof(struct groove_step) * GROOVE_STEPS);
    for (unsigned i = 0; i < 16; ++i) {
        struct groove_step n = {0, 0, 0};
        unsigned bit = 1U << i;
        if (v->role == BEAT) {
            if (beat_kick[pattern] & bit) n.drums |= 1U;
            if (beat_hat[pattern] & bit) n.drums |= 1U << 10;
            if (n.drums) n.velocity = (uint8_t)clamp_velocity(((n.drums & 1U) ? 96 : 46) + human[i] / 2);
        } else if (v->role == PAD) {
            if (i == 0) {
                n.note = (uint8_t)register_base(43, root);
                n.velocity = 92;
                n.drums = (uint16_t)((PAD_CHORD_SLOT + quality + 1U) | (15U << 5) | (1U << 12));
            }
        } else if (v->role == BASS) {
            int d = bass_patterns[pattern][i];
            if (d >= 0) {
                static const uint8_t offset[3] = {0, 7, 12};
                unsigned drone = pattern == B_DRONE;
                n.note = (uint8_t)(register_base(40, root) + offset[d]);
                n.velocity = (uint8_t)clamp_velocity(drone ? 96 : (int)accent(i) + human[i]);
                n.drums = (uint16_t)(((drone ? 15U : pattern == B_PULSE8 ? 6U : 11U) << 5) | (drone << 12));
            }
        } else { /* ARP and LEAD */
            int d = arp_patterns[pattern][i];
            if (d >= 0) {
                unsigned base = register_base(v->role == LEAD ? 67 : 55, root);
                n.note = (uint8_t)(base + chord_tones[quality][d]);
                int vel = (int)accent(i) + human[i];
                if (v->role == LEAD) vel -= 14;
                n.velocity = (uint8_t)clamp_velocity(vel);
                n.drums = (uint16_t)(arp_gates[pattern] << 5);
            }
        }
        for (unsigned bar = 0; bar < 4; ++bar) cells[bar * 16 + i] = n;
    }
}

const char *groove_factory_name(unsigned index)
{
    return index < GROOVE_FACTORY_PROJECTS ? scenes[index].name : NULL;
}
int groove_factory_build(unsigned index, struct groove_project *p)
{
    if (!p || index >= GROOVE_FACTORY_PROJECTS) return GROOVE_PROJECT_INVALID;
    const struct scene *s = &scenes[index];
    memset(p, 0, sizeof *p);
    memset(p->name, ' ', sizeof p->name); memcpy(p->name, s->name, strlen(s->name));
    p->tempo = s->bpm; p->mute = (uint8_t)(255U << LAYERS);
    p->effects[0] = s->decay;
    p->effects[1] = GROOVE_DELAY_SYNC + 7U; /* tempo-synced dotted eighth */
    p->effects[2] = s->feedback; p->effects[3] = s->fx_level;
    groove_sequence_defaults(&p->sequence);
    memcpy(p->sequence.chord[PAD_CHORD_SLOT], pad_chords, sizeof pad_chords);
    groove_project_arp_defaults(p);
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        const struct part empty = {GROOVE_PSG, LEAD, 0, 100, 0};
        const struct part *v = t < LAYERS ? &s->part[t] : &empty;
        preset(&p->preset[t], v);
        if (t >= LAYERS) continue;
        p->preset[t].sequence_pages = 1; /* one-bar loop: chord changes land fast */
        layer_cells(s, t, s->root, MAJ, p->step[t]);
    }
    return groove_project_valid(p) ? 0 : GROOVE_PROJECT_INVALID;
}

/* ---- Chord follow -----------------------------------------------------
 * Active whenever the loaded project carries a worship scene name. Holding
 * keys sets the chord: the lowest key is the root, a minor/major third
 * decides the quality, otherwise 2nd = sus2 and 4th = sus4. A single key
 * changes only the root. Releases never change the chord, so it latches. */
static struct {
    int8_t scene;
    uint8_t root, quality;
    char label[20];
} follow = {-1, 0, MAJ, ""};

static int active_scene(void)
{
    char name[13];
    groove_project_name(name);
    for (unsigned i = 0; i < GROOVE_FACTORY_PROJECTS; ++i) {
        char padded[12];
        memset(padded, ' ', sizeof padded);
        memcpy(padded, scenes[i].name, strlen(scenes[i].name));
        if (!memcmp(name, padded, 12)) return (int)i;
    }
    return -1;
}
const char *worship_chord_label(void) { return follow.label; }
int worship_keys(uint32_t keys, uint32_t pressed, int octave)
{
    int scene = active_scene();
    if (scene < 0) { follow.scene = -1; return -1; }
    const struct scene *s = &scenes[scene];
    if (follow.scene != scene) { follow.scene = (int8_t)scene; follow.root = s->root; follow.quality = MAJ; }
    /* Only a new key press re-reads the chord: lifting fingers one at a time
     * must never turn A-C-E into C-E (C major) on the way out. */
    if (!(keys & pressed)) return 0;
    unsigned lowest = 128, mask = 0;
    for (unsigned key = 0; key < 27; ++key)
        if (keys & (1U << key)) {
            unsigned note = groove_key_note(0, key, octave);
            if (note < lowest) lowest = note;
        }
    if (lowest > 127) return 0;
    for (unsigned key = 0; key < 27; ++key)
        if (keys & (1U << key)) {
            unsigned note = groove_key_note(0, key, octave);
            if (note < 128) mask |= 1U << ((note - lowest) % 12U);
        }
    unsigned root = lowest % 12U, quality = follow.quality;
    if (mask & (1U << 4)) quality = MAJ;
    else if (mask & (1U << 3)) quality = MIN;
    else if (mask & (1U << 2)) quality = SUS2;
    else if (mask & (1U << 5)) quality = SUS4;
    if (root == follow.root && quality == follow.quality) return 0;
    struct groove_step cells[GROOVE_STEPS];
    for (unsigned t = 0; t < LAYERS; ++t) {
        if (s->part[t].role == BEAT) continue;
        layer_cells(s, t, root, quality, cells);
        groove_sequence_replace(t, 0, cells, GROOVE_STEPS);
    }
    follow.root = (uint8_t)root; follow.quality = (uint8_t)quality;
    static const char *const roots[12] = {"C","C#","D","Eb","E","F","F#","G","Ab","A","Bb","B"};
    static const char *const kinds[QUALITIES] = {"MAJ", "MIN", "SUS2", "SUS4"};
    char *out = follow.label;
    strcpy(out, "CHORD  "); strcat(out, roots[root]); strcat(out, " "); strcat(out, kinds[quality]);
    return 1;
}
