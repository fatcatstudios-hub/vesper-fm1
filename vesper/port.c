/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* VESPER 2: the FM1 Open SDK calls VESPER was written against, on fm1-base's plat.h.
 * The same job as web/engine/Simulator.c does for the browser, for the FM-1 itself.
 *
 *  - panel: plat_buttons / plat_keys / plat_enc / plat_master in the SDK's order
 *  - screen: 16-row bands sent with DMA (app.c's vesper_lcd_*)
 *  - flash: the 1.x user partition, mapped into fm1-base's data area; the sample
 *    bank part reads VESPER's own bank, built into the firmware (web/synth/worship_bank.inc)
 *  - MIDI: USB and the TRS jack in, USB out
 *  - the engine: web/engine (the open FM1 engine) and LUSH, as in 1.x */
#include "fm_patch.h"
#include "internal.h"
#include "fm1_device.h"
#include "fm1_engine_control.h"
#include "fm1_audio_io.h"
#include "audio_output.h"
#include "plat.h"
#include "lush.h"
#include <string.h>

/* app.c */
int vesper_lcd_pending(void);
void vesper_lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px);
void vesper_lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c);
/* the sample bank, built into the firmware (web/synth/worship_bank.inc) */
extern const uint8_t worship_bank[335672];
#define vesper_bank worship_bank
#define vesper_bank_bytes ((uint32_t)sizeof worship_bank)
/* screen.c, sound.c */
extern int groove_screen_open(unsigned target);

/* ---- engine gate: the engine's edits hold it; a render that finds it held skips the
 * engine for that block (LUSH and the effects go on) ---- */
static volatile unsigned gate;
void fm1_web_pause(void) { ++gate; }
void fm1_web_resume(void) { if (gate) --gate; }

/* ---- stats ---- */
static volatile uint32_t blocks;
void fm1_engine_stats_read(struct fm1_engine_stats *stats)
{
    if (!stats) return;
    memset(stats, 0, sizeof *stats);
    stats->submitted = stats->completed = blocks;
    stats->underruns = plat_xruns();
}
void fm1_audio_stats_read(struct fm1_audio_stats *stats)
{
    if (!stats) return;
    memset(stats, 0, sizeof *stats);
    stats->blocks = blocks;
}
void fm1_control_request(void) {}
uint32_t fm1_millis(void) { return plat_ms(); }

/* ---- panel ---- */
/* SDK button order: OCT-, OCT+, FX, SEL, ENV, LFO, EDIT, GLO, HOME, SAVE, ARP, SEQ, PLAY, REC */
static uint32_t sdk_buttons(uint32_t b)
{
    uint32_t r = (b & ((1u << B_OCTDN) - 1u)) << 2;       /* FX..REC: SDK 2..13 */
    if (b & (1u << B_OCTDN)) r |= 1u;
    if (b & (1u << B_OCTUP)) r |= 2u;
    return r;
}
static uint32_t base_buttons(uint32_t s)
{
    uint32_t r = (s >> 2) & ((1u << B_OCTDN) - 1u);
    if (s & 1u) r |= 1u << B_OCTDN;
    if (s & 2u) r |= 1u << B_OCTUP;
    return r;
}
/* SDK encoders: SELECT, ALGO, K1, K2, K3, K4, PRESET */
static const uint8_t enc_role[7] = {EN_SELECT, EN_ALGO, EN_K1, EN_K2, EN_K3, EN_K4, EN_PRESET};
static int16_t enc_sum[7];
static uint32_t led_buttons, led_keys, led_shown = ~0u, led_keys_shown = ~0u;

