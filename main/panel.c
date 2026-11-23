// The side panels (panel.h). The picture is 480 wide in the middle of 1024: 272 px are free on each side.
// panel_midi() runs in the USB task and only records what is on; a 30 ms LVGL timer (the LVGL task, display lock
// held by it) recolours the keys that changed, so no lock is taken from the USB task and nothing is held for long.
#include <stdio.h>
#include <string.h>
#include "panel.h"
#include "display.h"
#include "lvgl.h"

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

#define DRUM_CH     9                       // MIDI channel 10: the pads, until the keyboard's status names the drum channel

#define BAR_X       SIDE_W                  // the status bar: the 60 px strip over the picture
#define BAR_Y       18

// What a lit key or pad shows: 1-4 the track's colour (SLOOP's TE_COL, sloop-fm1 firmware/src/ui_input.c PAL),
// 5 a channel no track has, 6 no status yet / DAW mode
enum { LIT_OFF, LIT_T1, LIT_T2, LIT_T3, LIT_T4, LIT_OTHER, LIT_PLAIN };
static const uint32_t LIT_COL[] = {0, 0x287CFF, 0x1ECC70, 0xFFC618, 0xFF621A, 0x9A9A9A, 0xFF8000};
#define ST_LEN      10                      // mirror_proto.h MIRROR_STATUS_LEN: flags track low bpm(2) ch[4] dawch
#define COL_WHITE   0xC8C8C8
#define COL_BLACK   0x181818
#define COL_PAD     0x2A2A2A
#define COL_DIM     0x909090

// SLOOP's drum lanes as MIDI notes (sloop-fm1 firmware/src/drums.c LANE_NOTE): a pad in DRUM KIT plays lane i.
// Drawn with lanes 0-7 on the bottom row and 8-15 on the top row (the keyboard's pads 9-16 / 1-8).
static const uint8_t LANE_NOTE[NPADS] = {36, 35, 38, 39, 42, 46, 44, 37, 40, 43, 48, 49, 51, 70, 63, 56};

static lv_obj_t *key_obj[NKEYS], *pad_obj[NPADS], *lbl_range, *lbl_last;
static lv_obj_t *bar_mode, *bar_track, *bar_bpm, *bar_run;
static uint8_t key_black[NKEYS];
static uint8_t key_shown[NKEYS], pad_shown[NPADS];  // what is drawn (LVGL task)

// written by the USB task, read by the timer: single bytes, so a torn read is at worst one frame late
static volatile uint8_t note_on[128], pad_on[NPADS];
static volatile uint8_t base = BASE_DEF, last[3], last_new;
static volatile uint8_t stat[ST_LEN], stat_have, stat_new;   // the keyboard's 'S' record
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

static void tick(lv_timer_t *t)
{
    (void)t;
    uint8_t b = base;
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
        "BANK A B C D\n"
        "\n"
        "[ COMBOS ]\n"
        "< + >\n"
        "KNOB+FADER BK\n"
        "PATCH + OCT+\n"
        "OCT- + OCT+";
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
        "LVL FLT REV DLY\n"
        "\n"
        "\n"
        "SYNTH - DAW\n"
        "PADS KIT - STEPS\n"
        "DAW MAP EDIT\n"
        "CALIB (POWER-ON)";
    text(scr, 12, 16, SIDE_W - 16, &lv_font_montserrat_20, 0xFFFFFF, "SMK-37  >  SLOOP");
    text(scr, 12, 56, 118, &lv_font_montserrat_14, COL_DIM, PRINTED);
    text(scr, 132, 56, SIDE_W - 136, &lv_font_montserrat_14, 0xFFFFFF, SLOOP);
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
        white++;
    }
    white = 0;
    for (int i = 0; i < NKEYS; i++) {
        if (!key_black[i]) {
            white++;
            continue;
        }
        key_obj[i] = box(scr, PIANO_X + white * WHITE_W - BLACK_W / 2 - 1, PIANO_Y, BLACK_W, BLACK_H, COL_BLACK);
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
    }
    text(scr, RIGHT_X + 4, 360, SIDE_W - 8, &lv_font_montserrat_14, COL_DIM, "LAST MIDI");
    lbl_last = text(scr, RIGHT_X + 4, 380, SIDE_W - 8, &lv_font_montserrat_14, 0xFFFFFF, "-");
    // the status bar: hidden until the keyboard sends its first status (an older firmware sends none)
    bar_mode = text(scr, BAR_X + 8, BAR_Y, 100, &lv_font_montserrat_20, 0xFFFFFF, "");
    bar_track = text(scr, BAR_X + 118, BAR_Y, 120, &lv_font_montserrat_20, 0xFFFFFF, "");
    bar_bpm = text(scr, BAR_X + 250, BAR_Y, 110, &lv_font_montserrat_20, 0xFFFFFF, "");
    bar_run = text(scr, BAR_X + 390, BAR_Y, 82, &lv_font_montserrat_20, COL_DIM, "");
    lv_obj_set_style_text_align(bar_run, LV_TEXT_ALIGN_RIGHT, 0);
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
}

void panel_status(const uint8_t *s)
{
    for (int i = 0; i < ST_LEN; i++)
        stat[i] = s[i];
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
