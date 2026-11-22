/* Snoop37 screen mirror, the USB side: the ESP32-P4 is the host. It waits for the keyboard (1209:0001),
 * takes its vendor interface (class 0xFF, alt 1: one isochronous IN endpoint), reads one packet per
 * millisecond and hands them to the decoder (mirror_decode.c); the picture goes to view.c.
 *
 * One task owns everything here (the client task): the USB Host Library runs its callbacks from
 * usb_host_client_handle_events, so the decoder and the transfers need no lock. */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "usb/usb_host.h"
#include "usb/usb_helpers.h"
#include "driver/gpio.h"
#include "esp_intr_alloc.h"
#include "sdkconfig.h"
#include "mirror_decode.h"
#include "view.h"
#include "usb_mirror.h"

#define SNOOP_VID 0x1209
#define SNOOP_PID 0x0001
#define ISO_PKTS 8                  /* packets (ms) per transfer */
#define ISO_XFERS 3                 /* transfers in flight: the host never runs dry while one is handled */

static const char *TAG = "mirror";

/* the board's host port (Guition JC1060P470C: the USB-C marked "High speed USB", the P4's HS OTG controller and
 * UTMI PHY; its 5 V is the board's, always on). Kconfig: main/Kconfig.projbuild */
#if CONFIG_SNOOPVIEW_USB_HOST_FS
#define HOST_PERIPHERAL_MAP BIT1
#else
#define HOST_PERIPHERAL_MAP BIT0
#endif
#define HOST_VBUS_GPIO CONFIG_SNOOPVIEW_USB_VBUS_EN_GPIO

static TaskHandle_t client_th;

static usb_host_client_handle_t client;
static usb_device_handle_t dev;
static uint8_t new_addr;            /* a device to look at (0: none) */
static bool gone;                   /* the device left */
static bool running;                /* the stream is on */
static int in_flight;               /* transfers the library holds */
static uint8_t ifnum, ep_addr;
static uint16_t ep_mps;
static usb_transfer_t *iso[ISO_XFERS], *ctrl;
static bool ctrl_busy, want_resync;
static mirror_dec_t *dec;
static struct {
    uint32_t pkts, bytes, empty, errs, frames, resyncs;
} st;

static void client_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    (void)arg;
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV)
        new_addr = msg->new_dev.address;
    else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE && msg->dev_gone.dev_hdl == dev)
        gone = true;
}

static void ctrl_cb(usb_transfer_t *t)
{
    if (t->status != USB_TRANSFER_STATUS_COMPLETED)
        ESP_LOGW(TAG, "control request failed (%d)", (int)t->status);
    ctrl_busy = false;
    in_flight--;
}

/* a request with no data stage */
static bool ctrl_send(uint8_t type, uint8_t req, uint16_t value, uint16_t index)
{
    usb_setup_packet_t *s = (usb_setup_packet_t *)ctrl->data_buffer;
    if (ctrl_busy || !dev)
        return false;
    s->bmRequestType = type;
    s->bRequest = req;
    s->wValue = value;
    s->wIndex = index;
    s->wLength = 0;
    ctrl->num_bytes = sizeof(usb_setup_packet_t);
    ctrl->device_handle = dev;
    ctrl->bEndpointAddress = 0;
    ctrl->callback = ctrl_cb;
    ctrl->context = NULL;
    if (usb_host_transfer_submit_control(client, ctrl) != ESP_OK)
        return false;
    ctrl_busy = true;
    in_flight++;
    return true;
}

static void iso_submit(usb_transfer_t *t)
{
    int i;
    if (!running || gone)
        return;
    t->num_bytes = ISO_PKTS * ep_mps;
    for (i = 0; i < ISO_PKTS; i++)
        t->isoc_packet_desc[i].num_bytes = ep_mps;
    if (usb_host_transfer_submit(t) == ESP_OK)
        in_flight++;
    else
        st.errs++;
}

