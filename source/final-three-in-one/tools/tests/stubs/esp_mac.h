#pragma once
#include <stdint.h>

#define ESP_OK 0
int esp_efuse_mac_get_default(uint8_t mac[6]);
