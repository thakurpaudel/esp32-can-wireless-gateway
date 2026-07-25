#include "ble_gateway.h"
#include "can_gateway.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "web_server.h"
#include "wifi_manager.h"

static const char *TAG = "gateway";

void app_main(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);

  ESP_ERROR_CHECK(wifi_manager_start());
  ESP_ERROR_CHECK(web_server_start());

  err = ble_gateway_start();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "BLE unavailable: %s; Wi-Fi gateway remains active",
             esp_err_to_name(err));
  }

  ESP_ERROR_CHECK(can_gateway_start());
  ESP_LOGI(TAG, "CAN gateway started");
}
