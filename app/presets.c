/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "instrument.h"
#include "fm_patch.h"
#include "drum808.h"
#include "sampledrum.h"
#include "lush.h"
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
    uint8_t lush[LAYERS]; /* LUSH patch per layer; LUSH_NONE keeps the FM1 engine */
    uint8_t shimmer;      /* octave-up shimmer fed back into the reverb, 0..127 */
};
/* sends: reverb 0..15 + 16 * delay 0..15. LUSH layers park a silent PSG in
 * the engine; the LUSH engine plays their notes. */
#define LU(role, level, rev, dly) {GROOVE_PSG, role, 0, level, rev + 16*dly}
#define DR(level) {GROOVE_808, BEAT, 0, level, 0}
/* Pattern ids: ARP/LEAD use the sequence bank, BASS the bass bank, BEAT the
 * beat bank; PAD ignores it (sustained open chord). */
enum { S_DOTTED8, S_ROLL3, S_STAIRS, S_OCTAVES, S_GALLOP, S_SYNC, S_DOTTED_RISE, S_PULSE16,
       S_8THS, S_BELLS, S_SPARSE, S_PAIRS, S_CASCADE, S_UPDOWN, S_SWELL, SEQUENCES };
enum { B_DRONE, B_PULSE8, B_PULSE16, B_SYNC, B_HEART, B_QUARTER, B_DOTTED, BASSES };
enum { D_HEART, D_FLOOR, D_HALF };
#define GLS_ LUSH_GLASS_PLUCK
#define WBS_ LUSH_WARM_BASS
#define SUB_ LUSH_SUB
#define DPL_ LUSH_DETUNE_PLUCK
#define SAW_ LUSH_SUPERSAW
#define JUN_ LUSH_JUNO_ARP
#define DBS_ LUSH_DETUNE_BASS
#define PNO_ LUSH_PIANO
#define MAR_ LUSH_MARIMBA
#define GLK_ LUSH_GLOCK
#define HAL_ LUSH_HALO_PLUCK
#define HPN_ LUSH_HANDPAN
#define KAL_ LUSH_KALIMBA
#define VOO_ LUSH_VOX_OOH
#define VEE_ LUSH_VOX_EE
#define MGT_ LUSH_MUTED_GTR
#define RVP_ LUSH_REV_PIANO
#define RVG_ LUSH_REV_GLOCK
#define FM1_ LUSH_NONE
/* Sequence-first scenes: two interlocking sequences (mid ARP + high LEAD), a
 * sequenced bass, and a beat. Only four scenes keep a (quiet) pad. Loops of
 * different lengths (16/32/48 steps) stay locked to the bar but keep evolving.
 * Delay sends are high; the dual delay does the rest. */
/* No pads, no drones: every layer moves. Hybrid voices (synth or sample plus
 * a pitched-breath halo), tempo-locked filter motion and shimmer do the
 * atmosphere that pads used to. */
#if WORSHIP_DIAG
/* Cost probe: each slot plays one sound alone (16ths at 90 BPM, all notes, typical
 * sends) so the emulator's idle figure gives that sound's CPU cost. Slot 16 holds
 * the three basses together. Tracks 2-4 are muted except in slot 16. */
#define T(name, patch) {name, 90, 4, 100,58,92, {LU(ARP,100,8,12), LU(BASS,100,1,0), LU(BASS,100,1,0), LU(BASS,100,1,0)}, \
                        {S_CASCADE,B_PULSE16,B_PULSE16,B_PULSE16}, {patch,FM1_,FM1_,FM1_}, 15}
