// The side panels (panel.h). The picture is 480 wide in the middle of 1024: 272 px are free on each side.
// panel_midi() runs in the USB task and only records what is on; a 30 ms LVGL timer (the LVGL task, display lock
// held by it) recolours the keys that changed, so no lock is taken from the USB task and nothing is held for long.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "panel.h"
#include "display.h"
#include "lvgl.h"
#include "esp_heap_caps.h"

#define SIDE_W      272
#define RIGHT_X     (DISP_W - SIDE_W)

#define NKEYS       37                      // the SMK-37's keys: three octaves and a C
#define WHITE_W     12
#define WHITE_H     110
#define BLACK_W     8
#define BLACK_H     66
#define PIANO_X     (RIGHT_X + 4)           // 22 white keys x 12 = 264
#define PIANO_Y     80
#define BASE_DEF    48                      // C3: the lowest key at octave 0 (sloop-fm1 SMK37.md, DAW mode)

#define NPADS       16
#define PAD_SZ      30
#define PAD_GAP     3
#define PADS_X      (RIGHT_X + 5)           // 8 x 30 + 7 x 3 = 261
#define PADS_Y      270

#define CHORD_Y     198                     // between the piano and the pads
#define LAST_Y      344
#define ROLL_X      PIANO_X                 // the note history: under each key, the newest at the top
#define ROLL_Y      396
#define ROLL_W      (22 * WHITE_W)
#define ROLL_H      192                     // rows, one each 30 ms tick: 5.8 s
#define ROLL_BAR    6                       // a note's width

#define DRUM_CH     9                       // MIDI channel 10: the pads, until the keyboard's status names the drum channel

#define BAR_X       SIDE_W                  // the status bar: the 60 px strip over the picture
#define BAR_Y       18

// What a lit key or pad shows: 1-4 the track's colour (SLOOP's TE_COL, sloop-fm1 firmware/src/ui_input.c PAL),
// 5 a channel no track has, 6 no status yet / DAW mode
enum { LIT_OFF, LIT_T1, LIT_T2, LIT_T3, LIT_T4, LIT_OTHER, LIT_PLAIN };
static const uint32_t LIT_COL[] = {0, 0x287CFF, 0x1ECC70, 0xFFC618, 0xFF621A, 0x9A9A9A, 0xFF8000};
// mirror_proto.h MIRROR_STATUS_LEN: flags track low bpm(2) ch[4] dawch | head plen steps(2) page dial[4] fader[4]
// fbank master batt | scale(2) root bend mod (the parts after the first | from a version 3 firmware)
#define ST_LEN      31
#define ST_STRIP    26                      // the strip's part is there from this length on
#define ST_MIN      10

#define DIM_TICKS   (5 * 60 * 1000 / 30)    // 5 minutes of 30 ms ticks with nothing played: the backlight dims
#define DIM_LEVEL   30                      // of 255

#define STRIP_X     SIDE_W                  // the strip under the picture: 480 x 60
#define STRIP_Y     540
#define STRIP_BAR_Y 6
#define STRIP_BAR_H 30
#define STRIP_CAP_Y 40
#define NBARS       11                      // dials 1-4, faders 1-4, the volume, the pitch wheel, the mod wheel
#define BAR_PITCH   9                       // (drawn from the middle: 64 is at rest)
#define COL_WHITE   0xC8C8C8
#define COL_BLACK   0x181818
#define COL_PAD     0x2A2A2A
#define COL_DIM     0x909090

// SLOOP's drum lanes as MIDI notes (sloop-fm1 firmware/src/drums.c LANE_NOTE): a pad in DRUM KIT plays lane i.
// Drawn with lanes 0-7 on the bottom row and 8-15 on the top row (the keyboard's pads 9-16 / 1-8).
static const uint8_t LANE_NOTE[NPADS] = {36, 35, 38, 39, 42, 46, 44, 37, 40, 43, 48, 49, 51, 70, 63, 56};

