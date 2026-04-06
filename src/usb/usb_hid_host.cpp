// usb_hid_host.cpp – USB HID Host with hub support (ESP32-S2/S3)
//
// Uses the ESP-IDF USB Host Library whose built-in hub driver transparently
// enumerates devices behind a USB hub / docking station.  Each downstream
// device fires a normal NEW_DEV event, so our code handles hubs and
// directly-connected devices identically.
//
// Boot-protocol HID interfaces (keyboard protocol=1, mouse protocol=2) are
// claimed, switched to boot protocol, and polled via interrupt-IN transfers.
// Parsed reports are pushed to the shared hid_event_queue consumed by the
// UDP sender – exactly the same path used by the Bluetooth HID modules.

#include "usb_hid_host.h"
#include "soc/soc_caps.h"

#if SOC_USB_OTG_SUPPORTED

#include "../hid/hid_parser.h"
#include "../hid/hid_types.h"

#include "esp_log.h"
#include "esp_intr_alloc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "usb/usb_host.h"

#include <string.h>

static const char *TAG = "USB_HID";

// ── USB / HID constants ────────────────────────────────────────────────

#define USB_DESC_TYPE_INTERFACE   4
#define USB_DESC_TYPE_ENDPOINT    5

#ifndef USB_CLASS_HID
#define USB_CLASS_HID             0x03
#endif
#ifndef USB_CLASS_HUB
#define USB_CLASS_HUB             0x09
#endif

#define HID_SUBCLASS_BOOT         0x01
#define HID_PROTOCOL_KEYBOARD     0x01
#define HID_PROTOCOL_MOUSE        0x02

// HID class requests (control endpoint)
#define HID_REQ_SET_IDLE          0x0A
#define HID_REQ_SET_PROTOCOL      0x0B
#define HID_BOOT_PROTOCOL_VAL     0x00

// Endpoint masks
#define EP_DIR_IN                 0x80
#define EP_XFER_TYPE_MASK         0x03
#define EP_XFER_TYPE_INTR         0x03

// ── internal state ──────────────────────────────────────────────────────

extern QueueHandle_t hid_event_queue; // created in main.cpp

// Per-HID-interface slot
typedef struct {
    bool                 in_use;
    usb_device_handle_t  dev_hdl;
    uint8_t              dev_addr;
    uint8_t              intf_num;
    uint8_t              ep_addr;
    uint16_t             ep_mps;       // max packet size
    HidDeviceRole        role;
    usb_transfer_t      *in_xfer;      // interrupt-IN transfer (continuously re-submitted)
} usb_hid_dev_t;

static usb_hid_dev_t               s_devs[USB_HID_MAX_DEVICES];
static usb_host_client_handle_t    s_client_hdl   = NULL;
static SemaphoreHandle_t           s_ctrl_sem     = NULL;   // synchronises control transfers

// Device event forwarded from client callback → client task
typedef enum { USB_DEV_EVT_NEW, USB_DEV_EVT_GONE } usb_dev_evt_type_t;
typedef struct {
    usb_dev_evt_type_t   type;
    union {
        uint8_t              new_addr;
        usb_device_handle_t  gone_hdl;
    };
} usb_dev_event_t;

static QueueHandle_t s_dev_evt_queue = NULL;

// ── helpers ─────────────────────────────────────────────────────────────

static usb_hid_dev_t *find_free_slot(void)
{
    for (int i = 0; i < USB_HID_MAX_DEVICES; i++) {
        if (!s_devs[i].in_use) return &s_devs[i];
    }
    return NULL;
}

// ── control transfer helper ─────────────────────────────────────────────

// Callback for one-shot control transfers – just gives the semaphore.
static void ctrl_xfer_done(usb_transfer_t *xfer)
{
    xSemaphoreGive((SemaphoreHandle_t)xfer->context);
}

