#pragma once
// Snoop37 screen mirror, the USB side: starts the USB Host Library and the task that reads the keyboard stream.
// Call after view_init().
void usb_mirror_start(void);
