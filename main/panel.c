// The side panels (panel.h). The picture is 480 wide in the middle of 1024: 272 px are free on each side.
// panel_midi() runs in the USB task and only records what is on; a 30 ms LVGL timer (the LVGL task, display lock
// held by it) recolours the keys that changed, so no lock is taken from the USB task and nothing is held for long.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "panel.h"
#include "touch.h"
#include "usb_mirror.h"
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
// fbank master batt | scale(2) root bend mod (the parts after the first | from a version 3 firmware, as far as
// it sends them)
#define ST_LEN      31
#define ST_SCALE    31                      // the scale and the wheels are there from this length on
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
// The left side's two pages: the cheat sheet and the song page. A tap on the title line (the top PAGE_TAP_H
// pixels) changes the page; on the song page the other taps are its buttons.
#define PAGE_TAP_H  46
#define LY_SONG     7                       // sloop-fm1 seq.c: the SAVE layer, whose keys the song page presses
enum { SB_PLAY, SB_REC, SB_PART, SB_SAVE = SB_PART + 4, SB_SREC = SB_SAVE + 4, SB_LOOP, SB_SONG, SB_UNDO, SB_COUNT,
       // the SONG EDIT page's buttons, in the same table
       EB_PREV = SB_COUNT, EB_NEXT, EB_PART, EB_M4 = EB_PART + 4, EB_M1, EB_P1, EB_P4, EB_ADD, EB_DEL, EB_SAVE, EB_COUNT };
enum { PG_CHEAT, PG_SONG, PG_EDIT, PG_COUNT };
#define ED_SONG     100                     // usb_mirror.h: the song order command
#define SONG_STEPS  16
#define SONG_LEN    (3 + 2 * SONG_STEPS)    // its reply: op rc count, then 16 x (part, bars)
typedef struct {
    int16_t x, y, w, h;
    lv_obj_t *box, *lab;
    uint32_t bg, fg;                        // as drawn
    char txt[16];
} sbtn_t;
static sbtn_t sb[EB_COUNT];
static lv_obj_t *page_obj[PG_COUNT], *song_help, *song_info;
static int page;                                    // the left side's page, PG_*
static bool touch_was;
// the SONG EDIT page
static lv_obj_t *edit_help, *edit_info, *cell_box[SONG_STEPS], *cell_lab[SONG_STEPS];
static volatile uint8_t songd[SONG_LEN], song_new;
static int song_age = 1000, edit_sel, edit_ask;     // ticks since the last order read; the line picked; ticks to the next read
static int cell_state[SONG_STEPS];                  // what a cell shows, packed (edit_update)
static char edit_help_shown[160], edit_info_shown[64], edit_note[64];
static int flash_btn = -1, flash_ticks;             // the button just tapped: a white frame for a moment
static int save_armed = -1, save_ticks;             // SAVE tapped on a used part: the second tap replaces it
// the keyboard's live state (editor_live.c ed_live): op flags flags2 bpm(2) playing+1 asked+1 chain_n chain_i mutes
// solos rec sel fx cpu song_entry song_entries song_bar song_bars stored ...
#define LIVE_LEN    20
static volatile uint8_t live[LIVE_LEN], live_new;
static int live_age = 1000;                         // ticks since the last one (more than LIVE_OLD: none)
#define LIVE_OLD    50
static char help_shown[160], info_shown[64];
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
                    stat_len >= ST_SCALE ? stat[29 + (i - 9)] : 0xFF;
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

// ---- the song page: SLOOP's song recording with buttons to tap (the keyboard does the work: usb_mirror_live) ----
static void live_key(int key)               // a key of the SAVE layer, down and up: editor_live.c op 2
{
    uint8_t d[4] = {2, LY_SONG, (uint8_t)key, 1};
    usb_mirror_live(d, 4);
    d[3] = 0;
    usb_mirror_live(d, 4);
}