static void iso_cb(usb_transfer_t *t)
{
    int i, res = 0;
    in_flight--;
    if (running && !gone) {
        for (i = 0; i < t->num_isoc_packets; i++) {
            const usb_isoc_packet_desc_t *p = &t->isoc_packet_desc[i];
            if (p->status != USB_TRANSFER_STATUS_COMPLETED || p->actual_num_bytes <= 0) {
                st.empty++;                         /* an idle screen sends nothing: not an error */
                continue;
            }
            st.pkts++;
            st.bytes += (uint32_t)p->actual_num_bytes;
            res |= md_packet(dec, t->data_buffer + (size_t)i * ep_mps, (uint32_t)p->actual_num_bytes);
        }
        if (res & MD_RESYNC)
            want_resync = true;
        if (res & MD_FRAME) {
            int x, y, w, h;
            st.frames++;
            if (md_take_dirty(dec, &x, &y, &w, &h))
                view_blit_dirty(dec->shadow, x, y, w, h);
        }
    }
    iso_submit(t);
}

/* the vendor interface's number, endpoint and packet size from the configuration descriptor */
static bool find_mirror(const usb_config_desc_t *cfg)
{
    int off = 0;
    const usb_standard_desc_t *d = (const usb_standard_desc_t *)cfg;
    bool in_if = false;
    while ((d = usb_parse_next_descriptor(d, cfg->wTotalLength, &off)) != NULL) {
        if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t *f = (const usb_intf_desc_t *)d;
            in_if = f->bInterfaceClass == 0xFF && f->bAlternateSetting == 1 && f->bNumEndpoints >= 1;
            if (in_if)
                ifnum = f->bInterfaceNumber;
        } else if (in_if && d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            const usb_ep_desc_t *e = (const usb_ep_desc_t *)d;
            if ((e->bEndpointAddress & 0x80) && (e->bmAttributes & 3) == 1) {
                ep_addr = e->bEndpointAddress;
                ep_mps = e->wMaxPacketSize & 0x7FF;
                return ep_mps > MIRROR_PKT_HDR;
            }
        }
    }
    return false;
}

static void dev_close(void)
{
    int i;
    running = false;
    /* the library still holds transfers: it gives them back (cancelled / no device) through the callbacks */
    for (i = 0; in_flight > 0 && i < 200; i++)
        usb_host_client_handle_events(client, pdMS_TO_TICKS(10));
    if (in_flight)
        ESP_LOGW(TAG, "%d transfers never came back", in_flight);
    for (i = 0; i < ISO_XFERS; i++)
        if (iso[i]) {
            usb_host_transfer_free(iso[i]);
            iso[i] = NULL;
        }
    if (ctrl) {
        usb_host_transfer_free(ctrl);
        ctrl = NULL;
    }
    if (dev) {
        usb_host_interface_release(client, dev, ifnum);
        usb_host_device_close(client, dev);
        dev = NULL;
    }
    ctrl_busy = false;
    want_resync = false;
    in_flight = 0;
    view_set_connected(false);
    ESP_LOGI(TAG, "closed");
}

