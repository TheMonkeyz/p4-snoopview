// The song page's lesson: one step at a time, the button to tap framed, the next step when the keyboard's live
// state shows the step done. Plain C (no LVGL, no USB), so test/lesson_test.c runs it on a PC. panel.c owns the
// buttons and the commands; this only says which step it is, what to write and which buttons work.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// the song page's buttons (panel.c's table sb[]; its SONG EDIT buttons follow from SB_COUNT)
enum { SB_PLAY, SB_REC, SB_PART, SB_SAVE = SB_PART + 4, SB_SREC = SB_SAVE + 4, SB_LOOP, SB_SONG, SB_UNDO, SB_LESSON,
       SB_COUNT };

#define LESSON_STEPS    13                  // steps 1..13; 0 is the warning, 14 the end
#define LESSON_ARM      80                  // ticks (30 ms) for the second tap that replaces a used part: under the
                                            // keyboard's own 3 s, so a tap counted here is one the keyboard takes too

typedef struct {
    bool have;                              // a live state came lately (not: the lesson waits)
    bool playing, rec, srec, song_mode, song_plays;
    uint8_t stored;                         // bit per part A..D
} lesson_in_t;

void lesson_start(void);
void lesson_stop(void);
bool lesson_active(void);
int lesson_step(void);                      // -1 off, 0 the warning (UNDO reads START), 1..13, 14 done
void lesson_update(const lesson_in_t *in);  // every tick while the song page shows
void lesson_tap(int button);                // a tap on a button lesson_allowed() has (not SB_LESSON: panel.c's own)
uint32_t lesson_allowed(void);              // bit per SB_*: the buttons that work now (off: all of them)
uint32_t lesson_frames(void);               // bit per SB_*: the buttons to frame
void lesson_text(char *dst, size_t n);      // what to do now
