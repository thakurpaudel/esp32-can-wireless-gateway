#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "mdns.h"

static const char *TAG = "wifi";
static EventGroupHandle_t events;
static const EventBits_t CONNECTED = BIT0;
static int retries;

static void event_handler(void *arg, esp_event_base_t base, int32_t id,
                          void *data) {
  (void)arg;
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    if (retries++ < 10) {
      esp_wifi_connect();
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = data;
    retries = 0;
    ESP_LOGI(TAG, "Dashboard: http://" IPSTR " or http://%s.local",
             IP2STR(&event->ip_info.ip), CONFIG_GATEWAY_HOSTNAME);
    xEventGroupSetBits(events, CONNECTED);
  }
}

static esp_err_t start_fallback_ap(void) {
#if CONFIG_GATEWAY_AP_FALLBACK
  wifi_config_t ap = {0};
  snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s-setup",
           CONFIG_GATEWAY_HOSTNAME);
  snprintf((char *)ap.ap.password, sizeof(ap.ap.password), "%s",
           CONFIG_GATEWAY_AP_PASSWORD);
  ap.ap.ssid_len = strlen((char *)ap.ap.ssid);
  ap.ap.max_connection = 4;
  ap.ap.authmode =
      strlen((char *)ap.ap.password) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG,
                      "Cannot enable fallback AP");
  ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG,
                      "Cannot configure fallback AP");
  ESP_LOGW(TAG,
           "Station unavailable; connect to '%s' and open http://192.168.4.1",
           ap.ap.ssid);
  return ESP_OK;
#else
  return ESP_ERR_TIMEOUT;
#endif
}

esp_err_t wifi_manager_start(void) {
  events = xEventGroupCreate();
  if (!events) {
    return ESP_ERR_NO_MEM;
  }

  ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "Network stack init failed");
  ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG,
                      "Event loop init failed");
  esp_netif_create_default_wifi_sta();
#if CONFIG_GATEWAY_AP_FALLBACK
  esp_netif_create_default_wifi_ap();
#endif

  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "Wi-Fi init failed");
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             event_handler, NULL));

  wifi_config_t station = {0};
  snprintf((char *)station.sta.ssid, sizeof(station.sta.ssid), "%s",
           CONFIG_GATEWAY_WIFI_SSID);
  snprintf((char *)station.sta.password, sizeof(station.sta.password), "%s",
           CONFIG_GATEWAY_WIFI_PASSWORD);
  station.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG,
                      "Cannot set station mode");
  ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &station), TAG,
                      "Cannot configure station");
  ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Cannot start Wi-Fi");

  EventBits_t bits = xEventGroupWaitBits(events, CONNECTED, pdFALSE, pdTRUE,
                                         pdMS_TO_TICKS(15000));
  if (!(bits & CONNECTED)) {
    ESP_RETURN_ON_ERROR(start_fallback_ap(), TAG, "Wi-Fi connection timed out");
  }

  ESP_RETURN_ON_ERROR(mdns_init(), TAG, "mDNS init failed");
  ESP_RETURN_ON_ERROR(mdns_hostname_set(CONFIG_GATEWAY_HOSTNAME), TAG,
                      "mDNS hostname failed");
  mdns_instance_name_set("ESP32 CAN Wireless Gateway");
  mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
  return ESP_OK;
}