static lv_obj_t *key_obj[NKEYS], *pad_obj[NPADS], *lbl_range, *lbl_last;
static lv_obj_t *bar_mode, *bar_track, *bar_bpm, *bar_run;
static lv_obj_t *lbl_chord, *pad_name[NPADS], *roll_img;
static uint16_t *roll_pix;                          // ROLL_W x ROLL_H RGB565, PSRAM
static lv_image_dsc_t roll_dsc;
static uint16_t roll_col[sizeof LIT_COL / sizeof LIT_COL[0]];
static uint16_t key_x[NKEYS];                       // a key's bar in the history, from ROLL_X
static int roll_busy;                               // rows that still hold a note
static uint16_t chord_mask = 0xFFFF;                // the pitch classes last named (0xFFFF: none yet)
static uint8_t chord_bass = 0xFF;
static uint8_t key_black[NKEYS];
static uint8_t key_shown[NKEYS], pad_shown[NPADS];  // what is drawn (LVGL task)

// written by the USB task, read by the timer: single bytes, so a torn read is at worst one frame late
static volatile uint8_t note_on[128], pad_on[NPADS];
static volatile uint8_t base = BASE_DEF, last[3], last_new;
static volatile uint8_t stat[ST_LEN], stat_have, stat_new;   // the keyboard's 'S' record
static volatile uint8_t stat_len;                   // how much of it the keyboard sent
static volatile uint8_t act;                        // something was played or changed since the last tick
static volatile unsigned link_fps, link_lost;
static volatile uint8_t link_new;
static lv_obj_t *key_dot[NKEYS], *lbl_link, *lbl_scale;
static uint8_t dot_shown[NKEYS];                    // 0 hidden, 1 in the scale, 2 its root
static int idle_ticks;
static bool dimmed;
static lv_obj_t *strip, *step_obj[16], *bar_fill[NBARS], *bat_obj[3], *cap_steps, *cap_fad;
static uint32_t step_col[16];
static int bar_h[NBARS];
static uint8_t base_shown = 0xFF;

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t col)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(col), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *text(lv_obj_t *parent, int x, int y, int w, const lv_font_t *font, uint32_t col, const char *s)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_set_style_text_line_space(l, 2, 0);
    lv_label_set_text(l, s);
    return l;
}

static void note_name(char *dst, size_t n, int note)
{
    static const char *const NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    snprintf(dst, n, "%s%d", NAME[note % 12], note / 12 - 1);
}

// The chord of the pitch classes in mask (bit 0 = C), bass the lowest note's class: "C maj7", "A min / C"; "" when
// fewer than three classes or none of the shapes below fits. The bass is tried first as the root (C6 before Am7).
static void chord_name(char *dst, size_t n, uint16_t mask, int bass)
{
    static const struct { uint16_t shape; const char *name; } CH[] = {
        {0x091, "maj"}, {0x089, "min"}, {0x049, "dim"}, {0x111, "aug"}, {0x085, "sus2"}, {0x0A1, "sus4"},
        {0x491, "7"}, {0x891, "maj7"}, {0x489, "min7"}, {0x889, "min maj7"}, {0x449, "min7 b5"}, {0x249, "dim7"},
        {0x291, "6"}, {0x289, "min6"}, {0x095, "add9"}, {0x08D, "min add9"}, {0x495, "9"}, {0x895, "maj9"},
        {0x48D, "min9"}, {0x4A1, "7 sus4"},
    };
    static const char *const NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    dst[0] = 0;
    if (__builtin_popcount(mask) < 3)
        return;
    for (int k = 0; k < 12; k++) {
        int root = (bass + k) % 12;
        if (!((mask >> root) & 1))
            continue;
        uint16_t rel = (uint16_t)(((mask >> root) | (mask << (12 - root))) & 0xFFF);
        for (size_t i = 0; i < sizeof CH / sizeof CH[0]; i++)
            if (CH[i].shape == rel) {
                if (root == bass) snprintf(dst, n, "%s %s", NAME[root], CH[i].name);
                else snprintf(dst, n, "%s %s / %s", NAME[root], CH[i].name, NAME[bass]);
                return;
            }
    }
}

