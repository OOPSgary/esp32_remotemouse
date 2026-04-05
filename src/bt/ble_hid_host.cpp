#include "ble_hid_host.h"
#include "../hid/hid_parser.h"
#include "../hid/hid_types.h"
#include "../storage/pairing_store.h"

#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_gatt_defs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <string.h>

static const char *TAG = "BLE_HID";

// UUID for the HID service (0x1812)
#define HID_SERVICE_UUID 0x1812
// UUID for HID Report characteristic (0x2A4D)
#define HID_REPORT_CHAR_UUID 0x2A4D
// UUID for Boot Keyboard Input Report (0x2A22)
#define BOOT_KB_INPUT_CHAR_UUID 0x2A22
// UUID for Boot Mouse Input Report (0x2A33)
#define BOOT_MOUSE_INPUT_CHAR_UUID 0x2A33
// UUID for Client Characteristic Configuration Descriptor (0x2902)
#define CCCD_UUID 0x2902

// Maximum GATTC applications (one per device slot)
#define GATTC_APP_NUM BLE_HID_MAX_DEVICES

extern QueueHandle_t hid_event_queue; // defined in main.cpp

// Per-device connection state
typedef struct {
    bool         in_use;
    uint16_t     gattc_if;
    uint16_t     conn_id;
    esp_bd_addr_t bda;
    bool         is_keyboard; // true=keyboard, false=mouse (determined by service char UUIDs)
    uint16_t     report_handle;       // handle of the chosen input report characteristic
    uint16_t     report_cccd_handle;  // handle of the CCCD for notifications
} ble_dev_t;

static ble_dev_t s_devs[BLE_HID_MAX_DEVICES];

// Scan parameters
static esp_ble_scan_params_t s_scan_params = {
    .scan_type          = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval      = 0x50,
    .scan_window        = 0x30,
    .scan_duplicate     = BLE_SCAN_DUPLICATE_DISABLE,
};

// Find a free device slot; returns NULL if full
static ble_dev_t *find_free_slot(void) {
    for (int i = 0; i < BLE_HID_MAX_DEVICES; i++) {
        if (!s_devs[i].in_use) return &s_devs[i];
    }
    return NULL;
}

// Find device slot by BDA
static ble_dev_t *find_dev_by_bda(esp_bd_addr_t bda) {
    for (int i = 0; i < BLE_HID_MAX_DEVICES; i++) {
        if (s_devs[i].in_use && memcmp(s_devs[i].bda, bda, ESP_BD_ADDR_LEN) == 0) {
            return &s_devs[i];
        }
    }
    return NULL;
}

// Find device slot by gattc_if + conn_id
static ble_dev_t *find_dev_by_conn(uint16_t gattc_if, uint16_t conn_id) {
    for (int i = 0; i < BLE_HID_MAX_DEVICES; i++) {
        if (s_devs[i].in_use &&
            s_devs[i].gattc_if == gattc_if &&
            s_devs[i].conn_id  == conn_id) {
            return &s_devs[i];
        }
    }
    return NULL;
}

// GAP event handler: handles scan results and connection events
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        esp_ble_gap_start_scanning(0); // 0 = scan indefinitely
        break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        esp_ble_gap_cb_param_t *scan = param;
        if (scan->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
            // Check if adv data contains HID service UUID 0x1812
            uint8_t  adv_data_len = scan->scan_rst.adv_data_len;
            uint8_t *adv_data     = scan->scan_rst.ble_adv;
            bool     found_hid    = false;
            uint8_t  pos          = 0;
            while (pos + 1 < adv_data_len) {
                uint8_t length = adv_data[pos];
                if (length == 0 || pos + length >= adv_data_len) break;
                uint8_t ad_type = adv_data[pos + 1];
                // AD type 0x02 = 16-bit UUID incomplete, 0x03 = complete
                if (ad_type == 0x02 || ad_type == 0x03) {
                    for (uint8_t j = 2; j + 1 <= length; j += 2) {
                        uint16_t uuid = adv_data[pos + j] | ((uint16_t)adv_data[pos + j + 1] << 8);
                        if (uuid == HID_SERVICE_UUID) {
                            found_hid = true;
                            break;
                        }
                    }
                }
                pos += length + 1;
            }
            if (!found_hid) break;

            // Skip if already connected
            if (find_dev_by_bda(scan->scan_rst.bda)) break;

            // Skip if all slots are full
            if (!find_free_slot()) break;

            ESP_LOGI(TAG, "Found BLE HID device " MACSTR ", connecting...",
                     MAC2STR(scan->scan_rst.bda));
            esp_ble_gap_stop_scanning();
            esp_ble_gattc_open(s_devs[0].gattc_if, // use first registered gattc_if for connect
                               scan->scan_rst.bda,
                               scan->scan_rst.ble_addr_type,
                               true);
        }
        break;
    }

    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        ESP_LOGI(TAG, "BLE scan stopped");
        break;

    default:
        break;
    }
}