// Send a no-data-stage HID class request (SET_IDLE / SET_PROTOCOL).
// Blocks until the device replies (or 2 s timeout).
static esp_err_t hid_class_request(usb_device_handle_t dev_hdl,
                                   uint8_t bRequest, uint16_t wValue,
                                   uint8_t intf_num)
{
    usb_transfer_t *xfer = NULL;
    esp_err_t err = usb_host_transfer_alloc(sizeof(usb_setup_packet_t), 0, &xfer);
    if (err != ESP_OK) return err;

    usb_setup_packet_t *setup = (usb_setup_packet_t *)xfer->data_buffer;
    setup->bmRequestType = USB_BM_REQUEST_TYPE_DIR_OUT
                         | USB_BM_REQUEST_TYPE_TYPE_CLASS
                         | USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
    setup->bRequest = bRequest;
    setup->wValue   = wValue;
    setup->wIndex   = intf_num;
    setup->wLength  = 0;

    xfer->num_bytes        = sizeof(usb_setup_packet_t);
    xfer->device_handle    = dev_hdl;
    xfer->bEndpointAddress = 0;          // EP0
    xfer->callback         = ctrl_xfer_done;
    xfer->context          = s_ctrl_sem;

    err = usb_host_transfer_submit_control(s_client_hdl, xfer);
    if (err == ESP_OK) {
        if (xSemaphoreTake(s_ctrl_sem, pdMS_TO_TICKS(2000)) != pdTRUE) {
            err = ESP_ERR_TIMEOUT;
            ESP_LOGW(TAG, "Control transfer timeout (req=0x%02X intf=%d)",
                     bRequest, intf_num);
        } else if (xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
            ESP_LOGW(TAG, "Control transfer status %d (req=0x%02X intf=%d)",
                     xfer->status, bRequest, intf_num);
            err = ESP_FAIL;
        }
    }

    usb_host_transfer_free(xfer);
    return err;
}

// ── interrupt-IN callback ───────────────────────────────────────────────
//
// Called from the USB Host Library daemon task when an interrupt-IN
// transfer completes.  Must return quickly – no blocking calls.

static void in_xfer_cb(usb_transfer_t *xfer)
{
    usb_hid_dev_t *dev = (usb_hid_dev_t *)xfer->context;
    if (!dev || !dev->in_use) return;

    if (xfer->status == USB_TRANSFER_STATUS_COMPLETED &&
        xfer->actual_num_bytes > 0) {
        HidPacket pkt;
        if (hid_parse_report(xfer->data_buffer,
                             (uint16_t)xfer->actual_num_bytes,
                             dev->role, &pkt)) {
            xQueueSend(hid_event_queue, &pkt, 0);
        }
    }

    // Re-submit unless the device has been physically removed
    if (xfer->status != USB_TRANSFER_STATUS_NO_DEVICE && dev->in_use) {
        usb_host_transfer_submit(xfer);
    }
}

// ── device enumeration ──────────────────────────────────────────────────