static void poll_encoders(void)
{
    for (unsigned i = 0; i < 7; ++i) {
        int32_t d = plat_enc(enc_role[i]), v = enc_sum[i] + d;
        enc_sum[i] = (int16_t)(v > 127 ? 127 : v < -127 ? -127 : v);
    }
}
void fm1_controls_init(void) { memset(enc_sum, 0, sizeof enc_sum); }
void fm1_controls_read(struct fm1_controls *c)
{
    poll_encoders();
    c->keys = plat_keys() & ((1u << 27) - 1u);
    c->buttons = sdk_buttons(plat_buttons());
    memcpy(c->encoders, enc_sum, sizeof enc_sum);
    memset(enc_sum, 0, sizeof enc_sum);
    c->knob = (uint16_t)(plat_master() >> 2 > 1023u ? 1023u : plat_master() >> 2);
    int b = plat_battery();                               /* 0..3, 4 on USB power, -1 unknown */
    static const uint16_t adc[5] = {650, 730, 810, 880, 900};
    c->battery = b < 0 ? 900 : adc[b > 4 ? 4 : b];
}
void fm1_led_set(unsigned key, int on)
{
    if (key < 14) {
        uint32_t bit = 1u << key;
        led_buttons = on ? led_buttons | bit : led_buttons & ~bit;
    } else if (key < 41) {
        uint32_t bit = 1u << (key - 14);
        led_keys = on ? led_keys | bit : led_keys & ~bit;
    }
}
static void leds_out(void)
{
    if (led_buttons == led_shown && led_keys == led_keys_shown) return;
    led_shown = led_buttons; led_keys_shown = led_keys;
    plat_leds(base_buttons(led_buttons), led_keys);
}

/* ---- screen ---- */
void fm1_screen_init(void) { vesper_lcd_fill(0, 0, 240, 240, 0); }
void fm1_screen_fill(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t c)
{
    if (x > 240 || y > 240 || w > 240 - x || h > 240 - y || !w || !h) return;
    while (vesper_lcd_pending()) {}
    vesper_lcd_fill(x, y, w, h, c);
}
static uint16_t blit_band[FM1_SCREEN_DMA_PIXELS] VESPER_BIG;
int fm1_screen_blit(unsigned x, unsigned y, unsigned w, unsigned h, const uint16_t *src, size_t count)
{
    if (!src || x > 240 || y > 240 || w > 240 - x || h > 240 - y || count < (size_t)w * h) return -1;
    unsigned rows = w ? FM1_SCREEN_DMA_PIXELS / w : 0;
    for (unsigned r = 0; r < h && rows; r += rows) {
        unsigned n = h - r < rows ? h - r : rows;
        while (vesper_lcd_pending()) {}
        for (unsigned i = 0; i < n * w; ++i) {
            uint16_t p = src[r * w + i];
            blit_band[i] = (uint16_t)((p << 8) | (p >> 8));
        }
        vesper_lcd_blit(x, y + r, w, n, blit_band);
    }
    return 0;
}
int fm1_screen_pending(void) { return vesper_lcd_pending(); }
int fm1_screen_submit(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t *px, size_t count)
{
    if (!px || !w || !h || w > 240U || h > 240U || x > 240U - w || y > 240U - h ||
        w * h > FM1_SCREEN_DMA_PIXELS || count < (size_t)w * h) return -1;
    if (vesper_lcd_pending()) return 1;
    for (unsigned i = 0; i < w * h; ++i) px[i] = (uint16_t)((px[i] << 8) | (px[i] >> 8));
    vesper_lcd_blit(x, y, w, h, px);
    return 0;
}