static void song_tap(int b)
{
    uint8_t d[2];
    flash_btn = b;
    flash_ticks = 8;
    if (b == SB_PLAY) {
        d[0] = 1; d[1] = 2;                 // as the PLAY button
        usb_mirror_live(d, 2);
    } else if (b == SB_REC) {
        d[0] = 4; d[1] = 0;                 // as the REC button
        usb_mirror_live(d, 2);
    } else if (b == SB_UNDO) {
        d[0] = 5; d[1] = 0;
        usb_mirror_live(d, 2);
    } else if (b >= SB_PART && b < SB_PART + 4) {
        live_key(b - SB_PART);              // play it at the next bar (stopped: load it)
    } else if (b >= SB_SAVE && b < SB_SAVE + 4) {
        int i = b - SB_SAVE;
        bool used = live_age <= LIVE_OLD && ((live[19] >> i) & 1);
        // the keyboard itself wants a second tap within 3 s before it replaces a used part: say so here
        if (used && save_armed != i) {
            save_armed = i;
            save_ticks = 100;
        } else {
            save_armed = -1;
        }
        live_key(4 + i);
    } else if (b == SB_SREC) {
        live_key(13);
    } else if (b == SB_LOOP || b == SB_SONG) {
        bool song_mode = live_age <= LIVE_OLD && (live[1] & 4);
        if ((b == SB_SONG) != song_mode)
            live_key(12);                   // (it toggles)
    }
}

static void sb_set(int i, uint32_t bg, uint32_t fg, const char *txt)
{
    sbtn_t *b = &sb[i];
    if (b->bg != bg) {
        b->bg = bg;
        lv_obj_set_style_bg_color(b->box, lv_color_hex(bg), 0);
    }
    if (b->fg != fg) {
        b->fg = fg;
        lv_obj_set_style_text_color(b->lab, lv_color_hex(fg), 0);
    }
    if (txt && strcmp(b->txt, txt)) {
        snprintf(b->txt, sizeof b->txt, "%s", txt);
        lv_label_set_text(b->lab, txt);
    }
}

// The buttons, the help line and the info line from the live state. LVGL task, every tick while the page shows.
static void song_update(void)
{
    bool have = live_age <= LIVE_OLD;
    uint8_t f = have ? live[1] : 0, f2 = have ? live[2] : 0, stored = have ? live[19] & 15 : 0;
    bool playing = f & 1, song_mode = f & 4, song_plays = f & 8, srec = f & 64;
    bool rec = have && ((live[11] & 15) || (f2 & 1));
    int now = have ? live[5] - 1 : -1, next = have ? live[6] - 1 : -1, nstored = __builtin_popcount(stored);
    char help[160], info[64], t[16];
    static const char *const PART[4] = {"A", "B", "C", "D"};

    sb_set(SB_PLAY, playing ? 0x30E060 : 0x3A3A3A, playing ? 0x000000 : 0xFFFFFF, playing ? "STOP" : "PLAY");
    sb_set(SB_REC, rec ? 0xFF3030 : 0x3A3A3A, 0xFFFFFF, rec ? "REC ON" : "REC");
    for (int i = 0; i < 4; i++) {
        bool used = (stored >> i) & 1;
        sb_set(SB_PART + i, !used ? 0x242424 : i == now ? LIT_COL[LIT_T1 + i] : 0x2A3A4A, !used ? 0x606060 : i == now ? 0x000000 : 0xFFFFFF, NULL);
        lv_obj_set_style_border_width(sb[SB_PART + i].box, i == next || flash_btn == SB_PART + i ? 3 : 0, 0);
        snprintf(t, sizeof t, save_armed == i ? "%s ?" : "%s", PART[i]);
        sb_set(SB_SAVE + i, save_armed == i ? 0xFF3030 : 0x3A3A3A, 0xFFFFFF, t);
    }
    sb_set(SB_SREC, srec ? 0xFF3030 : 0x3A3A3A, 0xFFFFFF, srec ? "STOP SONG REC" : "SONG REC");
    sb_set(SB_LOOP, !song_mode ? 0xFFFFFF : 0x3A3A3A, !song_mode ? 0x000000 : 0xFFFFFF, NULL);
    sb_set(SB_SONG, song_mode ? 0xFFFFFF : 0x3A3A3A, song_mode ? 0x000000 : 0xFFFFFF, NULL);
    for (int i = 0; i < SB_COUNT; i++)
        if (i < SB_PART || i >= SB_PART + 4)
            lv_obj_set_style_border_width(sb[i].box, flash_btn == i ? 3 : 0, 0);

    // what to do next, in plain words
    if (!have)
        snprintf(help, sizeof help, "Waiting for the keyboard...");
    else if (save_armed >= 0)
        snprintf(help, sizeof help, "Part %s is used. Tap SAVE %s again now to replace it. This cannot be undone.", PART[save_armed], PART[save_armed]);
    else if (srec && playing)
        snprintf(help, sizeof help, "Recording the song. Tap the parts in the order you want. Tap STOP SONG REC to finish.");
    else if (srec)
        snprintf(help, sizeof help, "SONG REC is ready. Tap PLAY, then tap the parts in the order you want.");
    else if (song_mode)
        snprintf(help, sizeof help, playing ? "The song plays. Tap STOP to end it." : "SONG mode: tap PLAY to hear your song. Tap LOOP to work on the parts.");
    else if (!nstored)
        snprintf(help, sizeof help, "1. Play your loop (REC records your notes).  2. Tap STOP.  3. Tap SAVE LOOP AS: A.");
    else if (nstored == 1)
        snprintf(help, sizeof help, "Change the loop, tap STOP and save it as the next part. With 2 parts or more: tap PLAY, then SONG REC.");
    else
        snprintf(help, sizeof help, playing ? "Tap a part: it starts at the next bar. Tap SONG REC to record the order as a song."
                                            : "Tap PLAY. Then tap SONG REC and tap the parts in the order you want.");
    if (strcmp(help, help_shown)) {
        snprintf(help_shown, sizeof help_shown, "%s", help);
        lv_label_set_text(song_help, help);
    }
    if (!have)
        info[0] = 0;
    else if (song_plays)
        snprintf(info, sizeof info, "song: part %d of %d, bar %d of %d", live[15] + 1, live[16], live[17] + 1, live[18]);
    else if (now >= 0 && next >= 0 && next != now)
        snprintf(info, sizeof info, "playing %s, next %s", PART[now & 3], PART[next & 3]);
    else if (now >= 0)
        snprintf(info, sizeof info, "playing part %s", PART[now & 3]);
    else
        snprintf(info, sizeof info, "%d of 4 parts saved", nstored);
    if (strcmp(info, info_shown)) {
        snprintf(info_shown, sizeof info_shown, "%s", info);
        lv_label_set_text(song_info, info);
    }
}

