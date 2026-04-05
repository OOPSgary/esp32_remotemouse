// main.cpp – ESP32 RemoteMouse firmware entry point
//
// Topology:
//   Bluetooth keyboard/mouse (BLE or Classic BT)
//       ↓
//   [ESP32]  ── Wi-Fi UDP ──→  [Windows PC 192.168.0.29:10086]
//                                       ↓
//                               SendInput OS injection
//
// LED (GPIO2) status:
//   Slow blink (1 Hz)  – Bluetooth not yet connected
//   Fast blink (5 Hz)  – Wi-Fi not connected
//   Solid ON           – Normal operation

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "driver/gpio.h"

#include "config.h"
#include "hid/hid_types.h"
#include "bt/ble_hid_host.h"
#include "bt/classic_hid_host.h"
#include "net/udp_sender.h"
#include "storage/pairing_store.h"

static const char *TAG = "MAIN";

// HID event queue shared between BT host modules and the UDP sender task.
// Depth 32 is sufficient for burst keyboard/mouse events.
QueueHandle_t hid_event_queue;

#define LED_GPIO GPIO_NUM_2

// LED blink task: visual status indicator
static void led_task(void *arg)
{
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);

    // For a simple implementation we blink at a fixed rate; a full status
    // machine would integrate Wi-Fi and BT connection events.
    while (1) {
        gpio_set_level(LED_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(500));
        gpio_set_level(LED_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "ESP32 RemoteMouse starting");

    // 1. Initialise NVS (required for BT pairing and Wi-Fi credentials)
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition invalid, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs_err);
    }

    // 2. Create the default event loop (needed by Wi-Fi and BT)
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 3. Create HID event queue (depth 32, element = HidPacket)
    hid_event_queue = xQueueCreate(32, sizeof(HidPacket));
    if (!hid_event_queue) {
        ESP_LOGE(TAG, "Failed to create hid_event_queue");
        esp_restart();
    }

    // 4. Initialise Bluetooth controller and Bluedroid stack
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    bt_cfg.mode = ESP_BT_MODE_BTDM; // Dual-mode: Classic + BLE
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BTDM));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    // 5. Initialise BLE HID Host (registers GAP/GATTC callbacks, starts scan)
    ble_hid_host_init();
    ble_hid_host_start_task();

    // 6. Initialise Classic BT HID Host
    classic_hid_host_init();
    classic_hid_host_start_task();

    // 7. Initialise Wi-Fi and start UDP sender task
    udp_sender_init();

    // 8. LED status blink task
    xTaskCreate(led_task, "led", 1024, NULL, 1, NULL);

    ESP_LOGI(TAG, "All subsystems started");
}