/* ---- Bluetooth: not in this build yet ---- */
static uint8_t radio_features = FM1_BT_AUDIO | FM1_BT_MIDI, radio_role;
int fm1_bluetooth_configure(unsigned features)
{
    if (features & ~(FM1_BT_AUDIO | FM1_BT_MIDI)) return -1;
    radio_features = (uint8_t)features; return 0;
}
void fm1_bluetooth_get_status(struct fm1_bluetooth_status *s)
{ if (s) { memset(s, 0, sizeof *s); s->features = radio_features; } }
int fm1_ble_set_role(enum fm1_ble_role role)
{
    if (role != FM1_BLE_PERIPHERAL && role != FM1_BLE_CENTRAL) return -1;
    radio_role = (uint8_t)role; return 0;
}
int fm1_ble_scan(void) { return -3; }
int fm1_ble_connect(unsigned index) { (void)index; return -3; }
int fm1_ble_disconnect(void) { return -3; }
int fm1_ble_forget_bonds(void) { return -3; }
void fm1_ble_get_status(struct fm1_ble_status *s) { if (s) { memset(s, 0, sizeof *s); s->role = radio_role; } }
int fm1_ble_get_device(unsigned index, struct fm1_ble_device *d) { (void)index; if (d) memset(d, 0, sizeof *d); return -1; }

/* ---- flash: 1.x's user partition (GROOVE_FLASH_BYTES), mapped:
 *   [0, GROOVE_SAMPLE_OFFSET)                 projects, settings, journal -> data area 0x97000
 *   [GROOVE_SAMPLE_OFFSET, GROOVE_LIBRARY_OFFSET)  the sample bank: VESPER's, in the firmware (read only)
 *   [GROOVE_LIBRARY_OFFSET, GROOVE_FLASH_BYTES)    DX7 banks, tuning, session -> data area after the projects */
enum { PROJ_PHYS = 0x97000u, TAIL_PHYS = PROJ_PHYS + GROOVE_SAMPLE_OFFSET,
       TAIL_BYTES = GROOVE_FLASH_BYTES - GROOVE_LIBRARY_OFFSET };
_Static_assert(GROOVE_SAMPLE_OFFSET % 4096 == 0 && GROOVE_LIBRARY_OFFSET % 4096 == 0, "sector aligned");
_Static_assert(TAIL_PHYS + TAIL_BYTES <= 0xDC000u, "below the panel calibration");
static int phys(uint32_t at, size_t n, uint32_t *p)
{
    if (at + n < at || at + n > GROOVE_FLASH_BYTES) return 0;
    if (at + n <= GROOVE_SAMPLE_OFFSET) { *p = PROJ_PHYS + at; return 1; }
    if (at >= GROOVE_LIBRARY_OFFSET) { *p = TAIL_PHYS + (at - GROOVE_LIBRARY_OFFSET); return 1; }
    return 0;
}
static int in_bank(uint32_t at, size_t n) { return at >= GROOVE_SAMPLE_OFFSET && at + n <= GROOVE_LIBRARY_OFFSET && at + n >= at; }
uint32_t fm1_flash_size(void) { return GROOVE_FLASH_BYTES; }
int fm1_flash_read(uint32_t at, void *data, size_t n)
{
    uint32_t p;
    if (!data) return FM1_FLASH_INVALID;
    if (phys(at, n, &p)) {
        const uint8_t *s = plat_data(p);
        if (!s) return FM1_FLASH_INVALID;
        memcpy(data, s, n); return 0;
    }
    if (in_bank(at, n)) {
        uint32_t o = at - GROOVE_SAMPLE_OFFSET;
        uint8_t *d = data;
        for (size_t i = 0; i < n; ++i, ++o) d[i] = o < vesper_bank_bytes ? vesper_bank[o] : 0xFF;
        return 0;
    }
    return FM1_FLASH_INVALID;
}
int fm1_flash_write(uint32_t at, const void *data, size_t n)
{
    uint32_t p;
    if (!data || !phys(at, n, &p)) return FM1_FLASH_INVALID;   /* the bank is part of the firmware */
    const uint8_t *now = plat_data(p), *src = data;
    if (!now) return FM1_FLASH_INVALID;
    for (size_t i = 0; i < n; ++i) if ((now[i] & src[i]) != src[i]) return FM1_FLASH_NEEDS_ERASE;
    if (plat_data_write(p, data, (uint32_t)n)) return FM1_FLASH_IO;
    return memcmp(plat_data(p), data, n) ? FM1_FLASH_IO : 0;
}
int fm1_flash_erase(uint32_t at)
{
    uint32_t p;
    if (at % FM1_FLASH_SECTOR_BYTES || !phys(at, FM1_FLASH_SECTOR_BYTES, &p)) return FM1_FLASH_INVALID;
    return plat_data_erase(p) ? FM1_FLASH_IO : 0;
}
const uint8_t *fm1_flash_map(uint32_t at, size_t n)
{
    uint32_t p;
    if (phys(at, n, &p)) return plat_data(p);
    if (in_bank(at, n) && at - GROOVE_SAMPLE_OFFSET + n <= vesper_bank_bytes) return vesper_bank + (at - GROOVE_SAMPLE_OFFSET);
    return NULL;
}
int fm1_flash_invalidate(uint32_t at, size_t n) { uint32_t p; return phys(at, n, &p) || in_bank(at, n) ? 0 : FM1_FLASH_INVALID; }

