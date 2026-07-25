#pragma once

#include "esp_err.h"
#include "gateway_frame.h"

esp_err_t ble_gateway_start(void);
void ble_gateway_publish(const gateway_frame_t *frame);