static const struct scene scenes[GROOVE_FACTORY_PROJECTS] = {
    T("T GLASS", GLS_), T("T DPLUCK", DPL_), T("T SUPERSAW", SAW_), T("T JUNO", JUN_),
    T("T PIANO", PNO_), T("T MARIMBA", MAR_), T("T GLOCK", GLK_), T("T HALO", HAL_),
    T("T HANDPAN", HPN_), T("T KALIMBA", KAL_), T("T VOX OOH", VOO_), T("T VOX EE", VEE_),
    T("T MUTEDGTR", MGT_), T("T REVPIANO", RVP_), T("T REVGLOCK", RVG_),
    {"T BASSES", 90, 4, 100,58,92, {LU(BASS,100,1,0), LU(BASS,100,1,0), LU(BASS,100,1,0), LU(BASS,100,1,0)},
     {B_PULSE16,B_PULSE16,B_PULSE16,B_PULSE16}, {WBS_,DBS_,SUB_,FM1_}, 15},
};
#undef T
#else
static const struct scene scenes[GROOVE_FACTORY_PROJECTS] = {
    {"DOTTED 8TH",  72, 4,104,64,96, {LU(ARP,124,7,13), LU(LEAD,86,10,12), LU(BASS,98,1,2), LU(LEAD,74,13,10)},  {S_8THS,S_ROLL3,B_PULSE8,S_SWELL},         {MGT_,HAL_,DBS_,RVG_}, 15},
    {"ROLLING 3S",  76, 7,104,60,94, {LU(ARP,104,7,12), LU(LEAD,84,11,12), LU(BASS,96,1,0), DR(66)},              {S_ROLL3,S_SPARSE,B_HEART,D_HEART},       {DPL_,HPN_,WBS_,FM1_}, 20},
    {"STAIRCASE",   80, 2, 98,58,92, {LU(ARP,96,6,11), LU(LEAD,90,9,13), LU(BASS,100,1,1), DR(68)},               {S_STAIRS,S_DOTTED8,B_SYNC,D_HALF},       {SAW_,MAR_,DBS_,FM1_}, 10},
    {"OCTAVE RUN",  84, 9, 96,56,90, {LU(ARP,104,6,10), LU(LEAD,86,8,12), LU(BASS,100,1,0), DR(70)},              {S_OCTAVES,S_SYNC,B_PULSE16,D_FLOOR},     {JUN_,GLS_,DBS_,FM1_}, 10},
    {"GALLOP",      88,11, 96,58,90, {LU(ARP,104,6,11), LU(LEAD,80,11,13), LU(BASS,100,1,0), DR(70)},             {S_GALLOP,S_SPARSE,B_PULSE8,D_FLOOR},     {DPL_,VEE_,DBS_,FM1_}, 15},
    {"MORNING",     70, 0,106,62,94, {LU(ARP,116,8,12), LU(LEAD,102,10,13), LU(BASS,96,1,0), LU(LEAD,76,13,11)},   {S_8THS,S_DOTTED_RISE,B_DOTTED,S_DOTTED8},{KAL_,MAR_,WBS_,VOO_}, 20},
    {"SANCTUARY",   66, 5,110,60,96, {LU(ARP,118,10,12), LU(LEAD,108,11,13), LU(BASS,100,1,0), LU(ARP,116,9,12)},    {S_ROLL3,S_DOTTED_RISE,B_HEART,S_SWELL},  {HAL_,GLK_,WBS_,RVP_}, 30},
    {"HEARTBEAT",   74, 4,102,60,92, {LU(ARP,116,7,11), LU(LEAD,94,9,13), LU(BASS,100,1,0), DR(70)},              {S_PAIRS,S_DOTTED8,B_HEART,D_HEART},      {PNO_,DPL_,WBS_,FM1_}, 15},
    {"GLORY BUILD", 86, 7, 94,54,88, {LU(ARP,86,5,10), LU(LEAD,86,8,12), LU(BASS,102,1,0), DR(72)},               {S_PULSE16,S_ROLL3,B_PULSE16,D_FLOOR},    {SAW_,JUN_,DBS_,FM1_}, 10},
    {"CASCADE",     76, 0,104,62,94, {LU(ARP,116,8,12), LU(LEAD,92,10,13), LU(BASS,96,1,0), LU(LEAD,86,12,12)},   {S_CASCADE,S_DOTTED8,B_QUARTER,S_SPARSE}, {MAR_,HAL_,SUB_,HPN_}, 20},
    {"ANTHEM 8THS", 90, 9, 94,52,88, {LU(ARP,102,5,10), LU(LEAD,76,9,12), LU(BASS,102,1,0), DR(72)},              {S_PULSE16,S_DOTTED8,B_PULSE8,D_FLOOR},   {DPL_,SAW_,DBS_,FM1_}, 10},
    {"QUIET PLACE", 60,10,110,60,96, {LU(ARP,102,13,11), LU(ARP,122,11,12), LU(BASS,104,1,0), LU(LEAD,112,12,13)},    {S_DOTTED8,S_SPARSE,B_QUARTER,S_SWELL},{HPN_,PNO_,SUB_,RVP_}, 30},
    {"SYNC LINES",  78, 2,100,60,92, {LU(ARP,104,6,12), LU(LEAD,84,9,12), LU(BASS,100,1,1), DR(68)},              {S_SYNC,S_ROLL3,B_SYNC,D_HALF},           {JUN_,KAL_,DBS_,FM1_}, 15},
    {"SPIRIT MOVE", 82,11,100,60,92, {LU(ARP,126,7,12), LU(LEAD,92,10,13), LU(BASS,100,1,0), DR(68)},             {S_DOTTED_RISE,S_SPARSE,B_DOTTED,D_HEART},{MGT_,SAW_,WBS_,FM1_}, 15},
    {"ALTAR CALL",  62, 4,110,60,96, {LU(ARP,116,10,12), LU(ARP,116,10,12), LU(BASS,100,1,0), LU(LEAD,104,13,13)},    {S_SYNC,S_UPDOWN,B_DOTTED,S_SPARSE},      {HAL_,PNO_,WBS_,VOO_}, 30},
    {"NEON SKY",    92, 8, 94,56,90, {LU(ARP,84,6,11), LU(LEAD,72,8,12), LU(BASS,102,1,0), DR(72)},              {S_OCTAVES,S_CASCADE,B_PULSE16,D_FLOOR},  {SAW_,JUN_,DBS_,FM1_}, 10}
};
#endif
#undef LU
#undef DR
#if WORSHIP_DX7_RECIPES /* unused by the worship scenes; off to save program Flash */
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
#endif
static void preset(struct groove_preset *p, const struct part *v)
{
    p->type = v->source; p->volume = v->level; p->modified = 1;
    p->controls.envelope[2] = 100;
#if WORSHIP_DX7_RECIPES
    if (v->source == GROOVE_DX7) {
        demo_voice(p->data, v->tone); p->program = v->tone; p->modified = 0;
    }
    else
#endif
    if (v->source == GROOVE_303) {
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

/* ---- Worship sequences -----------------------------------------------
 * Degrees index the chord-tone table; a step is 4 bits: 0 rest, else degree + 1.
 * A sequence is 16, 32 or 48 sixteenths long, so layers with different lengths
 * drift against each other and realign every few bars without leaving the grid.
 * Shapes favour root/5th/9th/10th over plain triad climbs. */
enum { MAX_SEQ = 48 };
struct sequence { uint8_t length, gate, step[MAX_SEQ / 2]; };
static int sequence_degree(const struct sequence *q, unsigned at)
{
    return (int)((q->step[at >> 1] >> ((at & 1U) * 4U)) & 15U) - 1;
}
static const struct sequence sequences[SEQUENCES] = {
    [S_DOTTED8] = {16, 12, {1,64,0,5,32,0,4,3}},
    [S_ROLL3] = {48, 7, {33,20,82,33,100,69,33,20,82,33,100,69,33,20,82,33,100,69,33,20,82,83,134,86}},
    [S_STAIRS] = {32, 8, {33,84,67,86,103,69,35,36,33,84,83,134,103,53,36,3}},
    [S_OCTAVES] = {16, 6, {49,49,98,98,132,132,149,53}},
    [S_GALLOP] = {16, 7, {1,67,2,83,1,67,6,53}},
    [S_SYNC] = {32, 10, {1,32,0,4,5,64,0,2,3,32,0,4,6,5,4,0}},
    [S_DOTTED_RISE] = {48, 13, {1,64,0,5,32,0,6,16,0,4,80,0,2,96,0,1,64,0,5,32,0,6,112,0}},
    [S_PULSE16] = {16, 5, {51,51,51,115,51,51,51,116}},
    [S_8THS] = {16, 10, {1,2,4,5,3,2,4,5}},
    [S_BELLS] = {32, 15, {4,0,5,0,6,0,5,0,4,0,5,0,8,0,6,0}},
    [S_SPARSE] = {32, 15, {7,0,0,5,0,4,0,0,8,0,0,6,0,9,0,0}},
    [S_PAIRS] = {16, 9, {4,5,4,2,4,5,6,5}},
    [S_CASCADE] = {16, 6, {137,103,69,35,120,86,52,18}},
    [S_UPDOWN] = {16, 10, {1,2,3,4,5,4,3,2}},
    [S_SWELL] = {16, 15, {0,0,0,0,4,0,0,0}},
};
/* Bass degrees: 0 root, 1 fifth, 2 octave. */
static const struct sequence basses[BASSES] = {
    [B_DRONE] = {16, 15, {1,0,0,0,0,0,0,0}},
    [B_PULSE8] = {16, 6, {1,1,1,1,1,1,1,1}},
    [B_PULSE16] = {16, 4, {17,19,17,19,17,19,49,18}},
    [B_SYNC] = {32, 6, {1,16,0,1,3,16,0,2,1,16,0,1,3,2,1,0}},
    [B_HEART] = {16, 11, {1,16,0,0,1,16,0,0}},
    [B_QUARTER] = {16, 11, {1,0,1,0,1,0,2,0}},
    [B_DOTTED] = {16, 8, {1,16,0,1,16,0,3,2}},
};
/* 808 kick (bit 0) and closed hat (bit 10) masks, read from the first 16th. */
static const uint16_t beat_kick[3] = {[D_HEART] = 0x0909, [D_FLOOR] = 0x1111, [D_HALF] = 0x0401};
static const uint16_t beat_hat[3]  = {[D_HEART] = 0x0000, [D_FLOOR] = 0x4444, [D_HALF] = 0x1111};
/* Small fixed velocity offsets keep repeated notes from sounding machine-gunned
 * while staying identical every loop, so the groove never wanders. */
static const int8_t human[16] = {0,-6,3,-9,2,-4,5,-8,1,-7,4,-5,0,-3,6,-10};

enum { MAJ, MIN, SUS2, SUS4, QUALITIES };
/* Degrees: R, 5, 8va, 9, 10th(3rd), 12th(5), 15ma, 16th(9), 17th(3rd). */
static const uint8_t chord_tones[QUALITIES][9] = {
    [MAJ]  = {0, 7, 12, 14, 16, 19, 24, 26, 28},
    [MIN]  = {0, 7, 12, 14, 15, 19, 24, 26, 27},
    [SUS2] = {0, 7, 12, 14, 14, 19, 24, 26, 26},
    [SUS4] = {0, 7, 12, 14, 17, 19, 24, 26, 29},
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

/* Loop length of a layer in sixteenths (16, 32 or 48). */
static unsigned layer_length(const struct scene *s, unsigned layer)
{
    const struct part *v = &s->part[layer];
    if (v->role == BASS) return basses[s->pattern[layer]].length;
    if (v->role == ARP || v->role == LEAD) return sequences[s->pattern[layer]].length;
    return 16;
}
static void layer_cells(const struct scene *s, unsigned layer, unsigned root, unsigned quality,
                        struct groove_step cells[GROOVE_STEPS])
{
    const struct part *v = &s->part[layer];
    unsigned pattern = s->pattern[layer], length = layer_length(s, layer);
    memset(cells, 0, sizeof(struct groove_step) * GROOVE_STEPS);
    for (unsigned i = 0; i < GROOVE_STEPS; ++i) {
        struct groove_step n = {0, 0, 0};
        unsigned at = i % length, beat = at % 16, bit = 1U << beat;
        if (v->role == BEAT) {
            if (beat_kick[pattern] & bit) n.drums |= 1U;
            if (beat_hat[pattern] & bit) n.drums |= 1U << 10;
            if (n.drums) n.velocity = (uint8_t)clamp_velocity(((n.drums & 1U) ? 96 : 46) + human[beat] / 2);
        } else if (v->role == PAD) {
            if (beat == 0) {
                n.note = (uint8_t)register_base(48, root);
                n.velocity = 92;
                n.drums = (uint16_t)((PAD_CHORD_SLOT + quality + 1U) | (15U << 5) | (1U << 12));
            }
        } else if (v->role == BASS) {
            const struct sequence *q = &basses[pattern];
            int d = sequence_degree(q, at);
            if (d >= 0) {
                static const uint8_t offset[3] = {0, 7, 12};
                unsigned drone = pattern == B_DRONE;
                n.note = (uint8_t)(register_base(40, root) + offset[d]);
                n.velocity = (uint8_t)clamp_velocity(drone ? 96 : (int)accent(beat) + human[beat]);
                n.drums = (uint16_t)((q->gate << 5) | (drone << 12));
            }
        } else { /* ARP and LEAD sequences */
            const struct sequence *q = &sequences[pattern];
            int d = sequence_degree(q, at);
            if (d >= 0) {
                unsigned base = register_base(v->role == LEAD ? 60 : 48, root); /* low-mid: warm, not music box */
                n.note = (uint8_t)(base + chord_tones[quality][d]);
                int vel = (int)accent(beat) + human[beat];
                if (v->role == LEAD) vel -= 10;
                n.velocity = (uint8_t)clamp_velocity(vel);
                n.drums = (uint16_t)(q->gate << 5);
            }
        }
        cells[i] = n;
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
#if WORSHIP_DIAG
    p->mute |= index < 15 ? 0x0EU : 0x08U;
#endif
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
        p->preset[t].sequence_pages = (uint8_t)(layer_length(s, t) / 16U); /* 1/2/3 = 16/32/48 */
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

/* Control-task poll: keep the LUSH engine in step with the loaded scene, the
 * track volumes and sends, and the FX page (decay, delay time, feedback, level). */
void worship_poll(void)
{
    int scene = active_scene();
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        unsigned patch = scene >= 0 && t < LAYERS ? scenes[scene].lush[t] : LUSH_NONE;
        if (patch == LUSH_NONE) { lush_layer(t, LUSH_NONE, 0, 0, 0); continue; }
        struct groove_view view;
        uint8_t sends[2];
        groove_get_view(t, 0, &view);
        groove_sends_get(t, sends);
        lush_layer(t, patch, view.volume, sends[0], sends[1]);
    }
    uint8_t fx[4];
    struct groove_view view;
    groove_get_view(0, 0, &view);
    groove_effects_get(fx);
    lush_effects(fx, groove_delay_ticks(), view.tempo, scene >= 0 ? scenes[scene].shimmer : 0);
}
