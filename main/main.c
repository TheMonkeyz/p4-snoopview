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
#include "touch.h"
#include "usb_mirror.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "snoopview starting");
    display_init();
    view_init();
    panel_init();
    touch_init();

    view_test_pattern();
    ESP_LOGI(TAG, "test pattern shown; the \"waiting for snoop37\" label stays up");

    usb_mirror_start();
}
