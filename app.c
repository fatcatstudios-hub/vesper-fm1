/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* VESPER 2 on fm1-base: the platform contract. Part of fm1-base's unity build, after the
 * platform, so the LCD's own functions are at hand here; everything else VESPER needs is in
 * vesper/port.c (the FM1 Open SDK calls, on plat.h), compiled apart with VESPER's code. */
#include "app.h"

/* vesper/port.c */
void vesper_boot(void);
void vesper_ui(void);
void vesper_control(void);
void vesper_input(void);
void vesper_render(int32_t *lr);
void vesper_silence(void);
void vesper_overload(void);
int vesper_sysex(const uint8_t *p, uint32_t n);

static volatile uint8_t ready;

/* ---- the screen: VESPER sends 16-row bands with DMA and polls for the end (1.x's
 * fm1_screen_submit / fm1_screen_pending); lcd_blit leaves its transfer running ---- */
int vesper_lcd_pending(void)
{
    /* VESPER renders the next band while the last one is sent; when it asks, the transfer is finished
     * (lcd_sync waits for its end: only the BSP touches the LCD's registers) */
    lcd_sync();
    return 0;
}
void vesper_lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px) { lcd_blit(x, y, w, h, px); }
void vesper_lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c) { lcd_fill(x, y, w, h, c); }

void app_splash(void) { lcd_fill(0, 0, 240, 240, 0); }   /* black: VESPER's own start-up follows */

void app_init(void) {}                  /* the engine starts with the screen (app_ui_init) */
void app_ui_init(void)
{
    if (!ready) {
        vesper_boot();                  /* engine, scenes, projects, then the start-up screen */
        ready = 1;
    } else
        vesper_ui();                    /* back from an update session or a restore: draw again */
}
void app_frame(void) { if (ready) vesper_control(); }
void app_input_only(void) { if (ready) vesper_input(); }
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    (void)master_q12;
    if (!ready || n != APP_BLOCK) {
        for (uint32_t i = 0; i < 2u * n; i++) lr[i] = 0;
        return;
    }
    for (uint32_t k = 0; k < n; k += 128u)
        vesper_render(lr + 2u * k);
}
void app_overload(void) { if (ready) vesper_overload(); }
void app_panic(void) { if (ready) vesper_silence(); }
void app_say(const char *a, const char *b) { (void)a; (void)b; }
int app_sysex(const uint8_t *p, uint32_t n) { return ready ? vesper_sysex(p, n) : 0; }
