#include "pairing_store.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG      = "PAIR_STORE";
static const char *NVS_NS   = "hid_pair";    // NVS namespace
static const char *NVS_KEY  = "paired_devs"; // blob key

// NVS-stored blob: an array of PairedDevice structs prefixed with a count byte.
typedef struct {
    uint8_t      count;
    PairedDevice devs[MAX_PAIRED_DEVICES];
} __attribute__((packed)) PairBlob;

esp_err_t pairing_store_save(const uint8_t bda[6], PairingProtocol protocol)
{
    nvs_handle_t handle;
    esp_err_t    err = nvs_open(NVS_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    // Load existing blob
    PairBlob blob = {};
    size_t   blob_size = sizeof(blob);
    err = nvs_get_blob(handle, NVS_KEY, &blob, &blob_size);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_get_blob error: %s", esp_err_to_name(err));
        blob.count = 0;
    }
    if (blob.count > MAX_PAIRED_DEVICES) blob.count = 0; // corruption guard

    // Check if BDA already exists; if so, update protocol and return
    for (int i = 0; i < blob.count; i++) {
        if (memcmp(blob.devs[i].bda, bda, 6) == 0) {
            blob.devs[i].protocol = protocol;
            goto write_back;
        }
    }

    // Add new entry (evict oldest if full)
    if (blob.count >= MAX_PAIRED_DEVICES) {
        // Shift entries left, dropping the oldest
        memmove(&blob.devs[0], &blob.devs[1],
                (MAX_PAIRED_DEVICES - 1) * sizeof(PairedDevice));
        blob.count = MAX_PAIRED_DEVICES - 1;
    }
    memcpy(blob.devs[blob.count].bda, bda, 6);
    blob.devs[blob.count].protocol = protocol;
    blob.count++;

write_back:
    err = nvs_set_blob(handle, NVS_KEY, &blob, sizeof(blob));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved pairing for " MACSTR, MAC2STR(bda));
    } else {
        ESP_LOGE(TAG, "Failed to write NVS: %s", esp_err_to_name(err));
    }
    return err;
}

int pairing_store_load(PairedDevice *out, int max_count)
{
    nvs_handle_t handle;
    esp_err_t    err = nvs_open(NVS_NS, NVS_READONLY, &handle);
    if (err != ESP_OK) return 0;

    PairBlob blob = {};
    size_t   blob_size = sizeof(blob);
    err = nvs_get_blob(handle, NVS_KEY, &blob, &blob_size);
    nvs_close(handle);
    if (err != ESP_OK) return 0;
    if (blob.count > MAX_PAIRED_DEVICES) return 0;

    int n = (blob.count < max_count) ? blob.count : max_count;
    memcpy(out, blob.devs, n * sizeof(PairedDevice));
    return n;
}

esp_err_t pairing_store_clear(void)
{
    nvs_handle_t handle;
    esp_err_t    err = nvs_open(NVS_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(handle);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "Pairing store cleared");
    return err;
}
