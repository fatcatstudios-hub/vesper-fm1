/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* VESPER 2: the app's half of the fm1-base contract. VESPER's own code (vesper/: the
 * screens, scenes, LUSH engine, sequencer) is the 1.x firmware's, unchanged; app.c is
 * the adapter that gives it the FM1 Open SDK calls it was written against, on fm1-base. */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "VESPER"
#define APP_BLOCK 256u                   /* 5.8 ms a half: VESPER renders it as two of its 128-frame blocks */
#define APP_SPLASH                       /* VESPER draws its own start-up screen */
void app_splash(void);
#define APP_GAIN(m) 4096u                /* MASTER is VESPER's own (smoothed in the engine) */
#define APP_BIG_FONT FONT_B               /* the platform's own screens: the smaller face (20 KB of flash for VESPER) */
#define APP_OVERLOAD                     /* the render near its limit: fewer synth voices for a while */
void app_overload(void);
#define APP_SYSEX                        /* project transfers (vesper/app/transfer.c) */
int app_sysex(const uint8_t *p, uint32_t n);

/* the store: only the platform's panel calibration; VESPER keeps its projects in the data area itself */
enum { OBJ_PANEL, OBJ_NOBJ };
#define APP_STORE_MAP {0xDC000u}

void app_init(void);
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);
void app_panic(void);
void app_ui_init(void);
void app_frame(void);
void app_input_only(void);
void app_say(const char *a, const char *b);