static void handle_new_device(uint8_t dev_addr)
{
    usb_device_handle_t dev_hdl = NULL;
    esp_err_t err = usb_host_device_open(s_client_hdl, dev_addr, &dev_hdl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Cannot open device addr=%d: %s",
                 dev_addr, esp_err_to_name(err));
        return;
    }

    const usb_device_desc_t *dev_desc;
    usb_host_get_device_descriptor(dev_hdl, &dev_desc);
    ESP_LOGI(TAG, "USB device addr=%d VID=0x%04X PID=0x%04X class=%d",
             dev_addr, dev_desc->idVendor, dev_desc->idProduct,
             dev_desc->bDeviceClass);

    // Hub-class devices are managed internally by the USB Host Library's
    // built-in hub driver – we just close our handle and move on.
    if (dev_desc->bDeviceClass == USB_CLASS_HUB) {
        ESP_LOGI(TAG, "Hub detected – handled by USB Host Library");
        usb_host_device_close(s_client_hdl, dev_hdl);
        return;
    }

    const usb_config_desc_t *cfg_desc;
    err = usb_host_get_active_config_descriptor(dev_hdl, &cfg_desc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Cannot get config descriptor: %s", esp_err_to_name(err));
        usb_host_device_close(s_client_hdl, dev_hdl);
        return;
    }

    // ── Walk the raw configuration descriptor to find HID interfaces and
    //    their interrupt-IN endpoints. ──
    const uint8_t *p   = (const uint8_t *)cfg_desc;
    const uint8_t *end = p + cfg_desc->wTotalLength;
    p += cfg_desc->bLength;                        // skip config header

    bool          claimed_any = false;
    bool          cur_is_hid  = false;             // current interface is boot HID?
    bool          cur_claimed = false;             // already claimed an EP for it?
    uint8_t       cur_intf_num = 0;
    HidDeviceRole cur_role     = HID_ROLE_KEYBOARD;

    while (p + 1 < end) {
        uint8_t bLength = p[0];
        uint8_t bType   = p[1];
        if (bLength == 0 || p + bLength > end) break;

        if (bType == USB_DESC_TYPE_INTERFACE &&
            bLength >= sizeof(usb_intf_desc_t)) {
            const usb_intf_desc_t *intf = (const usb_intf_desc_t *)p;
            cur_claimed  = false;
            cur_is_hid   = false;
            cur_intf_num = intf->bInterfaceNumber;

            if (intf->bInterfaceClass == USB_CLASS_HID &&
                intf->bInterfaceSubClass == HID_SUBCLASS_BOOT) {
                if (intf->bInterfaceProtocol == HID_PROTOCOL_KEYBOARD) {
                    cur_is_hid = true;
                    cur_role   = HID_ROLE_KEYBOARD;
                } else if (intf->bInterfaceProtocol == HID_PROTOCOL_MOUSE) {
                    cur_is_hid = true;
                    cur_role   = HID_ROLE_MOUSE;
                } else {
                    ESP_LOGI(TAG, "  intf %d: HID boot-subclass but protocol %d – skipped",
                             cur_intf_num, intf->bInterfaceProtocol);
                }
            }
        } else if (bType == USB_DESC_TYPE_ENDPOINT &&
                   bLength >= sizeof(usb_ep_desc_t) &&
                   cur_is_hid && !cur_claimed) {
            const usb_ep_desc_t *ep = (const usb_ep_desc_t *)p;

            // Only interested in interrupt-IN endpoints
            if (!(ep->bEndpointAddress & EP_DIR_IN) ||
                (ep->bmAttributes & EP_XFER_TYPE_MASK) != EP_XFER_TYPE_INTR) {
                p += bLength;
                continue;
            }

            usb_hid_dev_t *slot = find_free_slot();
            if (!slot) {
                ESP_LOGW(TAG, "No free USB HID slot");
                break;
            }

            // Claim the interface
            err = usb_host_interface_claim(s_client_hdl, dev_hdl, cur_intf_num, 0);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "  claim intf %d failed: %s",
                         cur_intf_num, esp_err_to_name(err));
                p += bLength;
                continue;
            }

            // Switch to boot protocol and set idle-rate to indefinite
            hid_class_request(dev_hdl, HID_REQ_SET_PROTOCOL,
                              HID_BOOT_PROTOCOL_VAL, cur_intf_num);
            hid_class_request(dev_hdl, HID_REQ_SET_IDLE,
                              0, cur_intf_num);

            // Allocate and submit interrupt-IN transfer for continuous polling
            uint16_t mps = ep->wMaxPacketSize;
            usb_transfer_t *in_xfer = NULL;
            err = usb_host_transfer_alloc(mps, 0, &in_xfer);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "  transfer alloc failed: %s", esp_err_to_name(err));
                usb_host_interface_release(s_client_hdl, dev_hdl, cur_intf_num);
                p += bLength;
                continue;
            }

            slot->in_use   = true;
            slot->dev_hdl  = dev_hdl;
            slot->dev_addr = dev_addr;
            slot->intf_num = cur_intf_num;
            slot->ep_addr  = ep->bEndpointAddress;
            slot->ep_mps   = mps;
            slot->role     = cur_role;
            slot->in_xfer  = in_xfer;

            in_xfer->device_handle    = dev_hdl;
            in_xfer->bEndpointAddress = ep->bEndpointAddress;
            in_xfer->callback         = in_xfer_cb;
            in_xfer->context          = slot;
            in_xfer->num_bytes        = mps;

            err = usb_host_transfer_submit(in_xfer);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "  submit IN failed: %s", esp_err_to_name(err));
                usb_host_transfer_free(in_xfer);
                usb_host_interface_release(s_client_hdl, dev_hdl, cur_intf_num);
                slot->in_use  = false;
                slot->in_xfer = NULL;
                p += bLength;
                continue;
            }

            cur_claimed = true;
            claimed_any = true;
            ESP_LOGI(TAG, "  intf %d: %s active (EP 0x%02X, MPS %d)",
                     cur_intf_num,
                     cur_role == HID_ROLE_KEYBOARD ? "Keyboard" : "Mouse",
                     ep->bEndpointAddress, mps);
        }

        p += bLength;
    }

    if (!claimed_any) {
        ESP_LOGI(TAG, "No boot-protocol HID interfaces on device addr=%d", dev_addr);
        usb_host_device_close(s_client_hdl, dev_hdl);
    }
}

