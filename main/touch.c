// Guition JC1060P470C: GT911 capacitive touch (I2C 0x5D or 0x14, SDA 7 / SCL 8), after esp32-s3-meteobus's
// touch_p4.c. Its RST and INT lines aren't the P4's to drive (its power comes with the panel's reset, display_p4.c),
// and it reports in its own resolution (800x480, read from its config), scaled here to the panel. Its config is
// read only, never written.
#include "touch.h"
#include "display.h"
#include "driver/i2c_master.h"
#include <stdio.h>
#include "esp_log.h"

static const char *TAG = "touch";

#define PIN_SDA   7
#define PIN_SCL   8

static i2c_master_dev_handle_t dev;
static i2c_master_bus_handle_t bus;
static bool ok;
static int max_x = PANEL_W, max_y = PANEL_H;
static int lr_state, lr_x, lr_y;            // the last report
static char note[48] = "GT911 not found: no touch";    // said again 12 s after start: the USB log misses the boot

static esp_err_t gt_read(uint16_t reg, uint8_t *d, size_t n)
{
    uint8_t r[2] = { reg >> 8, reg & 0xFF };
    return i2c_master_transmit_receive(dev, r, 2, d, n, 20);
}

static esp_err_t gt_write8(uint16_t reg, uint8_t v)
{
    uint8_t w[3] = { reg >> 8, reg & 0xFF, v };
    return i2c_master_transmit(dev, w, 3, 20);
}

void touch_init(void)
{
    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_1, .sda_io_num = PIN_SDA, .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK) { ESP_LOGE(TAG, "I2C bus init failed"); return; }
    // No bare address probe: right after the panel's reset it upsets the controller (meteobus). Read its product id
    // at either address instead.
    uint8_t addr = 0, id[4] = {0}, cfg[4] = {0};
    for (int i = 0; i < 2 && !addr; i++) {
        uint8_t a = i ? 0x14 : 0x5D;
        i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = a, .scl_speed_hz = 400000 };
        if (i2c_master_bus_add_device(bus, &dc, &dev) != ESP_OK) continue;
        if (gt_read(0x8140, id, 4) == ESP_OK) addr = a;
        else { i2c_master_bus_rm_device(dev); dev = NULL; }
    }
    if (!addr) { ESP_LOGE(TAG, "GT911 NOT found (0x5D, 0x14): no touch"); return; }
    if (gt_read(0x8048, cfg, 4) == ESP_OK) {             // the resolution the controller reports in
        int x = cfg[0] | cfg[1] << 8, y = cfg[2] | cfg[3] << 8;
        if (x > 0 && y > 0) { max_x = x; max_y = y; }
    }
    ok = true;
    snprintf(note, sizeof note, "GT%.4s found at 0x%02X, %dx%d", (char *)id, addr, max_x, max_y);
    ESP_LOGI(TAG, "%s", note);
}

int touch_poll(int *x, int *y)
{
    static int errs, polls;
    uint8_t s;
    if (polls < 400 && ++polls == 400) ESP_LOGI(TAG, "%s", note);
    if (!ok) return -1;
    if (gt_read(0x814E, &s, 1) != ESP_OK) {
        if (++errs == 50) {
            ok = false;
            snprintf(note, sizeof note, "GT911 stopped answering: touch off");
            ESP_LOGE(TAG, "%s", note);
        }
        return -1;
    }
    errs = 0;
    if (!(s & 0x80)) { *x = lr_x; *y = lr_y; return lr_state; }      // no new report yet: the last one stands
    if ((s & 0x0F) == 0) {
        lr_state = 0;
    } else {
        uint8_t d[4];
        if (gt_read(0x8150, d, 4) != ESP_OK) { gt_write8(0x814E, 0); return -1; }
        int rx = (d[0] | d[1] << 8) * PANEL_W / max_x, ry = (d[2] | d[3] << 8) * PANEL_H / max_y;
        lr_x = rx < 0 ? 0 : rx > PANEL_W - 1 ? PANEL_W - 1 : rx;
        lr_y = ry < 0 ? 0 : ry > PANEL_H - 1 ? PANEL_H - 1 : ry;
        lr_state = 1;
    }
    gt_write8(0x814E, 0);                                // report consumed
    *x = lr_x;
    *y = lr_y;
    return lr_state;
}
