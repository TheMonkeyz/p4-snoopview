# snoopview

A "big screen mirror" for **snoop37**, a MIDI keyboard with a 240x240 RGB565 screen (USB full-speed device,
VID 0x1209, PID 0x0001). The keyboard will stream its draw commands over USB to an ESP32-P4, which is the USB host
and shows the picture at exact 2x (480x480), centred on its 7" 1024x600 panel.

Board: **Guition JC1060P470C** (ESP32-P4 rev v1.3, 16 MB flash, 32 MB PSRAM, JD9165 MIPI-DSI panel, V2). The display
driver is esp32-s3-meteobus's `display_p4.c` (hand-written, no BSP); read that project's `docs/P4-PORT.md` before
changing it (no long critical sections: the panel flashes white; display init runs on core 1).

This is the skeleton: the display, the view, and a USB host spike that only enumerates a device and logs its
descriptors. The USB receive code and the stream decoder come later.

| File | What |
| --- | --- |
| `main/display_p4.c`, `display.h`, `lvgl_mem.c` | From esp32-s3-meteobus, unchanged. LVGL 9.2.2 runs in its own task on core 1 |
| `main/view.c/.h` | `view_init()`, `view_set_connected()`, `view_blit_dirty()`: a 480x480 `lv_image` over a PSRAM buffer, filled at 2x from a 240x240 shadow; any task may call them |
| `main/usb_mirror.c/.h` | `usb_mirror_start()`: USB Host Library on core 0, logs each device's descriptors, reads the Snoop37 stream (isochronous), handles unplug / replug |
| `main/mirror_decode.c/.h`, `mirror_proto.h` | the stream decoder and wire format, copied from `sloop-fm1` (`tools/mirror`, `firmware/src/mirror_proto.h`) |
| `main/main.c` | Display, view, a test pattern (colour bars, grey ramp, 1-pixel checker / lines), then the USB spike |
| `main/Kconfig.projbuild` | Panel V2 (default on), USB host controller (HS default), optional VBUS enable GPIO (none) |

## The board's USB-C ports

From the schematic (sheet `6_USB`, Guition's package, mirrored at
github.com/sukesh-ak/JC1060P470C_I_W-GUITION-ESP32-P4_ESP32-C6, `5-Schematic`):

| Port (schematic label) | Wired to | Use here |
| --- | --- | --- |
| **"High speed USB"** (USB3) | the P4's dedicated USB 2.0 pins: OTG **HS** controller, UTMI PHY | **USB host: plug snoop37 here** |
| "Full Speed USB" (USB2) | GPIO 24/25: the USB-Serial-JTAG (COM7 on the PC, VID 303A) | flashing and the log |
| "USB-TTL" (USB1) | CH340 to UART0 (and the auto-reset / boot transistors) | UART0 console (also gets the log) |

VBUS of all three ports is tied straight to the board's 5 V input rail (`USB5V_IN`): no load switch and no enable
GPIO, so the host port has 5 V whenever the board is powered over USB (from either other port). Each port has
5.1 k pull-downs on CC (a "sink"), so a USB-C to USB-C cable from a strict USB-C device may not be seen as a host
port by the device; the device still gets VBUS. A USB-A to USB-C OTG adapter on the host port also works.

## Build

ESP-IDF v5.5.4 at `C:\Espressif\esp-idf`, from PowerShell in this folder:

    cmd /c "set IDF_COMPONENT_CACHE_PATH=C:\Users\lmathieu\.espressif\cm_cache&& call C:\Espressif\esp-idf\export.bat && idf.py -B build\p4 -D IDF_TARGET=esp32p4 -D SDKCONFIG=build\p4\sdkconfig build"

(or, in an ESP-IDF shell: `idf.py -B build\p4 -D IDF_TARGET=esp32p4 -D SDKCONFIG=build\p4\sdkconfig build`).
The component manager fetches LVGL 9.2.2 and esp_lcd_jd9165 2.0.2. After changing `sdkconfig.defaults*`, delete
`build\p4\sdkconfig` so the defaults are read again.

## Flash

Through the "Full Speed USB" port (USB-Serial-JTAG, COM7 here), with the IDF environment as above:

    idf.py -B build\p4 -p COM7 flash monitor

or with a standalone esptool (the pattern of esp32-s3-meteobus's `p4\flash_helper_p4.ps1`, its `tools\esptool.exe`;
bootloader at 0x2000 on the P4; 40m is what this build's `build\p4\flash_args` says):

    esptool.exe --chip esp32p4 -p COM7 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_freq 40m --flash_size 16MB 0x2000 build\p4\bootloader\bootloader.bin 0x8000 build\p4\partition_table\partition-table.bin 0x10000 build\p4\snoopview.bin

## What you should see

- Screen: black, a 480x480 test pattern in the middle (8 colour bars, a grey ramp, then a 1-pixel checker, 1-pixel
  vertical lines and red/blue rows, which must look like crisp 2x2 blocks), an orange 1-pixel frame, and
  "waiting for snoop37" in grey under the picture.
- Log: `display: JD9165 panel up (V2, ...)`, `view: view 480x480 at 2x...`, `mirror: waiting for a Snoop37 on the host port`.
  Plug a device into the "High speed USB" port: `mirror: device 1: 1209:0001 version x.xx`, then its configuration
  descriptor. A Snoop37 built with `SNOOP_MIRROR=1` then logs `mirror: Snoop37 ...: interface 2, endpoint 84, N bytes per ms`,
  the "waiting" label goes away and the keyboard's screen appears at 2x. Every 5 s a line counts packets, frames and losses.
