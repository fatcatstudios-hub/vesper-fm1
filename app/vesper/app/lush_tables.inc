/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Generated lookup tables for the LUSH engine. */
static FM1_AUDIO_DATA const uint32_t lush_octave[12] = {815363807,863847862,915214929,969636441,1027294024,1088380105,1153098554,1221665363,1294309365,1371273005,1452813141,1539201906};
/* Envelope coefficient per 16-sample tick, indexed by time index / 2 (1 ms .. 30 s).
 * The first 16 are stored; lush_tables_build() derives the rest (tau grows 1.1763x
 * per step, c = 1 - e^(-1/tau) by series; within 0.3% of the original table). */
static const uint32_t lush_env_head[16] = {871397541,813902097,752343235,688666921,624704093,562036849,501928491,445306161,392780535,344687452,301139246,262077117,227319227,196601797,169612328,146015184};
static uint32_t lush_env[64];
/* Filter coefficient per semitone of cutoff (MIDI note): 2 sin(pi fc / fs), Q15,
 * capped for stability; built from lush_octave (within 11 cents of the stored table above 60 Hz). */
static uint16_t lush_cutoff[136];
/* Quarter sine wave, 0..90 degrees of a 256-step cycle; sine15() mirrors it. */
static FM1_AUDIO_DATA const int16_t lush_quarter[65] = {0,804,1608,2410,3212,4011,4808,5602,6393,7179,7962,8739,9512,10278,11039,11793,12539,13279,14010,14732,15446,16151,16846,17530,18204,18868,19519,20159,20787,21403,22005,22594,23170,23731,24279,24811,25329,25832,26319,26790,27245,27683,28105,28510,28898,29268,29621,29956,30273,30571,30852,31113,31356,31580,31785,31971,32137,32285,32412,32521,32609,32678,32728,32757,32767};
