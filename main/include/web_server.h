#pragma once

#include "esp_err.h"
#include "gateway_frame.h"

esp_err_t web_server_start(void);
void web_server_publish(const gateway_frame_t *frame);
