// The song page's lesson (lesson.h).
#include <stdio.h>
#include "lesson.h"

enum { L_WARN, L_PLAY1, L_REC, L_NOTES1, L_SAVE_A, L_PLAY2, L_NOTES2, L_SAVE_B, L_PLAY3, L_SREC, L_ORDER, L_STOP,
       L_SONG, L_HEAR, L_DONE };

static int step = -1;
static lesson_in_t cur;                     // the last live state that came
static uint8_t entry_stored;                // the parts stored when the step began
static int arm_ticks, part_taps;

#define BIT(b)  (1u << (b))

static void go(int s)
{
    step = s;
    entry_stored = cur.stored;
    arm_ticks = 0;
    if (s == L_ORDER)
        part_taps = 0;
}

// Song mode is on while the lesson works on the loop: PLAY would play the old song and SONG REC would refuse.
static bool need_loop(void)
{
    return step >= L_PLAY1 && step <= L_SREC && cur.have && cur.song_mode;
}

static int save_part(void)                  // the part this step saves: 0 A, 1 B, -1 not a save step
{
    return step == L_SAVE_A ? 0 : step == L_SAVE_B ? 1 : -1;
}

void lesson_start(void)
{
    cur.have = false;
    go(L_WARN);
}

void lesson_stop(void)
{
    step = -1;
}

bool lesson_active(void)
{
    return step >= 0;
}

int lesson_step(void)
{
    return step;
}

void lesson_update(const lesson_in_t *in)
{
    if (step < 0)
        return;
    if (arm_ticks)
        arm_ticks--;
    if (!in->have) {                        // the link is lost: wait, the step stays
        cur.have = false;
        return;
    }
    cur = *in;
    for (;;) {
        int was = step, p = save_part();
        if (need_loop())
            break;
        switch (step) {
        case L_PLAY1: case L_PLAY2: case L_PLAY3:
            if (cur.playing) go(step + 1);
            break;
        case L_REC:
            if (cur.rec) go(L_NOTES1);
            break;
        case L_NOTES1: case L_NOTES2:
            if (!cur.playing) go(step + 1);
            break;
        case L_SAVE_A: case L_SAVE_B:       // a free part: the keyboard says it is stored (a used one: lesson_tap)
            if (!((entry_stored >> p) & 1) && ((cur.stored >> p) & 1)) go(step + 1);
            break;
        case L_SREC:
            if (cur.srec) go(L_ORDER);
            else if (!cur.playing) go(L_PLAY3);
            break;
        case L_ORDER:
            if (!cur.srec) go(part_taps >= 2 ? L_STOP : L_PLAY3);
            break;
        case L_STOP:
            if (!cur.playing) go(L_SONG);
            break;
        case L_SONG:
            if (cur.song_mode) go(L_HEAR);
            break;
        case L_HEAR:
            if (cur.song_plays) go(L_DONE);
            break;
        default:
            break;
        }
        if (step == was)
            break;
    }
}

void lesson_tap(int b)
{
    int p = save_part();
    if (step == L_WARN && b == SB_UNDO) {
        go(L_PLAY1);
    } else if (p >= 0 && b == SB_SAVE + p && ((entry_stored >> p) & 1)) {
        if (arm_ticks) go(step + 1);        // the second tap in time: the keyboard replaces the part
        else arm_ticks = LESSON_ARM;
    } else if (step == L_ORDER && (b == SB_PART || b == SB_PART + 1)) {
        part_taps++;
    }
}

uint32_t lesson_allowed(void)
{
    if (step < 0)
        return ~0u;
    if (need_loop())
        return BIT(SB_LOOP);
    switch (step) {
    case L_WARN:    return BIT(SB_UNDO);
    case L_REC:     return BIT(SB_REC);
    case L_NOTES1: case L_NOTES2:
        return cur.rec ? BIT(SB_PLAY) : BIT(SB_REC);
    case L_SAVE_A:  return BIT(SB_SAVE);
    case L_SAVE_B:  return BIT(SB_SAVE + 1);
    case L_SREC:    return BIT(SB_SREC);
    case L_ORDER:   return BIT(SB_PART) | BIT(SB_PART + 1) | (part_taps >= 2 ? BIT(SB_SREC) : 0);
    case L_SONG:    return BIT(SB_SONG);
    default:        return BIT(SB_PLAY);    // the PLAY steps, STOP, HEAR; done: STOP still works
    }
}

uint32_t lesson_frames(void)
{
    if (step < 0)
        return 0;
    if (step == L_DONE)
        return BIT(SB_LESSON);
    if (step == L_ORDER && !need_loop())
        return part_taps >= 2 ? BIT(SB_SREC) : BIT(SB_PART) | BIT(SB_PART + 1);
    return lesson_allowed();
}

void lesson_text(char *dst, size_t n)
{
    const char *s = "";
    int p = save_part();
    if (step < 0) {
        snprintf(dst, n, "%s", "");
        return;
    }
    if (step == L_WARN) {
        snprintf(dst, n, "LESSON: make a song. This replaces parts A and B and the song order. Tap START, or EXIT.");
        return;
    }
    if (step == L_DONE) {
        snprintf(dst, n, "Done: you made a song! To keep it after power-off: SONG EDIT page, SAVE SONG. Tap EXIT.");
        return;
    }
    if (need_loop()) {
        s = "Tap LOOP.";
    } else if (p >= 0) {
        if (arm_ticks) s = p ? "Part B is used. Tap B again now to replace it." : "Part A is used. Tap A again now to replace it.";
        else s = p ? "Tap SAVE LOOP AS: B." : "Tap SAVE LOOP AS: A.";
    } else {
        switch (step) {
        case L_PLAY1: case L_PLAY2: case L_PLAY3:
            s = "Tap PLAY."; break;
        case L_REC:     s = "Tap REC. It records the notes you play."; break;
        case L_NOTES1:  s = cur.rec ? "Play notes on the keyboard. Then tap STOP." : "Tap REC."; break;
        case L_NOTES2:  s = cur.rec ? "Play more notes: they make part B. Then tap STOP." : "Tap REC."; break;
        case L_SREC:    s = "Tap SONG REC."; break;
        case L_ORDER:
            s = part_taps == 0 ? "Tap A or B. Each part you tap plays next and goes into the song."
              : part_taps == 1 ? "Good. Wait a bar, then tap A or B again."
              : "Tap A or B for more, or tap STOP SONG REC to finish.";
            break;
        case L_STOP:    s = "Tap STOP."; break;
        case L_SONG:    s = "Tap SONG."; break;
        case L_HEAR:    s = "Tap PLAY to hear your song."; break;
        default:        break;
        }
    }
    snprintf(dst, n, "LESSON %d/%d: %s", step, LESSON_STEPS, s);
}
