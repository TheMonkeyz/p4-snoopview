// The mirror view (view.h): one lv_image showing a 480x480 RGB565 buffer in PSRAM, which view_blit_dirty() fills
// at 2x from the device's 240x240 shadow. LVGL renders in software from that buffer (CPU reads through the cache, so
// no cache write-back is needed after the copy); only the copied area is invalidated.
#include <string.h>
#include "view.h"
#include "panel.h"
#include "display.h"
#include "lvgl.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "view";

#define DST_W (VIEW_SRC_W * VIEW_SCALE)
#define DST_H (VIEW_SRC_H * VIEW_SCALE)

static uint16_t *pix;                 // DST_W x DST_H RGB565 (LVGL's order: native-endian), PSRAM
static lv_image_dsc_t dsc;
static lv_obj_t *img, *label;

void view_init(void)
{
    const size_t n = (size_t)DST_W * DST_H * 2;
    pix = heap_caps_aligned_alloc(64, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(pix);
    memset(pix, 0, n);

    dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    dsc.header.w = DST_W;
    dsc.header.h = DST_H;
    dsc.header.stride = DST_W * 2;
    dsc.data_size = n;
    dsc.data = (const uint8_t *)pix;

    display_lock(-1);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    img = lv_image_create(scr);
    lv_image_set_src(img, &dsc);
    lv_image_set_antialias(img, false);
    lv_obj_set_size(img, DST_W, DST_H);
    lv_obj_center(img);

    // Small, centred horizontally just under the picture (over the picture it would hide the test pattern)
    label = lv_label_create(scr);
    lv_label_set_text(label, "waiting for snoop37");
    lv_obj_set_style_text_color(label, lv_color_hex(0x808080), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_align_to(label, img, LV_ALIGN_OUT_BOTTOM_MID, 0, 16);
    lv_obj_update_layout(scr);
    display_unlock();
    ESP_LOGI(TAG, "view %dx%d at 2x, centred on %dx%d", DST_W, DST_H, DISP_W, DISP_H);
}

// The picture changed under LVGL: drop any cached copy (none unless LVGL's image cache is on), then redraw the area
// a (in the image's own pixels) at its place on screen. Display lock held.
static void invalidate_img_area(int x, int y, int w, int h)
{
#if LV_CACHE_DEF_SIZE > 0
    lv_image_cache_drop(&dsc);
#endif
    lv_area_t c, a;
    lv_obj_get_coords(img, &c);
    a.x1 = c.x1 + x;
    a.y1 = c.y1 + y;
    a.x2 = a.x1 + w - 1;
    a.y2 = a.y1 + h - 1;
    lv_obj_invalidate_area(img, &a);
}

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Colour bars on the top half, a grey ramp, then 1-pixel detail at the bottom: a checker, vertical lines and
// red/blue rows. At an exact 2x every source pixel is a sharp 2x2 block on the panel (no blur, no uneven columns).
void view_test_pattern(void)
{
    static const uint8_t bars[8][3] = {
        {255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
        {255, 0, 255}, {255, 0, 0}, {0, 0, 255}, {0, 0, 0},
    };
    uint16_t *s = heap_caps_malloc(VIEW_SRC_W * VIEW_SRC_H * 2, MALLOC_CAP_SPIRAM);
    if (!s) return;                                                    // the picture stays as it is
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
    // Two dirty rectangles, as the stream decoder sends them
    view_blit_dirty(s, 0, 0, VIEW_SRC_W, 120);
    view_blit_dirty(s, 0, 120, VIEW_SRC_W, 120);
    heap_caps_free(s);
}

void view_set_connected(bool on)
{
    panel_set_connected(on);
    display_lock(-1);
    if (on) {
        lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    } else {
        memset(pix, 0, (size_t)DST_W * DST_H * 2);
        invalidate_img_area(0, 0, DST_W, DST_H);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
    }
    display_unlock();
    if (!on) view_test_pattern();                // the keyboard left: the test pattern again, as at start
}

void view_blit_dirty(const uint16_t *shadow240, int x, int y, int w, int h)
{
    // Clip to the source
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > VIEW_SRC_W) w = VIEW_SRC_W - x;
    if (y + h > VIEW_SRC_H) h = VIEW_SRC_H - y;
    if (!shadow240 || w <= 0 || h <= 0) return;

    display_lock(-1);
    // Each source pixel p becomes a 32-bit pair (p, p) on an even destination row, then that row is copied to the
    // odd row under it. Destination columns start at 2x (even), so the pairs are 4-byte aligned.
    for (int r = 0; r < h; r++) {
        const uint16_t *s = shadow240 + (size_t)(y + r) * VIEW_SRC_W + x;
        uint16_t *d0 = pix + (size_t)(2 * (y + r)) * DST_W + 2 * x;
        uint32_t *d = (uint32_t *)d0;
        for (int i = 0; i < w; i++) {
            uint32_t p = s[i];
            d[i] = p | (p << 16);
        }
        memcpy(d0 + DST_W, d0, (size_t)w * 4);
    }
    invalidate_img_area(2 * x, 2 * y, 2 * w, 2 * h);
    display_unlock();
}
