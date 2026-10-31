/* SPDX-License-Identifier: MIT
 * Fixed SEA5 dequantization table: Copyright (c) 2025 Dani Biro.
 * Stored compactly for the LUSH sample voice: every row is
 *   v0, v1, v1 + d, v1 + 2d, ... , v1 + 13d, v15   and each value appears as +v, -v.
 * lush_dqt_build() expands it once into RAM; verified identical, all 512 entries, to
 * web/synth/sample_metadata.inc. See THIRD_PARTY.md for SEA and MIT terms. */
static const int16_t lush_dqt_rows[16][4] = {
    {1, 3, 2, 31},
    {4, 13, 10, 155},
    {11, 35, 28, 434},
    {21, 70, 56, 868},
    {36, 120, 96, 1488},
    {56, 188, 150, 2325},
    {81, 270, 216, 3348},
    {113, 375, 300, 4650},
    {149, 498, 398, 6169},
    {193, 643, 514, 7967},
    {242, 808, 646, 10013},
    {299, 995, 796, 12338},
    {362, 1208, 966, 14973},
    {434, 1445, 1156, 17918},
    {512, 1705, 1364, 21142},
    {598, 1993, 1594, 24707}
};
static int16_t lush_dqt[512];
static void lush_dqt_build(void)
{
    if (lush_dqt[0]) return;
    for (unsigned r = 0; r < 16; ++r)
        for (unsigned q = 0; q < 16; ++q) {
            const int16_t *p = lush_dqt_rows[r];
            int16_t value = q == 0 ? p[0] : q == 15 ? p[3] : (int16_t)(p[1] + (q - 1) * p[2]);
            lush_dqt[r * 32 + 2 * q] = value;
            lush_dqt[r * 32 + 2 * q + 1] = (int16_t)-value;
        }
}
