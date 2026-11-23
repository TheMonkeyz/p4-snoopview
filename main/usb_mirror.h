#pragma once
// Snoop37 screen mirror, the USB side: starts the USB Host Library and the task that reads the keyboard stream.
// Call after view_init().
void usb_mirror_start(void);

#include <stdbool.h>
#include <stdint.h>
// A command for the keyboard: SLOOP's editor LIVE command (sloop-fm1 web/EDITOR_PROTOCOL.md "Live", firmware
// editor_live.c), sent as SysEx on its USB MIDI. args: the op and its arguments, n of them (1..10), each 0..127.
// Any task; dropped when no keyboard is there. Every command's reply is the live state: panel_live().
void usb_mirror_live(const uint8_t *args, int n);
// The same for another editor command: cmd 100, the song order (a SNOOP_MIRROR firmware's ED_SONG, editor.c).
// Its reply goes to panel_song().
void usb_mirror_cmd(uint8_t cmd, const uint8_t *args, int n);
// Ask for the live state five times a second (the song page is showing), or stop asking.
void usb_mirror_live_poll(bool on);
