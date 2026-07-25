#pragma once

#include "esp_err.h"
#include "gateway_frame.h"

esp_err_t web_server_start(void);
void web_server_publish(const gateway_frame_t *frame);
void web_server_publish_tx_status(uint32_t id, bool success,
                                  const char *message);