// The strip under the picture, from the status record's second part. LVGL task.
static void strip_update(bool have)
{
    static const char *const BANK[4] = {"FAD LVL", "FAD FLT", "FAD REV", "FAD DLY"};
    char s[24];
    if (!have) {
        lv_obj_add_flag(strip, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_HIDDEN);
    uint8_t trk = stat[1] & 3, head = stat[10], plen = stat[11], page = stat[14];
    unsigned mask = stat[12] | (unsigned)stat[13] << 8;
    for (int i = 0; i < 16; i++) {
        uint32_t c = i >= plen ? 0x101010 : i == head ? 0xFFFFFF : ((mask >> i) & 1) ? LIT_COL[LIT_T1 + trk] : 0x383838;
        if (c != step_col[i]) {
            step_col[i] = c;
            lv_obj_set_style_bg_color(step_obj[i], lv_color_hex(c), 0);
        }
    }
    snprintf(s, sizeof s, "STEPS %d - %d", page * 16 + 1, page * 16 + 16);
    lv_label_set_text(cap_steps, s);
    lv_label_set_text(cap_fad, BANK[stat[23] & 3]);
    for (int i = 0; i < NBARS; i++) {
        uint8_t v = i < 4 ? stat[15 + i] : i < 8 ? stat[19 + (i - 4)] : i == 8 ? stat[24] :
                    stat_len >= ST_LEN ? stat[29 + (i - 9)] : 0xFF;
        // h: 1..30 from the bottom (0 still shows a line), -1 no value here. The pitch wheel: from the middle,
        // up or down; kept as 1000 + its value so a change is seen.
        int h = v > 127 ? -1 : i == BAR_PITCH ? 1000 + v : 1 + v * (STRIP_BAR_H - 1) / 127;
        if (h == bar_h[i])
            continue;
        bar_h[i] = h;
        if (h < 0) {
            lv_obj_add_flag(bar_fill[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(bar_fill[i], LV_OBJ_FLAG_HIDDEN);
        if (i == BAR_PITCH) {
            int mid = STRIP_BAR_H / 2, d = ((int)v - 64) * mid / 64;
            lv_obj_set_height(bar_fill[i], d > 0 ? d + 1 : -d + 1);
            lv_obj_set_y(bar_fill[i], d > 0 ? mid - d : mid);
        } else {
            lv_obj_set_height(bar_fill[i], h);
            lv_obj_set_y(bar_fill[i], STRIP_BAR_H - h);
        }
    }
    for (int i = 0; i < 3; i++)
        lv_obj_set_style_bg_color(bat_obj[i], lv_color_hex(i < (stat[25] & 3) ? 0x30E060 : 0x303030), 0);
}

// The scale on the piano: a dot on every key in the selected track's scale, the root's in red. LVGL task.
static void scale_update(bool have)
{
    static const char *const NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    unsigned mask = stat[26] | (unsigned)stat[27] << 8;
    char s[24] = "";
    // what the dots mean, beside "KEYS": the root and how many notes; all twelve: no dots, and it says why
    if (have && mask == 0xFFF)
        snprintf(s, sizeof s, "scale: all notes");
    else if (have && mask)
        snprintf(s, sizeof s, "scale: %s, %d notes", NAME[stat[28] % 12], __builtin_popcount(mask));
    lv_label_set_text(lbl_scale, s);
    if (!have || mask == 0xFFF)                     // (every note: nothing to tell)
        mask = 0;
    for (int i = 0; i < NKEYS; i++) {
        int pc = i % 12;                            // (the piano starts on a C)
        uint8_t want = !((mask >> pc) & 1) ? 0 : pc == stat[28] ? 2 : 1;
        if (want == dot_shown[i])
            continue;
        dot_shown[i] = want;
        if (!want) {
            lv_obj_add_flag(key_dot[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(key_dot[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(key_dot[i], lv_color_hex(want == 2 ? 0xFF2020 : key_black[i] ? 0xB0B0B0 : 0x505050), 0);
        }
    }
}

// One vertical bar of the strip: a dark track at x, and in it the fill strip_update() sizes
static lv_obj_t *strip_bar(int x, uint32_t col)
{
    lv_obj_t *track = box(strip, x, STRIP_BAR_Y, 10, STRIP_BAR_H, 0x202020);
    return box(track, 0, STRIP_BAR_H - 1, 10, 1, col);
}

static void strip_init(lv_obj_t *scr)
{
    strip = lv_obj_create(scr);
    lv_obj_remove_style_all(strip);
    lv_obj_set_pos(strip, STRIP_X, STRIP_Y);
    lv_obj_set_size(strip, 480, 60);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < 16; i++) {
        step_obj[i] = box(strip, 4 + i * 13, STRIP_BAR_Y, 10, STRIP_BAR_H, 0x383838);
        lv_obj_set_style_radius(step_obj[i], 2, 0);
        step_col[i] = 0x383838;
    }
    cap_steps = text(strip, 4, STRIP_CAP_Y, 208, &lv_font_montserrat_14, COL_DIM, "STEPS");
    for (int i = 0; i < 4; i++) {                   // the dials and the faders: SLOOP's four colours
        bar_fill[i] = strip_bar(222 + i * 14, LIT_COL[LIT_T1 + i]);
        bar_fill[4 + i] = strip_bar(290 + i * 14, LIT_COL[LIT_T1 + i]);
    }
    bar_fill[8] = strip_bar(368, 0xFFFFFF);
    bar_fill[9] = strip_bar(404, 0x40C8FF);         // the pitch wheel
    bar_fill[10] = strip_bar(418, 0x40C8FF);        // the mod wheel
    for (int i = 0; i < NBARS; i++)
        bar_h[i] = 1;
    // the captions: at least 8 px between two of them; VOL and WHL centred under their bars
    text(strip, 222, STRIP_CAP_Y, 62, &lv_font_montserrat_14, COL_DIM, "DIALS");
    cap_fad = text(strip, 290, STRIP_CAP_Y, 62, &lv_font_montserrat_14, COL_DIM, "FAD");
    lv_obj_t *cv = text(strip, 357, STRIP_CAP_Y, 32, &lv_font_montserrat_14, COL_DIM, "VOL");
    lv_obj_t *cw = text(strip, 400, STRIP_CAP_Y, 32, &lv_font_montserrat_14, COL_DIM, "WHL");
    lv_obj_set_style_text_align(cv, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(cw, LV_TEXT_ALIGN_CENTER, 0);
    for (int i = 0; i < 3; i++)
        bat_obj[i] = box(strip, 442 + i * 12, STRIP_BAR_Y + 10, 10, 16, 0x303030);
    text(strip, 442, STRIP_CAP_Y, 38, &lv_font_montserrat_14, COL_DIM, "BAT");
    lv_obj_add_flag(strip, LV_OBJ_FLAG_HIDDEN);
}

static void roll_changed(void)
{
#if LV_CACHE_DEF_SIZE > 0
    lv_image_cache_drop(&roll_dsc);
#endif
    lv_obj_invalidate(roll_img);
}

static void tick(lv_timer_t *t)
{
    (void)t;
    uint8_t b = base;
    // the backlight: dim after DIM_TICKS with nothing played or changed, full again at once
    if (act) {
        act = 0;
        idle_ticks = 0;
        if (dimmed) {
            dimmed = false;
            display_brightness(255);
        }
    } else if (!dimmed && ++idle_ticks > DIM_TICKS) {
        dimmed = true;
        display_brightness(DIM_LEVEL);
    }
    if (link_new) {
        char s[40];
        link_new = 0;
        snprintf(s, sizeof s, "LINK  %u frames/s   lost %u", link_fps, link_lost);
        lv_label_set_text(lbl_link, s);
    }
    // the chord held (every channel but the pads')
    {
        uint16_t mask = 0;
        int bass = -1;
        for (int n = 0; n < 128; n++)
            if (note_on[n]) {
                mask |= (uint16_t)(1u << (n % 12));
                if (bass < 0)
                    bass = n % 12;
            }
        if (mask != chord_mask || (uint8_t)bass != chord_bass) {
            char s[32];
            chord_mask = mask;
            chord_bass = (uint8_t)bass;
            chord_name(s, sizeof s, mask, bass < 0 ? 0 : bass);
            lv_label_set_text(lbl_chord, s);
        }
    }
    // the note history: one row a tick, while a note sounds or is still on its way down
    if (b != base_shown && roll_busy) {             // the keys moved under it: start again
        memset(roll_pix, 0, (size_t)ROLL_W * ROLL_H * 2);
        roll_busy = 0;
        roll_changed();
    }
    {
        bool any = false;
        for (int i = 0; i < NKEYS && !any; i++)
            any = b + i < 128 && note_on[b + i];
        if (any || roll_busy) {
            memmove(roll_pix + ROLL_W, roll_pix, (size_t)ROLL_W * (ROLL_H - 1) * 2);
            memset(roll_pix, 0, (size_t)ROLL_W * 2);
            for (int i = 0; i < NKEYS; i++) {
                uint8_t on = b + i < 128 ? note_on[b + i] : 0;
                if (on)
                    for (int x = 0; x < ROLL_BAR; x++)
                        roll_pix[key_x[i] + x] = roll_col[on];
            }
            roll_busy = any ? ROLL_H : roll_busy - 1;
            roll_changed();
        }
    }
    if (b != base_shown) {
        char lo[8], hi[8], s[32];
        base_shown = b;
        note_name(lo, sizeof lo, b);
        note_name(hi, sizeof hi, b + NKEYS - 1);
        snprintf(s, sizeof s, "KEYS  %s - %s", lo, hi);
        lv_label_set_text(lbl_range, s);
    }
    for (int i = 0; i < NKEYS; i++) {
        int n = b + i;
        uint8_t on = n < 128 ? note_on[n] : 0;
        if (on != key_shown[i]) {
            key_shown[i] = on;
            lv_obj_set_style_bg_color(key_obj[i], lv_color_hex(on ? LIT_COL[on] : key_black[i] ? COL_BLACK : COL_WHITE), 0);
        }
    }
    for (int i = 0; i < NPADS; i++) {
        uint8_t on = pad_on[i];
        if (on != pad_shown[i]) {
            pad_shown[i] = on;
            lv_obj_set_style_bg_color(pad_obj[i], lv_color_hex(on ? LIT_COL[on] : COL_PAD), 0);
        }
    }
    if (stat_new) {
        uint8_t f = stat[0], trk = stat[1] & 3;
        char s[24];
        bool have = stat_have;
        stat_new = 0;
        lv_obj_t *all[4] = {bar_mode, bar_track, bar_bpm, bar_run};
        for (int i = 0; i < 4; i++) {
            if (have) lv_obj_remove_flag(all[i], LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(all[i], LV_OBJ_FLAG_HIDDEN);
        }
        for (int i = 0; i < NPADS; i++) {            // the drum names: not DAW mode's pads
            if (have && (f & 1)) lv_obj_add_flag(pad_name[i], LV_OBJ_FLAG_HIDDEN);
            else lv_obj_remove_flag(pad_name[i], LV_OBJ_FLAG_HIDDEN);
        }
        if (have) {
            lv_label_set_text(bar_mode, (f & 1) ? "DAW" : "SYNTH");
            if (trk == 3) snprintf(s, sizeof s, "DRUMS");
            else snprintf(s, sizeof s, "TRACK %d", trk + 1);
            lv_label_set_text(bar_track, s);
            lv_obj_set_style_text_color(bar_track, lv_color_hex(LIT_COL[LIT_T1 + trk]), 0);
            unsigned bpm10 = stat[3] | (unsigned)stat[4] << 8;
            snprintf(s, sizeof s, "%u BPM", bpm10 / 10);
            lv_label_set_text(bar_bpm, s);
            lv_label_set_text(bar_run, (f & 4) ? "REC" : (f & 2) ? "PLAY" : "STOP");
            lv_obj_set_style_text_color(bar_run, lv_color_hex((f & 4) ? 0xFF3030 : (f & 2) ? 0x30E060 : COL_DIM), 0);
        }
        strip_update(have && stat_len >= ST_STRIP);
        scale_update(have && stat_len >= ST_LEN);
    }
    if (last_new) {
        char s[48], nm[8];
        uint8_t st = last[0], d1 = last[1], d2 = last[2];
        last_new = 0;
        switch (st & 0xF0) {
        case 0x90:
            note_name(nm, sizeof nm, d1);
            snprintf(s, sizeof s, "CH %d  NOTE %s  VEL %d", (st & 15) + 1, nm, d2);
            break;
        case 0xB0:
            snprintf(s, sizeof s, "CH %d  CC %d  = %d", (st & 15) + 1, d1, d2);
            break;
        case 0xE0:
            snprintf(s, sizeof s, "CH %d  BEND %d", (st & 15) + 1, ((d2 << 7) | d1) - 8192);
            break;
        default:
            s[0] = 0;
            break;
        }
        if (s[0])
            lv_label_set_text(lbl_last, s);
    }
}

static void cheat_sheet(lv_obj_t *scr)
{
    static const char PRINTED[] =
        "[ BUTTONS ]\n"
        "SEQ PLAY\n"
        "SEQ REC\n"
        "PATCH\n"
        "CHORD\n"
        "NOTE REPEAT\n"
        "SCALE\n"
        "PARA\n"
        "GLOBE\n"
        "BT\n"
        "KNOB BANK\n"
        "FADER BANK\n"
        "\n"
        "[ KNOBS ]\n"
        "K1\n"
        "K2\n"
        "K3\n"
        "K4\n"
        "K5 - K8\n"
        "\n"
        "[ FADERS ]\n"
        "FADER 1 - 4\n"
        "BANK A B C D";
    // Two buttons pressed together: the names are too long for the left column, so each combo has a full-width
    // line for its buttons (grey) and one under it for what it does (white).
    static const char COMBO_KEYS[] =
        "[ PRESS BOTH TOGETHER ]\n"
        "<  and  >\n"
        "\n"
        "KNOB BANK and FADER BANK\n"
        "\n"
        "PATCH (hold) and OCTAVE+\n";
    static const char COMBO_DOES[] =
        "\n"
        "\n"
        "   switch SYNTH / DAW mode\n"
        "\n"
        "   pads play DRUMS or set STEPS\n"
        "\n"
        "   edit the DAW map (DAW mode)";
    static const char SLOOP[] =
        "\n"
        "PLAY\n"
        "REC  hold: clear\n"
        "HOME  hold: menu\n"
        "ENV\n"
        "LFO\n"
        "SEL\n"
        "EDIT\n"
        "GLO\n"
        "SAVE\n"
        "SOUND PICKER\n"
        "NEXT BANK\n"
        "\n"
        "\n"
        "MASTER\n"
        "SELECT  TEMPO\n"
        "TRACK\n"
        "SOUND  KIT\n"
        "DIAL 1 - 4\n"
        "\n"
        "\n"
        "TRACK 1 - 4\n"
        "LVL FLT REV DLY";
    text(scr, 12, 16, SIDE_W - 16, &lv_font_montserrat_20, 0xFFFFFF, "SMK-37  >  SLOOP");
    lv_obj_t *col = text(scr, 12, 50, 118, &lv_font_montserrat_14, COL_DIM, PRINTED);
    lv_obj_t *col2 = text(scr, 132, 50, SIDE_W - 136, &lv_font_montserrat_14, 0xFFFFFF, SLOOP);
    lv_obj_t *keys = text(scr, 12, 0, SIDE_W - 16, &lv_font_montserrat_14, COL_DIM, COMBO_KEYS);
    lv_obj_t *does = text(scr, 12, 0, SIDE_W - 16, &lv_font_montserrat_14, 0xFFFFFF, COMBO_DOES);
    lv_obj_t *all[4] = {col, col2, keys, does};
    for (int i = 0; i < 4; i++)                     // 30 lines in 550 px: one pixel between the lines
        lv_obj_set_style_text_line_space(all[i], 1, 0);
    lv_obj_update_layout(scr);
    lv_obj_align_to(keys, col, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 12);
    lv_obj_align_to(does, col, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 12);
}

static void piano(lv_obj_t *scr)
{
    static const uint8_t IS_BLACK[12] = {0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};
    int white = 0;
    // the white keys first, so the black ones lie over them
    for (int i = 0; i < NKEYS; i++) {
        key_black[i] = IS_BLACK[i % 12];
        if (key_black[i])
            continue;
        key_obj[i] = box(scr, PIANO_X + white * WHITE_W, PIANO_Y, WHITE_W - 1, WHITE_H, COL_WHITE);
        key_x[i] = (uint16_t)(white * WHITE_W + (WHITE_W - 1 - ROLL_BAR) / 2);
        white++;
    }
    white = 0;
    for (int i = 0; i < NKEYS; i++) {
        if (!key_black[i]) {
            white++;
            continue;
        }
        key_obj[i] = box(scr, PIANO_X + white * WHITE_W - BLACK_W / 2 - 1, PIANO_Y, BLACK_W, BLACK_H, COL_BLACK);
        key_x[i] = (uint16_t)(white * WHITE_W - 1 - ROLL_BAR / 2);
    }
    for (int i = 0; i < NKEYS; i++) {               // the scale's dots (scale_update), near the keys' lower ends
        key_dot[i] = key_black[i] ? box(key_obj[i], 2, BLACK_H - 10, 4, 4, 0xB0B0B0)
                                  : box(key_obj[i], 3, WHITE_H - 11, 5, 5, 0x505050);
        lv_obj_set_style_radius(key_dot[i], 2, 0);
        lv_obj_add_flag(key_dot[i], LV_OBJ_FLAG_HIDDEN);
    }
}

void panel_init(void)
{
    display_lock(-1);
    lv_obj_t *scr = lv_screen_active();
    cheat_sheet(scr);

    lbl_range = text(scr, RIGHT_X + 4, 44, SIDE_W - 8, &lv_font_montserrat_20, 0xFFFFFF, "KEYS");
    piano(scr);
    text(scr, RIGHT_X + 4, 234, SIDE_W - 8, &lv_font_montserrat_20, 0xFFFFFF, "PADS");
    for (int i = 0; i < NPADS; i++) {
        int col = i % 8, row = i < 8 ? 1 : 0;       // lanes 0-7: the bottom row
        pad_obj[i] = box(scr, PADS_X + col * (PAD_SZ + PAD_GAP), PADS_Y + row * (PAD_SZ + PAD_GAP), PAD_SZ, PAD_SZ, COL_PAD);
        lv_obj_set_style_radius(pad_obj[i], 4, 0);
        // the drum it plays (sloop-fm1 firmware/src/drums.c LANE_NAME, two letters: the pad is 30 px)
        static const char *const LANE2[NPADS] = {"KK", "K2", "SN", "CP", "HH", "OH", "PH", "RM",
                                                 "S2", "LT", "HT", "CR", "RD", "SH", "CG", "CB"};
        pad_name[i] = lv_label_create(pad_obj[i]);
        lv_obj_set_style_text_font(pad_name[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(pad_name[i], lv_color_hex(0xE0E0E0), 0);
        lv_label_set_text(pad_name[i], LANE2[i]);
        lv_obj_center(pad_name[i]);
    }
    text(scr, RIGHT_X + 4, CHORD_Y + 4, 60, &lv_font_montserrat_14, COL_DIM, "CHORD");
    lbl_chord = text(scr, RIGHT_X + 66, CHORD_Y, SIDE_W - 70, &lv_font_montserrat_20, 0xFFFFFF, "");
    text(scr, RIGHT_X + 4, LAST_Y, SIDE_W - 8, &lv_font_montserrat_14, COL_DIM, "LAST MIDI");
    lbl_last = text(scr, RIGHT_X + 4, LAST_Y + 18, SIDE_W - 8, &lv_font_montserrat_14, 0xFFFFFF, "-");

    // the note history: a picture this file draws into (as view.c's), under the keys
    const size_t n = (size_t)ROLL_W * ROLL_H * 2;
    roll_pix = heap_caps_aligned_alloc(64, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(roll_pix);
    memset(roll_pix, 0, n);
    for (size_t i = 1; i < sizeof LIT_COL / sizeof LIT_COL[0]; i++)
        roll_col[i] = lv_color_to_u16(lv_color_hex(LIT_COL[i]));
    roll_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    roll_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    roll_dsc.header.w = ROLL_W;
    roll_dsc.header.h = ROLL_H;
    roll_dsc.header.stride = ROLL_W * 2;
    roll_dsc.data_size = n;
    roll_dsc.data = (const uint8_t *)roll_pix;
    roll_img = lv_image_create(scr);
    lv_image_set_src(roll_img, &roll_dsc);
    lv_obj_set_pos(roll_img, ROLL_X, ROLL_Y);
    lv_obj_set_size(roll_img, ROLL_W, ROLL_H);
    // the status bar: hidden until the keyboard sends its first status (an older firmware sends none)
    bar_mode = text(scr, BAR_X + 8, BAR_Y, 100, &lv_font_montserrat_20, 0xFFFFFF, "");
    bar_track = text(scr, BAR_X + 118, BAR_Y, 120, &lv_font_montserrat_20, 0xFFFFFF, "");
    bar_bpm = text(scr, BAR_X + 250, BAR_Y, 110, &lv_font_montserrat_20, 0xFFFFFF, "");
    bar_run = text(scr, BAR_X + 390, BAR_Y, 82, &lv_font_montserrat_20, COL_DIM, "");
    lv_obj_set_style_text_align(bar_run, LV_TEXT_ALIGN_RIGHT, 0);
    strip_init(scr);
    lbl_link = text(scr, RIGHT_X + 4, 12, SIDE_W - 8, &lv_font_montserrat_14, 0x606060, "");
    lbl_scale = text(scr, RIGHT_X + 150, 49, SIDE_W - 154, &lv_font_montserrat_14, COL_DIM, "");
    lv_obj_set_style_text_align(lbl_scale, LV_TEXT_ALIGN_RIGHT, 0);
    stat_new = 1;                                   // (hides them)
    lv_timer_create(tick, 30, NULL);
    display_unlock();
}

void panel_set_connected(bool on)
{
    if (on)
        return;
    memset((void *)note_on, 0, sizeof note_on);
    memset((void *)pad_on, 0, sizeof pad_on);
    base = BASE_DEF;
    stat_have = 0;
    stat_new = 1;
    act = 1;
}

void panel_link(unsigned frames_per_s, unsigned lost)
{
    link_fps = frames_per_s;
    link_lost = lost;
    link_new = 1;
}

void panel_status(const uint8_t *s, int len)
{
    if (len < ST_MIN)
        return;
    for (int i = 0; i < ST_LEN; i++)
        stat[i] = s[i];
    stat_len = (uint8_t)(len > ST_LEN ? ST_LEN : len);
    act = 1;
    stat_have = 1;
    stat_new = 1;
    int b = (s[2] & 127) / 12 * 12;                 // the piano starts on a C: the one at or under the lowest key
    if (b > 96)
        b = 96;
    base = (uint8_t)b;
}

void panel_midi(uint8_t status, uint8_t d1, uint8_t d2)
{
    uint8_t type = status & 0xF0, ch = status & 15;
    d1 &= 127;
    d2 &= 127;
    act = 1;
    if (stat_have && (stat[0] & 1) && stat_len >= ST_LEN) {     // DAW mode: the wheels are MIDI, not SLOOP's
        if (type == 0xE0) {
            stat[29] = d2;
            stat_new = 1;
        } else if (type == 0xB0 && d1 == 1) {
            stat[30] = d2;
            stat_new = 1;
        }
    }
    if (type == 0x90 || type == 0x80) {
        uint8_t on = type == 0x90 && d2 != 0, lit = LIT_PLAIN;
        bool daw = stat_have && (stat[0] & 1);
        uint8_t drum_ch = stat_have && !daw ? stat[8] : DRUM_CH;
        if (stat_have && !daw) {                    // the track that sends on this channel: its colour
            lit = LIT_OTHER;
            for (int i = 0; i < 4; i++)
                if (stat[5 + i] == ch) {
                    lit = (uint8_t)(LIT_T1 + i);
                    break;
                }
        }
        if (!on)
            lit = LIT_OFF;
        if (daw && ch == DRUM_CH && d1 >= 36 && d1 < 36 + NPADS) {
            pad_on[d1 - 36] = lit;                  // DAW mode's default map, bank A: bottom row 36-43, top row 44-51
        } else if (!daw && ch == drum_ch) {
            for (int i = 0; i < NPADS; i++)
                if (LANE_NOTE[i] == d1)
                    pad_on[i] = lit;
        } else {
            note_on[d1] = lit;
            if (on) {                                // the octave buttons moved the keys: the window follows
                int b = base;
                while (d1 < b && b >= 12)
                    b -= 12;
                while (d1 > b + NKEYS - 1 && b + 12 <= 96)
                    b += 12;
                base = (uint8_t)b;
            }
        }
        if (!on)
            return;
    } else if (type != 0xB0 && type != 0xE0) {
        return;
    }
    last[0] = status;
    last[1] = d1;
    last[2] = d2;
    last_new = 1;
}