static void dev_open(uint8_t addr)
{
    const usb_device_desc_t *dd;
    const usb_config_desc_t *cfg;
    usb_device_handle_t h;
    int i;
    if (dev || usb_host_device_open(client, addr, &h) != ESP_OK)
        return;
    if (usb_host_get_device_descriptor(h, &dd) != ESP_OK || usb_host_get_active_config_descriptor(h, &cfg) != ESP_OK) {
        usb_host_device_close(client, h);
        return;
    }
    ESP_LOGI(TAG, "device %d: %04x:%04x version %x.%02x", addr, dd->idVendor, dd->idProduct, dd->bcdDevice >> 8,
             dd->bcdDevice & 0xFF);
    usb_print_config_descriptor(cfg, NULL);
    if (dd->idVendor != SNOOP_VID || dd->idProduct != SNOOP_PID) {
        ESP_LOGI(TAG, "not a Snoop37");
        usb_host_device_close(client, h);
        return;
    }
    if (!find_mirror(cfg)) {
        ESP_LOGW(TAG, "a Snoop37 without the mirror interface: flash a SNOOP_MIRROR=1 build");
        usb_host_device_close(client, h);
        return;
    }
    if (usb_host_interface_claim(client, h, ifnum, 1) != ESP_OK) {
        ESP_LOGE(TAG, "interface %d alt 1: claim failed (packet size %d)", ifnum, ep_mps);
        usb_host_device_close(client, h);
        return;
    }
    dev = h;
    gone = false;
    in_flight = 0;
    if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + 8, 0, &ctrl) != ESP_OK)
        goto fail;
    for (i = 0; i < ISO_XFERS; i++) {
        if (usb_host_transfer_alloc((size_t)ISO_PKTS * ep_mps, ISO_PKTS, &iso[i]) != ESP_OK)
            goto fail;
        iso[i]->device_handle = dev;
        iso[i]->bEndpointAddress = ep_addr;
        iso[i]->callback = iso_cb;
        iso[i]->context = NULL;
    }
    md_reset(dec);
    memset(&st, 0, sizeof st);
    /* SET_INTERFACE alt 1: the keyboard starts the stream with a full redraw */
    if (!ctrl_send(0x01, 0x0B, 1, ifnum))
        goto fail;
    for (i = 0; ctrl_busy && i < 100; i++)
        usb_host_client_handle_events(client, pdMS_TO_TICKS(10));
    if (ctrl_busy)
        goto fail;
    running = true;
    for (i = 0; i < ISO_XFERS; i++)
        iso_submit(iso[i]);
    view_set_connected(true);
    ESP_LOGI(TAG, "Snoop37 %x.%02x: interface %d, endpoint %02x, %d bytes per ms", dd->bcdDevice >> 8, dd->bcdDevice & 0xFF,
             ifnum, ep_addr, ep_mps);
    return;
fail:
    ESP_LOGE(TAG, "could not start the stream");
    dev_close();
}

static void client_task(void *arg)
{
    const usb_host_client_config_t cc = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {.client_event_callback = client_cb, .callback_arg = NULL},
    };
    TickType_t last = xTaskGetTickCount();
    (void)arg;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);            /* the library is installed */
    ESP_ERROR_CHECK(usb_host_client_register(&cc, &client));
    ESP_LOGI(TAG, "waiting for a Snoop37 on the host port");
    for (;;) {
        usb_host_client_handle_events(client, pdMS_TO_TICKS(50));
        if (gone && dev) {
            ESP_LOGI(TAG, "the Snoop37 left");
            dev_close();
        }
        gone = false;
        if (new_addr) {
            uint8_t a = new_addr;
            new_addr = 0;
            dev_open(a);
        }
        if (running && want_resync && ctrl_send(0x41, MIRROR_REQ_RESYNC, 0, ifnum)) {
            want_resync = false;
            st.resyncs++;
        }
        if (running && xTaskGetTickCount() - last >= pdMS_TO_TICKS(5000)) {
            last = xTaskGetTickCount();
            ESP_LOGI(TAG, "%u packets, %u bytes, %u frames, %u empty, lost %u, bad %u, resyncs %u", (unsigned)st.pkts,
                     (unsigned)st.bytes, (unsigned)st.frames, (unsigned)st.empty, (unsigned)dec->lost, (unsigned)dec->bad,
                     (unsigned)st.resyncs);
        }
    }
}

static void lib_task(void *arg)
{
    const usb_host_config_t hc = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
        .peripheral_map = HOST_PERIPHERAL_MAP,
    };
    (void)arg;
#if HOST_VBUS_GPIO >= 0
    {
        gpio_config_t g = {.pin_bit_mask = 1ULL << HOST_VBUS_GPIO, .mode = GPIO_MODE_OUTPUT};
        gpio_config(&g);
        gpio_set_level(HOST_VBUS_GPIO, 1);
    }
#endif
    ESP_ERROR_CHECK(usb_host_install(&hc));             /* here: the interrupt is on this task's core */
    xTaskNotifyGive(client_th);
    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
            usb_host_device_free_all();
    }
}

void usb_mirror_start(void)
{
    dec = heap_caps_calloc(1, sizeof *dec, MALLOC_CAP_SPIRAM);
    assert(dec);
    md_init(dec);
    xTaskCreatePinnedToCore(client_task, "mirror", 6144, NULL, 4, &client_th, 0);
    xTaskCreatePinnedToCore(lib_task, "usb_lib", 4096, NULL, 5, NULL, 0);
}
