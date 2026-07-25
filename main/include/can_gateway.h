#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "gateway_frame.h"

esp_err_t can_gateway_start(void);
esp_err_t can_gateway_set_bitrate(uint32_t bitrate);
uint32_t can_gateway_get_bitrate(void);
esp_err_t can_gateway_send(const gateway_frame_t *frame);