// GATTC event handler: handles connection, service discovery, notification
static void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                esp_ble_gattc_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTC_REG_EVT:
        ESP_LOGI(TAG, "GATTC registered, if=%d status=%d", gattc_if, param->reg.status);
        // Store gattc_if into the corresponding slot
        for (int i = 0; i < BLE_HID_MAX_DEVICES; i++) {
            if (!s_devs[i].in_use) {
                s_devs[i].gattc_if = gattc_if;
                break;
            }
        }
        // Set scan params once (only on first registration)
        if (gattc_if == s_devs[0].gattc_if) {
            esp_ble_gap_set_scan_params(&s_scan_params);
        }
        break;

    case ESP_GATTC_OPEN_EVT:
        if (param->open.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "GATTC open failed status=%d", param->open.status);
            esp_ble_gap_start_scanning(0);
            break;
        }
        {
            ble_dev_t *slot = find_free_slot();
            if (!slot) {
                esp_ble_gattc_close(gattc_if, param->open.conn_id);
                break;
            }
            slot->in_use   = true;
            slot->gattc_if = gattc_if;
            slot->conn_id  = param->open.conn_id;
            memcpy(slot->bda, param->open.remote_bda, ESP_BD_ADDR_LEN);
            ESP_LOGI(TAG, "Connected to " MACSTR, MAC2STR(slot->bda));
            // Save BDA to NVS
            pairing_store_save(slot->bda, PAIRING_PROTO_BLE);
            // Discover services
            esp_ble_gattc_search_service(gattc_if, param->open.conn_id, NULL);
        }
        break;

    case ESP_GATTC_SEARCH_RES_EVT: {
        ble_dev_t *dev = find_dev_by_conn(gattc_if, param->search_res.conn_id);
        if (!dev) break;
        if (param->search_res.srvc_id.id.uuid.len == ESP_UUID_LEN_16 &&
            param->search_res.srvc_id.id.uuid.uuid.uuid16 == HID_SERVICE_UUID) {
            ESP_LOGI(TAG, "HID service found");
        }
        break;
    }

    case ESP_GATTC_SEARCH_CMPL_EVT: {
        ble_dev_t *dev = find_dev_by_conn(gattc_if, param->search_cmpl.conn_id);
        if (!dev) break;
        // Get all characteristics – look for Boot KB/Mouse input or generic HID Report
        uint16_t count = 0;
        esp_gattc_char_elem_t *chars = NULL;
        // Try Boot Keyboard first
        esp_bt_uuid_t kb_uuid = { .len = ESP_UUID_LEN_16,
                                  .uuid = { .uuid16 = BOOT_KB_INPUT_CHAR_UUID } };
        esp_ble_gattc_get_attr_count(gattc_if, param->search_cmpl.conn_id,
                                     ESP_GATT_DB_CHARACTERISTIC, 0x0001, 0xFFFF,
                                     ESP_GATT_INVALID_HANDLE, &count);
        if (count > 0) {
            chars = (esp_gattc_char_elem_t *)malloc(count * sizeof(esp_gattc_char_elem_t));
            if (!chars) break;
            if (esp_ble_gattc_get_char_by_uuid(gattc_if, param->search_cmpl.conn_id,
                                                0x0001, 0xFFFF, kb_uuid, chars, &count)
                == ESP_GATT_OK && count > 0) {
                dev->is_keyboard    = true;
                dev->report_handle  = chars[0].char_handle;
                ESP_LOGI(TAG, "Boot keyboard characteristic at handle %d", dev->report_handle);
            } else {
                // Try Boot Mouse
                esp_bt_uuid_t ms_uuid = { .len = ESP_UUID_LEN_16,
                                          .uuid = { .uuid16 = BOOT_MOUSE_INPUT_CHAR_UUID } };
                if (esp_ble_gattc_get_char_by_uuid(gattc_if, param->search_cmpl.conn_id,
                                                    0x0001, 0xFFFF, ms_uuid, chars, &count)
                    == ESP_GATT_OK && count > 0) {
                    dev->is_keyboard   = false;
                    dev->report_handle = chars[0].char_handle;
                    ESP_LOGI(TAG, "Boot mouse characteristic at handle %d", dev->report_handle);
                } else {
                    // Fallback: first HID Report characteristic
                    esp_bt_uuid_t rpt_uuid = { .len = ESP_UUID_LEN_16,
                                               .uuid = { .uuid16 = HID_REPORT_CHAR_UUID } };
                    if (esp_ble_gattc_get_char_by_uuid(gattc_if, param->search_cmpl.conn_id,
                                                        0x0001, 0xFFFF, rpt_uuid, chars, &count)
                        == ESP_GATT_OK && count > 0) {
                        dev->is_keyboard   = true; // assume keyboard by default
                        dev->report_handle = chars[0].char_handle;
                        ESP_LOGI(TAG, "HID Report characteristic at handle %d", dev->report_handle);
                    }
                }
            }
            free(chars);
        }
        if (dev->report_handle == 0) {
            ESP_LOGW(TAG, "No suitable HID characteristic found, disconnecting");
            esp_ble_gattc_close(gattc_if, param->search_cmpl.conn_id);
            break;
        }
        // Find CCCD for this characteristic (it's at handle + 1 or + 2 typically)
        // We iterate descriptors on the char handle
        esp_bt_uuid_t cccd_uuid = { .len = ESP_UUID_LEN_16,
                                    .uuid = { .uuid16 = CCCD_UUID } };
        uint16_t desc_count = 0;
        esp_ble_gattc_get_attr_count(gattc_if, param->search_cmpl.conn_id,
                                     ESP_GATT_DB_DESCRIPTOR, dev->report_handle,
                                     dev->report_handle + 4,
                                     ESP_GATT_INVALID_HANDLE, &desc_count);
        if (desc_count > 0) {
            esp_gattc_descr_elem_t *descs =
                (esp_gattc_descr_elem_t *)malloc(desc_count * sizeof(esp_gattc_descr_elem_t));
            if (descs) {
                if (esp_ble_gattc_get_descr_by_uuid(gattc_if, param->search_cmpl.conn_id,
                                                     dev->report_handle,
                                                     dev->report_handle + 4,
                                                     cccd_uuid, descs, &desc_count)
                    == ESP_GATT_OK && desc_count > 0) {
                    dev->report_cccd_handle = descs[0].handle;
                }
                free(descs);
            }
        }
        // Register for notifications on the report characteristic
        esp_ble_gattc_register_for_notify(gattc_if, dev->bda, dev->report_handle);
        break;
    }

    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
        if (param->reg_for_notify.status == ESP_GATT_OK) {
            // Enable notifications by writing 0x0001 to CCCD
            ble_dev_t *dev = NULL;
            for (int i = 0; i < BLE_HID_MAX_DEVICES; i++) {
                if (s_devs[i].in_use &&
                    s_devs[i].report_handle == param->reg_for_notify.handle) {
                    dev = &s_devs[i];
                    break;
                }
            }
            if (dev && dev->report_cccd_handle != 0) {
                uint8_t notify_en[2] = {0x01, 0x00};
                esp_ble_gattc_write_char_descr(gattc_if, dev->conn_id,
                                               dev->report_cccd_handle,
                                               sizeof(notify_en), notify_en,
                                               ESP_GATT_WRITE_TYPE_RSP,
                                               ESP_GATT_AUTH_REQ_NONE);
            }
        }
        break;

    case ESP_GATTC_NOTIFY_EVT: {
        ble_dev_t *dev = find_dev_by_conn(gattc_if, param->notify.conn_id);
        if (!dev) break;
        if (param->notify.handle != dev->report_handle) break;

        HidPacket pkt;
        HidDeviceRole role = dev->is_keyboard ? HID_ROLE_KEYBOARD : HID_ROLE_MOUSE;
        if (hid_parse_report(param->notify.value, param->notify.value_len, role, &pkt)) {
            xQueueSend(hid_event_queue, &pkt, 0);
        }
        break;
    }

    case ESP_GATTC_DISCONNECT_EVT: {
        ble_dev_t *dev = find_dev_by_conn(gattc_if, param->disconnect.conn_id);
        if (dev) {
            ESP_LOGI(TAG, "Disconnected from " MACSTR, MAC2STR(dev->bda));
            dev->in_use              = false;
            dev->conn_id             = 0;
            dev->report_handle       = 0;
            dev->report_cccd_handle  = 0;
        }
        // Resume scanning to reconnect or find new device
        esp_ble_gap_start_scanning(0);
        break;
    }

    default:
        break;
    }
}