// Clean up all HID slots belonging to a disconnected device and close it.
static void handle_device_gone(usb_device_handle_t gone_hdl)
{
    for (int i = 0; i < USB_HID_MAX_DEVICES; i++) {
        usb_hid_dev_t *d = &s_devs[i];
        if (!d->in_use || d->dev_hdl != gone_hdl) continue;

        ESP_LOGI(TAG, "USB HID gone: addr=%d intf=%d (%s)",
                 d->dev_addr, d->intf_num,
                 d->role == HID_ROLE_KEYBOARD ? "keyboard" : "mouse");

        d->in_use = false;   // prevent callback from re-submitting

        if (d->in_xfer) {
            usb_host_transfer_free(d->in_xfer);
            d->in_xfer = NULL;
        }

        usb_host_interface_release(s_client_hdl, gone_hdl, d->intf_num);
    }

    usb_host_device_close(s_client_hdl, gone_hdl);
}

// ── USB host client callback ────────────────────────────────────────────
//
// Called from usb_host_client_handle_events().  We queue the event for
// later processing in the client task to keep the callback short.

static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    usb_dev_event_t evt;
    switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        evt.type     = USB_DEV_EVT_NEW;
        evt.new_addr = msg->new_dev.address;
        xQueueSend(s_dev_evt_queue, &evt, portMAX_DELAY);
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        evt.type     = USB_DEV_EVT_GONE;
        evt.gone_hdl = msg->dev_gone.dev_hdl;
        xQueueSend(s_dev_evt_queue, &evt, portMAX_DELAY);
        break;
    default:
        break;
    }
}

// ── FreeRTOS tasks ──────────────────────────────────────────────────────

// Daemon task – drives the low-level USB host stack (port events,
// hub enumeration, transfer completions, etc.)
static void usb_daemon_task(void *arg)
{
    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_LOGW(TAG, "All USB clients deregistered");
        }
    }
}

// Client task – registers as a USB host client, processes device
// connect / disconnect events, and sets up HID polling.
static void usb_hid_client_task(void *arg)
{
    usb_host_client_config_t client_cfg = {
        .is_synchronous    = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg          = NULL,
        },
    };
    ESP_ERROR_CHECK(usb_host_client_register(&client_cfg, &s_client_hdl));
    ESP_LOGI(TAG, "USB HID client registered");

    while (1) {
        // Drive USB host client events (triggers client_event_cb)
        usb_host_client_handle_events(s_client_hdl, pdMS_TO_TICKS(50));

        // Drain queued device events
        usb_dev_event_t evt;
        while (xQueueReceive(s_dev_evt_queue, &evt, 0) == pdTRUE) {
            if (evt.type == USB_DEV_EVT_NEW) {
                handle_new_device(evt.new_addr);
            } else {
                handle_device_gone(evt.gone_hdl);
            }
        }
    }
}

// ── public API ──────────────────────────────────────────────────────────

void usb_hid_host_init(void)
{
    memset(s_devs, 0, sizeof(s_devs));
    s_ctrl_sem      = xSemaphoreCreateBinary();
    s_dev_evt_queue = xQueueCreate(8, sizeof(usb_dev_event_t));

    // Install the USB Host Library.  The built-in hub driver starts
    // automatically – any hub plugged in will have its downstream ports
    // enumerated, and each device behind it fires a normal NEW_DEV event.
    usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags     = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));
    ESP_LOGI(TAG, "USB Host Library installed (hub support built-in)");
}

void usb_hid_host_start_task(void)
{
    xTaskCreate(usb_daemon_task,     "usb_daemon", 4096, NULL, 5, NULL);
    xTaskCreate(usb_hid_client_task, "usb_hid",    4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "USB HID tasks started (%d max interfaces)", USB_HID_MAX_DEVICES);
}

#endif // SOC_USB_OTG_SUPPORTED
