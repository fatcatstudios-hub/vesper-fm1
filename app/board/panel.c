/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Copyright (c) 2026 珠海笙科智能科技有限公司 (M-VAVE).
 * Connect the FM1 control API to the board scanner. */
#include "fm1.h"
#include "fm1_device.h"
#include "fm1_input.h"
#include "fm1_lcd_hw.h"
#include "lcd.inc"

static uint8_t controls_started;
uint8_t fm1_io_trs_midi_enabled(void) { return 1U; }

void fm1_controls_init(void)
{
    if (!controls_started) {
        /* PB1: battery divider; PB6: master potentiometer. */
        const uint32_t analog = (1U << 1) | (1U << 6);
        FM1_PR(FM1_PB, FM1_DIR) |= analog;
        FM1_PR(FM1_PB, FM1_DIE) &= ~analog;
        FM1_PR(FM1_PB, FM1_PU) &= ~analog;
        FM1_PR(FM1_PB, FM1_PD) &= ~analog;
        fm1_input_init();
        fm1_device_start(fm1_input_tick);
        controls_started = 1U;
    }
}

void fm1_controls_read(struct fm1_controls *state)
{
    if (!state) return;
    struct fm1_controls snapshot;
    uint32_t irq_state = fm1_device_irq_save();
    snapshot.keys = fm1_in.notes;
    snapshot.buttons = fm1_in.buttons;
    unsigned index = FM1_NENC;
    while (index) {
        --index;
        snapshot.encoders[index] = fm1_in.enc_steps[index];
        fm1_in.enc_steps[index] = 0;
    }
    fm1_device_irq_restore(irq_state);
    snapshot.knob = (uint16_t)fm1_device_adc(4U);
    snapshot.battery = (uint16_t)fm1_device_adc(3U);
    *state = snapshot;
}

/* Button test: the panel matrix has 88 contacts and the firmware maps 55.
 * Report the first unmapped contact that differs from power-up (column << 4 |
 * row), or 0xff, so a hidden switch (a SELECT push) can be located. */
unsigned fm1_panel_probe(void)
{
    static uint8_t base[FM1_NCOL], known[FM1_NCOL], ready;
    if (!ready) {
        for (unsigned k = 0; k < FM1_NKEY; ++k) known[fm1_key_pin[k] >> 4] |= (uint8_t)(1U << (fm1_key_pin[k] & 15U));
        for (unsigned e = 0; e < FM1_NENC; ++e)
            for (unsigned i = 0; i < 2; ++i) known[fm1_encoder_pin[e][i] >> 4] |= (uint8_t)(1U << (fm1_encoder_pin[e][i] & 15U));
        for (unsigned c = 0; c < FM1_NCOL; ++c) base[c] = fm1_in.rows[c];
        ready = 1;
    }
    for (unsigned c = 0; c < FM1_NCOL; ++c) {
        unsigned diff = (unsigned)(fm1_in.rows[c] ^ base[c]) & ~(unsigned)known[c] & 0xffU;
        for (unsigned r = 0; diff; ++r, diff >>= 1)
            if (diff & 1U) return c << 4 | r;
    }
    return 0xffU;
}

void fm1_led_set(unsigned key, int on)
{
    fm1_led_key(key, on);
}
