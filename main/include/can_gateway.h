#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t can_gateway_start(void);
esp_err_t can_gateway_set_bitrate(uint32_t bitrate);
uint32_t can_gateway_get_bitrate(void);
