// Host test of the song lesson (main/lesson.c): a small model of the keyboard takes the taps the lesson allows.
// Run: test/run.sh (WSL / Linux, cc).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lesson.h"

static lesson_in_t kb;                      // the model keyboard's live state
static int kb_armed = -1, fails;

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s (step %d)\n", __FILE__, __LINE__, #c, lesson_step()); fails++; } } while (0)

static void tick(int n)
{
    while (n-- > 0)
        lesson_update(&kb);
}

// A tap as panel.c's song_tap does it: locked buttons do nothing; else the lesson counts it and the keyboard acts.
static bool tap(int b)
{
    if (!((lesson_allowed() >> b) & 1))
        return false;
    bool start = lesson_step() == 0;
    lesson_tap(b);
    if (start)
        return true;                        // START sends no undo
    if (b == SB_PLAY) {
        kb.playing = !kb.playing;
        kb.song_plays = kb.playing && kb.song_mode;
        if (!kb.playing) kb.srec = false;
    } else if (b == SB_REC) {
        kb.rec = !kb.rec;
    } else if (b >= SB_SAVE && b < SB_SAVE + 4) {
        int i = b - SB_SAVE;
        if (((kb.stored >> i) & 1) && kb_armed != i) kb_armed = i;      // a used part: the second tap stores
        else { kb.stored |= (uint8_t)(1 << i); kb_armed = -1; }
    } else if (b == SB_SREC) {
        kb.srec = !kb.srec;
        if (kb.srec) kb.song_mode = false;
    } else if (b == SB_LOOP) {
        kb.song_mode = false;
    } else if (b == SB_SONG) {
        kb.song_mode = true;
    }
    tick(3);
    return true;
}

static void reset(uint8_t stored, bool song_mode, bool playing)
{
    memset(&kb, 0, sizeof kb);
    kb.have = true;
    kb.stored = stored;
    kb.song_mode = song_mode;
    kb.playing = playing;
    kb.song_plays = playing && song_mode;
    kb_armed = -1;
    lesson_stop();
    lesson_start();
    tick(3);
}

static bool text_has(const char *want)
{
    char s[160];
    lesson_text(s, sizeof s);
    if (strlen(s) > 130) { printf("FAIL: text too long for the help line: %s\n", s); fails++; }
    return strstr(s, want) != NULL;
}

// the whole lesson from the warning; used: parts A and B are stored already (each wants two taps)
static void run_all(bool used)
{
    CHECK(lesson_step() == 0 && text_has("START"));
    CHECK(!tap(SB_PLAY) && !tap(SB_SAVE));              // locked
    CHECK(lesson_frames() == 1u << SB_UNDO);
    CHECK(tap(SB_UNDO) && lesson_step() == 1);
    CHECK(!tap(SB_REC) && !tap(SB_SAVE) && !tap(SB_SREC) && !tap(SB_UNDO));
    CHECK(tap(SB_PLAY) && lesson_step() == 2);
    CHECK(tap(SB_REC) && lesson_step() == 3 && text_has("Play notes"));
    CHECK(!tap(SB_REC));                                // REC is on: only STOP
    CHECK(tap(SB_PLAY) && lesson_step() == 4 && text_has("SAVE LOOP AS: A"));
    CHECK(!tap(SB_SAVE + 1) && !tap(SB_SAVE + 2) && !tap(SB_PLAY));
    CHECK(tap(SB_SAVE));
    if (used) {
        CHECK(lesson_step() == 4 && text_has("again"));
        CHECK(tap(SB_SAVE));
    }
    CHECK(lesson_step() == 5 && (kb.stored & 1));
    CHECK(tap(SB_PLAY) && lesson_step() == 6 && text_has("part B"));
    CHECK(tap(SB_PLAY) && lesson_step() == 7 && text_has("SAVE LOOP AS: B"));
    CHECK(!tap(SB_SAVE));
    CHECK(tap(SB_SAVE + 1));
    if (used) {
        CHECK(lesson_step() == 7);
        CHECK(tap(SB_SAVE + 1));
    }
    CHECK(lesson_step() == 8 && (kb.stored & 2));
    CHECK(tap(SB_PLAY) && lesson_step() == 9);
    CHECK(!tap(SB_PART));
    CHECK(tap(SB_SREC) && lesson_step() == 10);
    CHECK(!tap(SB_SREC) && !tap(SB_PART + 2) && !tap(SB_PLAY));     // no finish before two parts
    CHECK(lesson_frames() == (1u << SB_PART | 1u << (SB_PART + 1)));
    CHECK(tap(SB_PART) && tap(SB_PART + 1) && lesson_step() == 10);
    CHECK(lesson_frames() == 1u << SB_SREC);
    CHECK(tap(SB_SREC) && lesson_step() == 11 && text_has("STOP"));
    CHECK(tap(SB_PLAY) && lesson_step() == 12);
    CHECK(tap(SB_SONG) && lesson_step() == 13);
    CHECK(tap(SB_PLAY) && lesson_step() == 14 && text_has("SAVE SONG"));
    CHECK(lesson_frames() == 1u << SB_LESSON);
    CHECK(tap(SB_PLAY) && lesson_step() == 14);         // STOP still works; the lesson stays done
    CHECK(!tap(SB_SAVE) && !tap(SB_SREC));
}

int main(void)
{
    CHECK(!lesson_active() && lesson_allowed() == ~0u && lesson_frames() == 0);

    reset(0, false, false);                 // an empty keyboard
    run_all(false);
    reset(0x0F, false, false);              // every part used: A and B are replaced with two taps
    run_all(true);
    CHECK(kb.stored == 0x0F);

    reset(1, false, false);                 // the second tap comes too late: asked again
    tap(SB_UNDO); tap(SB_PLAY); tap(SB_REC); tap(SB_PLAY);
    CHECK(lesson_step() == 4);
    tap(SB_SAVE);
    CHECK(text_has("again"));
    tick(LESSON_ARM + 1);
    CHECK(lesson_step() == 4 && text_has("SAVE LOOP AS: A"));
    tap(SB_SAVE); tap(SB_SAVE);
    CHECK(lesson_step() == 5);

    reset(0, true, false);                  // song mode is on: LOOP first
    tap(SB_UNDO);
    CHECK(lesson_step() == 1 && text_has("Tap LOOP") && lesson_allowed() == 1u << SB_LOOP);
    CHECK(!tap(SB_PLAY) && tap(SB_LOOP) && text_has("Tap PLAY"));

    reset(0, false, true);                  // it plays already: the PLAY step is skipped
    tap(SB_UNDO);
    tick(1);
    CHECK(lesson_step() == 2);
    tap(SB_REC);                            // REC goes off again by itself: the lesson asks for it
    kb.rec = false;
    tick(1);
    CHECK(lesson_step() == 3 && text_has("Tap REC") && !tap(SB_PLAY) && tap(SB_REC) && text_has("Play notes"));

    kb.have = false;                        // the link is lost: the step stays, then goes on
    kb.playing = false;
    tick(200);
    CHECK(lesson_step() == 3);
    kb.have = true;
    tick(1);
    CHECK(lesson_step() == 4);

    lesson_stop();                          // EXIT
    CHECK(!lesson_active() && lesson_allowed() == ~0u);

    printf(fails ? "lesson_test: %d FAILED\n" : "lesson_test: ok\n", fails);
    return fails ? 1 : 0;
}
