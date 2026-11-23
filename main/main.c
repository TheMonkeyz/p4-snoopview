// snoopview: a big-screen mirror for snoop37 (a MIDI keyboard with a 240x240 RGB565 screen) on the Guition
// JC1060P470C (ESP32-P4, 7" 1024x600). This skeleton shows a test pattern at 2x through the view and starts the USB
// host spike, which only logs the descriptors of a device plugged into the host port.
#include <assert.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "display.h"
#include "view.h"
#include "panel.h"
#include "usb_mirror.h"

static const char *TAG = "main";

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Colour bars on the top half, a grey ramp, then 1-pixel detail at the bottom: a checker, vertical lines and
// red/blue rows. At an exact 2x every source pixel is a sharp 2x2 block on the panel (no blur, no uneven columns).
static void test_pattern(uint16_t *s)
{
    static const uint8_t bars[8][3] = {
        {255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
        {255, 0, 255}, {255, 0, 0}, {0, 0, 255}, {0, 0, 0},
    };
    for (int y = 0; y < VIEW_SRC_H; y++) {
        for (int x = 0; x < VIEW_SRC_W; x++) {
            uint16_t c;
            if (y < 120) {
                const uint8_t *b = bars[x / 30];
                c = rgb565(b[0], b[1], b[2]);
            } else if (y < 150) {
                uint8_t v = (uint8_t)(x * 255 / (VIEW_SRC_W - 1));
                c = rgb565(v, v, v);
            } else if (x < 120) {
                c = ((x ^ y) & 1) ? 0xFFFF : 0x0000;                   // 1-pixel checker
            } else if (x < 180) {
                c = (x & 1) ? 0xFFFF : 0x0000;                         // 1-pixel vertical lines
            } else {
                c = (y & 1) ? rgb565(255, 0, 0) : rgb565(0, 0, 255);   // 1-pixel red/blue rows
            }
            if (x == 0 || y == 0 || x == VIEW_SRC_W - 1 || y == VIEW_SRC_H - 1) c = rgb565(255, 128, 0);   // frame
            s[y * VIEW_SRC_W + x] = c;
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "snoopview starting");
    display_init();
    view_init();
    panel_init();

    uint16_t *shadow = heap_caps_malloc(VIEW_SRC_W * VIEW_SRC_H * 2, MALLOC_CAP_SPIRAM);
    assert(shadow);
    test_pattern(shadow);
    // Two dirty rectangles, as the stream decoder will send them
    view_blit_dirty(shadow, 0, 0, VIEW_SRC_W, 120);
    view_blit_dirty(shadow, 0, 120, VIEW_SRC_W, 120);
    heap_caps_free(shadow);
    ESP_LOGI(TAG, "test pattern shown; the \"waiting for snoop37\" label stays up");

    usb_mirror_start();
}