void ble_hid_host_init(void)
{
    memset(s_devs, 0, sizeof(s_devs));

    esp_ble_gap_register_callback(gap_event_handler);
    esp_ble_gattc_register_callback(gattc_event_handler);

    // Register one GATTC application per device slot
    for (int i = 0; i < GATTC_APP_NUM; i++) {
        esp_ble_gattc_app_register(i);
    }

    ESP_LOGI(TAG, "BLE HID Host initialised (%d slots)", BLE_HID_MAX_DEVICES);
}

// Background task: periodically restart scan if fewer than max devices are connected
static void ble_hid_task(void *arg)
{
    while (1) {
        // Try to reconnect to previously paired devices from NVS
        PairedDevice devices[MAX_PAIRED_DEVICES];
        int n = pairing_store_load(devices, MAX_PAIRED_DEVICES);
        for (int i = 0; i < n; i++) {
            if (devices[i].protocol != PAIRING_PROTO_BLE) continue;
            if (!find_dev_by_bda(devices[i].bda)) {
                // Not currently connected – try to connect
                ble_dev_t *slot = find_free_slot();
                if (!slot) break;
                ESP_LOGI(TAG, "Reconnecting to stored BLE device " MACSTR,
                         MAC2STR(devices[i].bda));
                esp_ble_gattc_open(slot->gattc_if, devices[i].bda,
                                   BLE_ADDR_TYPE_PUBLIC, true);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void ble_hid_host_start_task(void)
{
    xTaskCreate(ble_hid_task, "ble_hid", 4096, NULL, 5, NULL);
}
