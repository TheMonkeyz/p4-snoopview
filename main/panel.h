#pragma once
// The side panels beside the mirror picture (view.c): left, a cheat sheet (the SMK-37's printed names -> SLOOP's);
// right, a 37-key piano and the 16 pads, lit from the keyboard's USB MIDI.
#include <stdbool.h>
#include <stdint.h>

void panel_init(void);                      // after view_init(); takes display_lock inside
// One MIDI channel message from the keyboard (status with its channel, two data bytes). Any task; no lock taken:
// it only records state, an LVGL timer draws it.
void panel_midi(uint8_t status, uint8_t d1, uint8_t d2);
void panel_set_connected(bool on);          // off: every key and pad goes dark. Any task; no lock taken
// The keyboard's status record (mirror_proto.h 'S', MIRROR_STATUS_LEN bytes): the track colours, the piano's
// octave and the status bar over the picture. Any task; no lock taken.
void panel_status(const uint8_t *s);