/* ---- MIDI: in from USB and TRS; out to USB ---- */
enum { MIDI_QUEUE = 64 };
static struct { uint8_t data[3], count; } midi[MIDI_QUEUE];
static volatile unsigned midi_read, midi_write;
static unsigned midi_source = FM1_MIDI_USB;
unsigned fm1_midi_output_ports(void) { return 1u << FM1_MIDI_USB; }
unsigned fm1_midi_input_ports(void) { return (1u << FM1_MIDI_USB) | (1u << FM1_MIDI_DIN); }
unsigned fm1_midi_ready_ports(void) { return 1u << FM1_MIDI_USB; }
unsigned fm1_midi_input_source(void) { return midi_source; }
static unsigned cin_of(unsigned status, size_t n)
{
    if (status < 0xF0) return status >> 4;
    if (status >= 0xF8) return 0xF;
    return n == 3 ? 0x3 : n == 2 ? 0x2 : 0x5;
}
int fm1_midi_send_port(unsigned port, const uint8_t *data, size_t n)
{
    if (port != FM1_MIDI_USB || !data || !n || n > 128) return -1;
    if (data[0] == 0xF0) { plat_sysex_send(data, (uint32_t)n); return 0; }
    if (n > 3) return -1;
    if (plat_midi_out_room(TO_USB) < 1) return -2;
    uint32_t pkt = cin_of(data[0], n) | (uint32_t)data[0] << 8;
    if (n > 1) pkt |= (uint32_t)data[1] << 16;
    if (n > 2) pkt |= (uint32_t)data[2] << 24;
    plat_midi_send(pkt, TO_USB);
    return 0;
}
int fm1_midi_send(const uint8_t *data, size_t n) { return fm1_midi_send_port(FM1_MIDI_USB, data, n); }
/* the engine's own MIDI (played keys, MIDI from outside routed to tracks): queued for the render */
int fm1_midi_post(const uint8_t *data, size_t n)
{
    if (!data || !n || n > 3 || data[0] < 128) return -1;
    unsigned next = (midi_write + 1) % MIDI_QUEUE;
    if (next == midi_read) return -1;
    memcpy(midi[midi_write].data, data, n); midi[midi_write].count = (uint8_t)n;
    midi_write = next; return 0;
}
static unsigned msg_len(unsigned status)
{
    if (status < 0xF0) return (status & 0xE0) == 0xC0 ? 2 : 3;
    if (status == 0xF1 || status == 0xF3) return 2;
    if (status == 0xF2) return 3;
    return 1;
}
static void midi_poll(void)
{
    uint32_t pkt;
    int port;
    for (unsigned k = 0; k < 32 && plat_midi_in(&pkt, &port); ++k) {
        uint8_t m[3] = {(uint8_t)(pkt >> 8), (uint8_t)(pkt >> 16), (uint8_t)(pkt >> 24)};
        if (m[0] < 0x80 || m[0] == 0xF0 || m[0] == 0xF7) continue;   /* SysEx comes by app_sysex */
        unsigned src = port == PORT_TRS ? FM1_MIDI_DIN : FM1_MIDI_USB;
        midi_source = src;
        fm1_app_midi_received(src, m, msg_len(m[0]));
    }
}
int vesper_sysex(const uint8_t *p, uint32_t n)
{
    static uint8_t frame[1026];
    if (n + 2 > sizeof frame) return 0;
    frame[0] = 0xF0; memcpy(frame + 1, p, n); frame[n + 1] = 0xF7;
    midi_source = FM1_MIDI_USB;
    fm1_app_sysex(frame, n + 2);
    return 1;
}

