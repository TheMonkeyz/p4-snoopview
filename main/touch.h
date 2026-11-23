#pragma once
// The panel's touch controller (GT911), read by polling: no LVGL input device, the panels only want taps.

void touch_init(void);                      // after display_init(): the controller is powered by the panel's reset
// The finger now: 1 down (x, y in screen pixels, 1024x600), 0 up, -1 nothing new or no controller.
// One short I2C read; call it from one task only.
int touch_poll(int *x, int *y);