static void sb_make(lv_obj_t *pg, int i, int x, int y, int w, int h, const char *txt, const lv_font_t *font)
{
    sbtn_t *b = &sb[i];
    b->x = (int16_t)x; b->y = (int16_t)y; b->w = (int16_t)w; b->h = (int16_t)h;
    b->bg = 0x3A3A3A;
    b->fg = 0xFFFFFF;
    b->box = box(pg, x, y, w, h, b->bg);
    lv_obj_set_style_radius(b->box, 6, 0);
    lv_obj_set_style_border_color(b->box, lv_color_white(), 0);
    b->lab = lv_label_create(b->box);
    lv_obj_set_style_text_font(b->lab, font, 0);
    lv_obj_set_style_text_color(b->lab, lv_color_white(), 0);
    snprintf(b->txt, sizeof b->txt, "%s", txt);
    lv_label_set_text(b->lab, txt);
    lv_obj_center(b->lab);
}

static void song_init(lv_obj_t *pg)
{
    static const char *const PART[4] = {"A", "B", "C", "D"};
    text(pg, 12, 16, 80, &lv_font_montserrat_20, 0xFFFFFF, "SONG");
    text(pg, 96, 20, SIDE_W - 100, &lv_font_montserrat_14, 0x606060, "tap here: edit the song");
    song_help = text(pg, 12, 50, SIDE_W - 24, &lv_font_montserrat_14, 0xFFC618, "");
    lv_label_set_long_mode(song_help, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(song_help, 70);
    sb_make(pg, SB_PLAY, 12, 126, 120, 56, "PLAY", &lv_font_montserrat_20);
    sb_make(pg, SB_REC, 140, 126, 120, 56, "REC", &lv_font_montserrat_20);
    text(pg, 12, 194, SIDE_W - 24, &lv_font_montserrat_14, COL_DIM, "PARTS   tap to play");
    text(pg, 12, 290, SIDE_W - 24, &lv_font_montserrat_14, COL_DIM, "SAVE LOOP AS");
    for (int i = 0; i < 4; i++) {
        sb_make(pg, SB_PART + i, 12 + i * 63, 214, 59, 64, PART[i], &lv_font_montserrat_20);
        sb_make(pg, SB_SAVE + i, 12 + i * 63, 310, 59, 46, PART[i], &lv_font_montserrat_20);
    }
    sb_make(pg, SB_SREC, 12, 372, 248, 56, "SONG REC", &lv_font_montserrat_20);
    sb_make(pg, SB_LOOP, 12, 440, 120, 48, "LOOP", &lv_font_montserrat_20);
    sb_make(pg, SB_SONG, 140, 440, 120, 48, "SONG", &lv_font_montserrat_20);
    sb_make(pg, SB_UNDO, 12, 500, 120, 44, "UNDO", &lv_font_montserrat_20);
    song_info = text(pg, 12, 560, SIDE_W - 24, &lv_font_montserrat_14, 0xFFFFFF, "");
}

// ---- the SONG EDIT page: the song order, a line at a time (the keyboard holds it: usb_mirror_cmd ED_SONG) ----
static void edit_cmd(int op, int a, int b, int c)
{
    uint8_t d[4] = {(uint8_t)op, (uint8_t)a, (uint8_t)b, (uint8_t)c};
    usb_mirror_cmd(ED_SONG, d, op == 1 ? 4 : op == 0 || op == 4 ? 1 : 2);
}

static void edit_tap(int b)
{
    int count = songd[2] ? songd[2] : 1, part = songd[3 + 2 * edit_sel] & 3, bars = songd[4 + 2 * edit_sel];
    static const int STEP[4] = {-4, -1, 1, 4};
    flash_btn = b;
    flash_ticks = 8;
    edit_note[0] = 0;
    if (song_age > LIVE_OLD)
        return;
    if (b == EB_PREV) {
        if (edit_sel > 0) edit_sel--;
    } else if (b == EB_NEXT) {
        if (edit_sel < count - 1) edit_sel++;
    } else if (b >= EB_PART && b < EB_PART + 4) {
        edit_cmd(1, edit_sel, b - EB_PART, bars);
    } else if (b >= EB_M4 && b <= EB_P4) {
        int v = bars + STEP[b - EB_M4];
        edit_cmd(1, edit_sel, part, v < 1 ? 1 : v > 64 ? 64 : v);
    } else if (b == EB_ADD) {
        edit_cmd(2, edit_sel, 0, 0);
    } else if (b == EB_DEL) {
        edit_cmd(3, edit_sel, 0, 0);
    } else if (b == EB_SAVE) {
        edit_cmd(4, 0, 0, 0);
    }
}

static void sb_set(int i, uint32_t bg, uint32_t fg, const char *txt);

static void edit_update(void)
{
    static const char *const PART[4] = {"A", "B", "C", "D"};
    bool have = song_age <= LIVE_OLD, live_ok = live_age <= LIVE_OLD;
    bool playing = live_ok && (live[1] & 1), song_plays = live_ok && (live[1] & 8);
    int count = have ? songd[2] : 0, total = 0;
    char help[160], info[64];
    if (edit_sel >= count) edit_sel = count ? count - 1 : 0;
    for (int i = 0; i < SONG_STEPS; i++) {
        int part = songd[3 + 2 * i] & 3, bars = songd[4 + 2 * i];
        bool used = i < count, sel = used && i == edit_sel, now = used && song_plays && live[15] == i;
        int st = !used ? 0 : 1 + part + 4 * bars + 1024 * sel + 2048 * now;
        if (i < count) total += bars;
        if (st == cell_state[i])
            continue;
        cell_state[i] = st;
        if (!used) {
            lv_label_set_text(cell_lab[i], "");
            lv_obj_set_style_bg_color(cell_box[i], lv_color_hex(0x000000), 0);
            continue;
        }
        lv_label_set_text_fmt(cell_lab[i], "%d   %s   %d bar%s%s", i + 1, PART[part], bars, bars == 1 ? "" : "s", now ? "  <" : "");
        lv_obj_set_style_bg_color(cell_box[i], lv_color_hex(sel ? 0xFFFFFF : 0x1C1C1C), 0);
        lv_obj_set_style_text_color(cell_lab[i], lv_color_hex(sel ? 0x000000 : LIT_COL[LIT_T1 + part]), 0);
    }
    for (int i = 0; i < 4; i++) {
        bool on = have && count && (songd[3 + 2 * edit_sel] & 3) == i;
        sb_set(EB_PART + i, on ? LIT_COL[LIT_T1 + i] : 0x3A3A3A, on ? 0x000000 : 0xFFFFFF, NULL);
    }
    for (int i = EB_PREV; i < EB_COUNT; i++)
        lv_obj_set_style_border_width(sb[i].box, flash_btn == i ? 3 : 0, 0);
    if (!have)
        snprintf(help, sizeof help, "Waiting for the song... This page needs the newer keyboard firmware.");
    else if (playing)
        snprintf(help, sizeof help, "The keyboard plays. Go to the SONG page and tap STOP. Then you can edit.");
    else
        snprintf(help, sizeof help, "Pick a line. Then set its part and its bars. SAVE SONG keeps it after power-off.");
    if (strcmp(help, edit_help_shown)) {
        snprintf(edit_help_shown, sizeof edit_help_shown, "%s", help);
        lv_label_set_text(edit_help, help);
    }
    if (edit_note[0])
        snprintf(info, sizeof info, "%s", edit_note);
    else if (have)
        snprintf(info, sizeof info, "%d line%s, %d bars", count, count == 1 ? "" : "s", total);
    else
        info[0] = 0;
    if (strcmp(info, edit_info_shown)) {
        snprintf(edit_info_shown, sizeof edit_info_shown, "%s", info);
        lv_label_set_text(edit_info, info);
    }
}

static void sb_make(lv_obj_t *pg, int i, int x, int y, int w, int h, const char *txt, const lv_font_t *font);

static void edit_init(lv_obj_t *pg)
{
    static const char *const PART[4] = {"A", "B", "C", "D"};
    static const char *const STEP[4] = {"- 4", "- 1", "+ 1", "+ 4"};
    text(pg, 12, 16, 130, &lv_font_montserrat_20, 0xFFFFFF, "SONG EDIT");
    text(pg, 146, 20, SIDE_W - 150, &lv_font_montserrat_14, 0x606060, "tap: cheat sheet");
    edit_help = text(pg, 12, 48, SIDE_W - 24, &lv_font_montserrat_14, 0xFFC618, "");
    lv_label_set_long_mode(edit_help, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(edit_help, 52);
    for (int i = 0; i < SONG_STEPS; i++) {          // two columns of eight lines
        cell_box[i] = box(pg, 12 + (i / 8) * 126, 104 + (i % 8) * 22, 122, 20, 0x000000);
        lv_obj_set_style_radius(cell_box[i], 3, 0);
        cell_lab[i] = lv_label_create(cell_box[i]);
        lv_obj_set_style_text_font(cell_lab[i], &lv_font_montserrat_14, 0);
        lv_label_set_text(cell_lab[i], "");
        lv_obj_align(cell_lab[i], LV_ALIGN_LEFT_MID, 4, 0);
        cell_state[i] = -1;
    }
    sb_make(pg, EB_PREV, 12, 286, 120, 46, LV_SYMBOL_UP "  LINE", &lv_font_montserrat_20);
    sb_make(pg, EB_NEXT, 140, 286, 120, 46, "LINE  " LV_SYMBOL_DOWN, &lv_font_montserrat_20);
    text(pg, 12, 338, 120, &lv_font_montserrat_14, COL_DIM, "PART");
    text(pg, 12, 410, 120, &lv_font_montserrat_14, COL_DIM, "BARS");
    for (int i = 0; i < 4; i++) {
        sb_make(pg, EB_PART + i, 12 + i * 63, 356, 59, 46, PART[i], &lv_font_montserrat_20);
        sb_make(pg, EB_M4 + i, 12 + i * 63, 428, 59, 46, STEP[i], &lv_font_montserrat_20);
    }
    sb_make(pg, EB_ADD, 12, 484, 120, 42, "ADD LINE", &lv_font_montserrat_20);
    sb_make(pg, EB_DEL, 140, 484, 120, 42, "DELETE", &lv_font_montserrat_20);
    sb_make(pg, EB_SAVE, 12, 534, 160, 42, "SAVE SONG", &lv_font_montserrat_20);
    edit_info = text(pg, 12, 580, SIDE_W - 24, &lv_font_montserrat_14, 0xFFFFFF, "");
}

// A side's page: a clear box over it that holds the page's objects, so one flag hides them all
static lv_obj_t *page_make(lv_obj_t *scr, int x)
{
    lv_obj_t *o = lv_obj_create(scr);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, 0);
    lv_obj_set_size(o, SIDE_W, DISP_H);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
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
    // touch: a tap on the left side's title line changes its page; the song page's other taps are its buttons.
    // The tap that wakes a dimmed screen does only that.
    {
        int tx = 0, ty = 0, r = touch_poll(&tx, &ty);
        if (r >= 0) {
            if (r && !touch_was) {
                act = 1;
                if (!dimmed && tx < SIDE_W && ty < PAGE_TAP_H) {       // the next page: cheat sheet, song, edit
                    lv_obj_add_flag(page_obj[page], LV_OBJ_FLAG_HIDDEN);
                    page = (page + 1) % PG_COUNT;
                    lv_obj_remove_flag(page_obj[page], LV_OBJ_FLAG_HIDDEN);
                    usb_mirror_live_poll(page != PG_CHEAT);
                    edit_ask = 0;
                    edit_note[0] = 0;
                } else if (!dimmed && page != PG_CHEAT && tx < SIDE_W) {
                    int first = page == PG_SONG ? 0 : EB_PREV, end = page == PG_SONG ? SB_COUNT : EB_COUNT, hit = -1;
                    for (int i = first; i < end && hit < 0; i++)
                        if (tx >= sb[i].x && tx < sb[i].x + sb[i].w && ty >= sb[i].y && ty < sb[i].y + sb[i].h)
                            hit = i;
                    if (hit >= 0 && page == PG_SONG)
                        song_tap(hit);
                    else if (hit >= 0)
                        edit_tap(hit);
                    else if (page == PG_EDIT && ty >= 104 && ty < 104 + 8 * 22 && song_age <= LIVE_OLD) {
                        int i = ((tx - 12) / 126 ? 8 : 0) + (ty - 104) / 22;      // a tap on a line picks it
                        if (tx >= 12 && i >= 0 && i < songd[2])
                            edit_sel = i;
                    }
                }
            }
            touch_was = r != 0;
        }
    }
    if (live_new) {
        live_new = 0;
        live_age = 0;
    } else if (live_age <= LIVE_OLD) {
        live_age++;
    }
    if (flash_ticks && !--flash_ticks)
        flash_btn = -1;
    if (save_ticks && !--save_ticks)
        save_armed = -1;
    if (song_new) {                                 // the song order came (a read, or a change's reply)
        static const char *const WHY[4] = {"", "Not possible here.", "", "Stop the keyboard first."};
        uint8_t op = songd[0], rc = songd[1];
        song_new = 0;
        song_age = 0;
        if (op && rc)
            snprintf(edit_note, sizeof edit_note, "%s", WHY[rc & 3]);
        else if (op == 4)
            snprintf(edit_note, sizeof edit_note, "Save sent: see the keyboard picture.");
        else if (op == 2 && edit_sel + 1 < songd[2])
            edit_sel++;                             // the new line is the one picked
    } else if (song_age <= LIVE_OLD) {
        song_age++;
    }
    if (page == PG_SONG) {
        song_update();
    } else if (page == PG_EDIT) {
        if (--edit_ask <= 0) {                      // read the order again once a second (the keyboard may change it)
            edit_ask = 33;
            edit_cmd(0, 0, 0, 0);
        }
        edit_update();
    }
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
        scale_update(have && stat_len >= ST_SCALE);
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
    text(scr, 12, 16, 180, &lv_font_montserrat_20, 0xFFFFFF, "SMK-37 > SLOOP");
    text(scr, 188, 20, SIDE_W - 192, &lv_font_montserrat_14, 0x606060, "tap: song");
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
    page_obj[PG_CHEAT] = page_make(scr, 0);
    cheat_sheet(page_obj[PG_CHEAT]);
    page_obj[PG_SONG] = page_make(scr, 0);
    song_init(page_obj[PG_SONG]);
    lv_obj_add_flag(page_obj[PG_SONG], LV_OBJ_FLAG_HIDDEN);
    page_obj[PG_EDIT] = page_make(scr, 0);
    edit_init(page_obj[PG_EDIT]);
    lv_obj_add_flag(page_obj[PG_EDIT], LV_OBJ_FLAG_HIDDEN);

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

void panel_live(const uint8_t *d, int n)
{
    if (n < LIVE_LEN)
        return;
    for (int i = 0; i < LIVE_LEN; i++)
        live[i] = d[i];
    live_new = 1;
}

void panel_song(const uint8_t *d, int n)
{
    if (n < SONG_LEN)
        return;
    for (int i = 0; i < SONG_LEN; i++)
        songd[i] = d[i];
    song_new = 1;
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
    if (stat_have && (stat[0] & 1) && stat_len >= ST_SCALE) {     // DAW mode: the wheels are MIDI, not SLOOP's
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
