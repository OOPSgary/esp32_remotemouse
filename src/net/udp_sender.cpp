#include "udp_sender.h"
#include "../config.h"
#include "../hid/hid_types.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include <string.h>
#include <errno.h>

static const char *TAG = "UDP_SENDER";

// Event bits for Wi-Fi connection state
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_events;
static int                s_retry_count = 0;
#define WIFI_MAX_RETRY 10

extern QueueHandle_t hid_event_queue; // defined in main.cpp

// Wi-Fi event handler
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGI(TAG, "Retrying Wi-Fi (%d/%d)...", s_retry_count, WIFI_MAX_RETRY);
        } else {
            // Exhausted quick retries; signal the blocker to wait and try again.
            // Reset the counter before signalling so the next esp_wifi_connect()
            // call (issued by wifi_connect_blocking) gets a fresh retry budget.
            s_retry_count = 0;
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Wi-Fi connected, IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

// Block until Wi-Fi is connected; reconnects automatically on drop.
static void wifi_connect_blocking(void)
{
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                                               WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                               pdTRUE, pdFALSE,
                                               pdMS_TO_TICKS(30000));
        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "Wi-Fi ready");
            return;
        }
        // On failure, wait a moment then retry
        ESP_LOGW(TAG, "Wi-Fi connection failed, retrying in 5 s...");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_wifi_connect();
    }
}

// UDP sender task: dequeues HidPacket and sends via UDP to the Windows PC
static void udp_sender_task(void *arg)
{
    wifi_connect_blocking();

    // Resolve target address once
    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(TARGET_PORT);
    if (inet_pton(AF_INET, TARGET_IP, &dest.sin_addr) != 1) {
        ESP_LOGE(TAG, "Invalid TARGET_IP: " TARGET_IP);
        vTaskDelete(NULL);
        return;
    }

    int sock = -1;

    while (1) {
        // Create (or re-create) socket
        if (sock < 0) {
            sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (sock < 0) {
                ESP_LOGE(TAG, "socket() failed: errno %d", errno);
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            // Non-blocking send timeout 100 ms
            struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            ESP_LOGI(TAG, "UDP socket ready, sending to %s:%d", TARGET_IP, TARGET_PORT);
        }

        HidPacket pkt;
        if (xQueueReceive(hid_event_queue, &pkt, pdMS_TO_TICKS(500)) == pdTRUE) {
            int sent = sendto(sock, &pkt, sizeof(pkt), 0,
                              (struct sockaddr *)&dest, sizeof(dest));
            if (sent < 0) {
                ESP_LOGW(TAG, "sendto failed errno=%d, recreating socket", errno);
                close(sock);
                sock = -1;
                // Wait for Wi-Fi to come back
                wifi_connect_blocking();
            }
        }
    }
}

void udp_sender_init(void)
{
    s_wifi_events = xEventGroupCreate();

    // Initialise TCP/IP stack and event loop (if not already done by caller)
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handlers for Wi-Fi + IP
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                         wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                         wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_cfg = {};
    strlcpy((char *)wifi_cfg.sta.ssid,     WIFI_SSID,     sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, WIFI_PASSWORD, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    xTaskCreate(udp_sender_task, "udp_sender", 4096, NULL, 6, NULL);
    ESP_LOGI(TAG, "UDP sender task started");
}