/* ---- audio: USB and Bluetooth audio are fm1-base's (mixed after VESPER) ---- */
int fm1_usb_audio_read(int32_t *pcm, unsigned frames, uint16_t gains[2])
{
    if (!pcm || !gains || frames != FM1_BLOCK_FRAMES) return -1;
    gains[0] = gains[1] = 0; return 0;
}
int fm1_bluetooth_audio_read(int16_t *pcm, unsigned frames) { return pcm && frames == FM1_BLOCK_FRAMES ? 0 : -1; }
int fm1_usb_audio_write(const int32_t *pcm, unsigned frames) { return pcm && frames == FM1_BLOCK_FRAMES ? 0 : -1; }

void vesper_render(int32_t *lr)
{
    while (midi_read != midi_write) {
        fm1_web_midi(midi[midi_read].data, midi[midi_read].count);
        midi_read = (midi_read + 1) % MIDI_QUEUE;
    }
    if (gate) memset(lr, 0, FM1_BLOCK_WORDS * sizeof *lr);
    else (void)fm1_engine_render(lr, FM1_BLOCK_FRAMES);
    fm1_output_mix(lr);
    ++blocks;
}
void vesper_silence(void)
{
    const uint8_t reset = 255;
    fm1_web_midi(&reset, 1);
    fm1_output_reset();
}

/* ---- control: the 1.x control task, about 100 times a second ---- */
static uint32_t last_control;
void vesper_load_track(void);
static void control(void)
{
    vesper_load_track();
    midi_poll();
    fm1_web_control();
    leds_out();
    last_control = plat_ms();
}
void vesper_boot(void)
{
    fm1_output_reset();
    fm1_web_init();                  /* the engine, samples, projects, MIDI, and the start-up screen */
    while (groove_samples_status() == 1) groove_samples_poll();
    control();
}
void vesper_ui(void) { groove_screen_init(); control(); }
void vesper_control(void) { control(); }
void vesper_input(void)
{
    poll_encoders();
    midi_poll();
    if (plat_ms() - last_control >= 10u) control();
}

/* 1.x's panel adapter hooks (weak in midi_router.c): the screen code owns the keys here */
void groove_midi_panel_reset(void) {}
void groove_midi_panel_track_reset(unsigned track) { (void)track; }

static volatile uint32_t shed_until;   /* vesper_overload */
/* TEST builds: the audio load's peak over the last second, and the dropouts (xruns) since start */
static uint32_t load_peak, load_since;
unsigned vesper_load_peak(void) { return load_peak; }
unsigned vesper_dropouts(void) { return plat_xruns(); }
void vesper_load_track(void)
{
    if (shed_until && (int32_t)(plat_ms() - shed_until) > 0) { shed_until = 0; lush_budget(10); }
    uint32_t now = plat_ms(), pct = plat_cpu_pct();
    if (now - load_since > 1000u) { load_peak = 0; load_since = now; }
    if (pct > load_peak) load_peak = pct;
}

/* the render near its limit (fm1-base APP_OVERLOAD, from the audio interrupt): LUSH keeps fewer synth voices
 * (the oldest released first) for two seconds, then its usual ten */
void vesper_overload(void) { lush_budget(6); shed_until = plat_ms() + 2000u; }
