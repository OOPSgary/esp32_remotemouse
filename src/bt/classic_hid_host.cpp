#include "classic_hid_host.h"
#include "../hid/hid_parser.h"
#include "../hid/hid_types.h"
#include "../storage/pairing_store.h"

#include "esp_log.h"
#include "esp_hid_common.h"
#include "esp_hidh.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <string.h>

static const char *TAG = "CLASSIC_HID";

extern QueueHandle_t hid_event_queue; // defined in main.cpp

// Determine if a HID device looks like a keyboard or mouse based on its
// usage/report descriptor; we use the esp_hid helper structs where available.
static HidDeviceRole guess_role(esp_hidh_dev_t *dev)
{
    // Query the device's usage via esp_hidh API
    size_t num_reports = 0;
    const esp_hid_raw_report_map_t *maps = esp_hidh_dev_report_maps_get(dev, &num_reports);
    if (maps && num_reports > 0) {
        // Walk through usages – 0x06 0x01 + 0x09 0x02 → mouse; 0x09 0x06 → keyboard
        // This is a simplified heuristic; a full descriptor parser is out of scope.
        const uint8_t *d   = maps[0].data;
        uint16_t       len = maps[0].len;
        for (uint16_t i = 0; i + 1 < len; i++) {
            if (d[i] == 0x09) {
                if (d[i + 1] == 0x02) return HID_ROLE_MOUSE;    // Mouse usage
                if (d[i + 1] == 0x06) return HID_ROLE_KEYBOARD;  // Keyboard usage
            }
        }
    }
    return HID_ROLE_KEYBOARD; // default
}

// esp_hidh callback: called for all HID host events from the BT stack
static void hidh_callback(void *handler_args, esp_event_base_t base,
                          int32_t id, void *event_data)
{
    esp_hidh_event_t       event = (esp_hidh_event_t)id;
    esp_hidh_event_data_t *param = (esp_hidh_event_data_t *)event_data;

    switch (event) {
    case ESP_HIDH_OPEN_EVENT: {
        if (param->open.status != ESP_OK) {
            ESP_LOGW(TAG, "Open failed: %s", esp_err_to_name(param->open.status));
            break;
        }
        esp_hidh_dev_t *dev = param->open.dev;
        const uint8_t *bda = esp_hidh_dev_bda_get(dev);
        ESP_LOGI(TAG, "Classic BT HID connected: " MACSTR, MAC2STR(bda));
        // Persist the pairing
        pairing_store_save((uint8_t *)bda, PAIRING_PROTO_CLASSIC);
        break;
    }

    case ESP_HIDH_BATTERY_EVENT:
        ESP_LOGI(TAG, "Battery: %d%%", param->battery.level);
        break;

    case ESP_HIDH_INPUT_EVENT: {
        esp_hidh_dev_t *dev  = param->input.dev;
        const uint8_t  *data = param->input.data;
        uint16_t        len  = param->input.length;
        HidDeviceRole   role = guess_role(dev);
        HidPacket       pkt;
        if (hid_parse_report(data, len, role, &pkt)) {
            xQueueSend(hid_event_queue, &pkt, 0);
        }
        break;
    }

    case ESP_HIDH_CLOSE_EVENT: {
        const uint8_t *bda = esp_hidh_dev_bda_get(param->close.dev);
        ESP_LOGI(TAG, "Classic BT HID disconnected: " MACSTR, MAC2STR(bda));
        break;
    }

    default:
        break;
    }
}

void classic_hid_host_init(void)
{
    esp_hidh_config_t config = {
        .callback       = hidh_callback,
        .event_stack_size = 4096,
        .callback_arg   = NULL,
    };
    ESP_ERROR_CHECK(esp_hidh_init(&config));
    ESP_LOGI(TAG, "Classic BT HID Host initialised");
}

// Background task: Inquiry scan + reconnect to stored Classic BT devices
static void classic_hid_task(void *arg)
{
    // First pass: attempt reconnect to NVS-stored Classic BT devices
    PairedDevice devices[MAX_PAIRED_DEVICES];
    int n = pairing_store_load(devices, MAX_PAIRED_DEVICES);
    for (int i = 0; i < n; i++) {
        if (devices[i].protocol != PAIRING_PROTO_CLASSIC) continue;
        ESP_LOGI(TAG, "Reconnecting to stored Classic BT device " MACSTR,
                 MAC2STR(devices[i].bda));
        esp_hidh_dev_open(devices[i].bda, ESP_HID_TRANSPORT_BT, 0);
    }

    // Periodic Inquiry scan: esp_hidh does not expose a direct inquiry scan,
    // but opening with a known BDA retries automatically through the BT stack.
    // We therefore just sleep and let the BT stack handle reconnections.
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        // Re-attempt connection for any stored device not currently connected
        n = pairing_store_load(devices, MAX_PAIRED_DEVICES);
        for (int i = 0; i < n; i++) {
            if (devices[i].protocol != PAIRING_PROTO_CLASSIC) continue;
            // esp_hidh_dev_open is idempotent when device is already connected
            esp_hidh_dev_open(devices[i].bda, ESP_HID_TRANSPORT_BT, 0);
        }
    }
}

void classic_hid_host_start_task(void)
{
    xTaskCreate(classic_hid_task, "classic_hid", 4096, NULL, 5, NULL);
}
