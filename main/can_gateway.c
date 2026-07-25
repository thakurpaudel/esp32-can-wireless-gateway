#include "can_gateway.h"

#include <stdio.h>

#include "ble_gateway.h"
#include "driver/twai.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "gateway_frame.h"
#include "web_server.h"

static const char *TAG = "can";
static QueueHandle_t bitrate_queue;
static uint32_t active_bitrate;

size_t gateway_frame_to_json(const gateway_frame_t *frame, char *buffer,
                             size_t size) {
  int used = snprintf(buffer, size,
                      "{\"timestamp_us\":%lld,\"id\":%lu,\"id_hex\":\"%s%lX\","
                      "\"extended\":%s,\"remote\":%s,\"dlc\":%u,\"data\":[",
                      (long long)frame->timestamp_us, (unsigned long)frame->id,
                      frame->extended ? "0x" : "0x", (unsigned long)frame->id,
                      frame->extended ? "true" : "false",
                      frame->remote ? "true" : "false", frame->dlc);
  if (used < 0 || (size_t)used >= size) {
    return 0;
  }

  for (uint8_t i = 0; i < frame->dlc; ++i) {
    int written = snprintf(buffer + used, size - (size_t)used, "%s%u",
                           i ? "," : "", frame->data[i]);
    if (written < 0 || (size_t)written >= size - (size_t)used) {
      return 0;
    }
    used += written;
  }

  int written = snprintf(buffer + used, size - (size_t)used, "]}");
  if (written < 0 || (size_t)written >= size - (size_t)used) {
    return 0;
  }
  return (size_t)(used + written);
}

static uint32_t configured_bitrate(void) {
#if CONFIG_CAN_BITRATE_125K
  return 125000;
#elif CONFIG_CAN_BITRATE_250K
  return 250000;
#elif CONFIG_CAN_BITRATE_1M
  return 1000000;
#else
  return 500000;
#endif
}

static twai_timing_config_t timing_config(uint32_t bitrate) {
  switch (bitrate) {
  case 125000:
    return (twai_timing_config_t)TWAI_TIMING_CONFIG_125KBITS();
  case 250000:
    return (twai_timing_config_t)TWAI_TIMING_CONFIG_250KBITS();
  case 1000000:
    return (twai_timing_config_t)TWAI_TIMING_CONFIG_1MBITS();
  default:
    return (twai_timing_config_t)TWAI_TIMING_CONFIG_500KBITS();
  }
}

static esp_err_t install_driver(uint32_t bitrate) {
  twai_general_config_t general = TWAI_GENERAL_CONFIG_DEFAULT(
      CONFIG_CAN_TX_GPIO, CONFIG_CAN_RX_GPIO, TWAI_MODE_NORMAL);
  general.rx_queue_len = 64;
  general.tx_queue_len = 8;

  twai_timing_config_t timing = timing_config(bitrate);
  twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  ESP_RETURN_ON_ERROR(twai_driver_install(&general, &timing, &filter), TAG,
                      "TWAI driver installation failed");
  esp_err_t err = twai_start();
  if (err != ESP_OK) {
    twai_driver_uninstall();
    return err;
  }
  active_bitrate = bitrate;
  return ESP_OK;
}

static void apply_requested_bitrate(void) {
  uint32_t requested;
  if (xQueueReceive(bitrate_queue, &requested, 0) != pdTRUE ||
      requested == active_bitrate) {
    return;
  }

  const uint32_t previous = active_bitrate;
  ESP_LOGI(TAG, "Changing bitrate from %lu to %lu bit/s",
           (unsigned long)previous, (unsigned long)requested);
  twai_stop();
  twai_driver_uninstall();

  esp_err_t err = install_driver(requested);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Bitrate change failed: %s; restoring %lu bit/s",
             esp_err_to_name(err), (unsigned long)previous);
    if (install_driver(previous) != ESP_OK) {
      ESP_LOGE(TAG, "Could not restore TWAI driver");
    }
  }
}

static void receive_task(void *arg) {
  (void)arg;
  twai_message_t message;

  while (true) {
    apply_requested_bitrate();
    esp_err_t err = twai_receive(&message, pdMS_TO_TICKS(100));
    if (err == ESP_ERR_TIMEOUT) {
      continue;
    }
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "Receive error: %s", esp_err_to_name(err));
      continue;
    }

    gateway_frame_t frame = {
        .id = message.identifier,
        .dlc = message.data_length_code > 8 ? 8 : message.data_length_code,
        .extended = message.extd,
        .remote = message.rtr,
        .timestamp_us = esp_timer_get_time(),
    };
    for (uint8_t i = 0; i < frame.dlc; ++i) {
      frame.data[i] = message.data[i];
    }

    web_server_publish(&frame);
    ble_gateway_publish(&frame);
  }
}

esp_err_t can_gateway_start(void) {
  bitrate_queue = xQueueCreate(1, sizeof(uint32_t));
  if (!bitrate_queue) {
    return ESP_ERR_NO_MEM;
  }
  ESP_RETURN_ON_ERROR(install_driver(configured_bitrate()), TAG,
                      "TWAI start failed");

  if (xTaskCreate(receive_task, "can_rx", 4096, NULL, 8, NULL) != pdPASS) {
    twai_stop();
    twai_driver_uninstall();
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(TAG, "TWAI RX GPIO=%d TX GPIO=%d bitrate=%lu", CONFIG_CAN_RX_GPIO,
           CONFIG_CAN_TX_GPIO, (unsigned long)active_bitrate);
  return ESP_OK;
}

esp_err_t can_gateway_set_bitrate(uint32_t bitrate) {
  if (bitrate != 125000 && bitrate != 250000 && bitrate != 500000 &&
      bitrate != 1000000) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!bitrate_queue) {
    return ESP_ERR_INVALID_STATE;
  }
  return xQueueOverwrite(bitrate_queue, &bitrate) == pdTRUE ? ESP_OK : ESP_FAIL;
}

uint32_t can_gateway_get_bitrate(void) { return active_bitrate; }
