#pragma once
// The mirror view: snoop37's 240x240 RGB565 screen shown at exact 2x (480x480), centred on the 1024x600 panel.
// All three functions may be called from any task (they take display_lock inside). Call view_init() first, after
// display_init().
#include <stdbool.h>
#include <stdint.h>

#define VIEW_SRC_W  240
#define VIEW_SRC_H  240
#define VIEW_SCALE  2

void view_init(void);                       // black screen, centred 480x480 lv_image backed by a PSRAM RGB565 buffer, plus a small centred label "waiting for snoop37"
void view_set_connected(bool on);           // hides/shows the "waiting" label (and shows the test pattern again when off)
void view_test_pattern(void);               // the test pattern in the picture: at start and when the keyboard leaves
// Copy a dirty rectangle from a 240x240 native-endian RGB565 shadow buffer (row-major, stride 240)
// into the 480x480 buffer at exact 2x (each source pixel -> 2x2 block), then invalidate only that area.
void view_blit_dirty(const uint16_t *shadow240, int x, int y, int w, int h);
