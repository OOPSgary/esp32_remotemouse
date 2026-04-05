#pragma once
#include <stdint.h>
#include "esp_bt_defs.h"

// Maximum number of paired devices stored in NVS
#define MAX_PAIRED_DEVICES 4

// Transport protocol used when a device was paired
typedef enum {
    PAIRING_PROTO_BLE     = 0x01,
    PAIRING_PROTO_CLASSIC = 0x02,
} PairingProtocol;

// A single pairing record
typedef struct {
    esp_bd_addr_t    bda;       // 6-byte Bluetooth Device Address
    PairingProtocol  protocol;
} PairedDevice;

// Save (or update) a BDA + protocol record in NVS.
// Returns ESP_OK on success.
esp_err_t pairing_store_save(const uint8_t bda[6], PairingProtocol protocol);

// Load all stored pairing records into the provided array.
// Returns the number of records actually loaded (0 if NVS is empty or on error).
int pairing_store_load(PairedDevice *out, int max_count);

// Erase all pairing records from NVS.
esp_err_t pairing_store_clear(void);
