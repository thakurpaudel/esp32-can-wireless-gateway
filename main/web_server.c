#include "web_server.h"

#include <string.h>

#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "web";
static httpd_handle_t server;

extern const unsigned char
    index_html_start[] asm("_binary_web_index_html_start");
extern const unsigned char index_html_end[] asm("_binary_web_index_html_end");

static esp_err_t index_handler(httpd_req_t *request) {
  httpd_resp_set_type(request, "text/html");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, (const char *)index_html_start,
                         index_html_end - index_html_start);
}

static esp_err_t ws_handler(httpd_req_t *request) {
  if (request->method == HTTP_GET) {
    return ESP_OK;
  }

  httpd_ws_frame_t frame = {0};
  frame.type = HTTPD_WS_TYPE_TEXT;
  esp_err_t err = httpd_ws_recv_frame(request, &frame, 0);
  if (err != ESP_OK || frame.len > 128) {
    return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
  }
  if (frame.len) {
    uint8_t payload[129];
    frame.payload = payload;
    err = httpd_ws_recv_frame(request, &frame, frame.len);
  }
  return err;
}

esp_err_t web_server_start(void) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_open_sockets = 7;
  config.lru_purge_enable = true;

  ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "HTTP server failed");

  httpd_uri_t index = {
      .uri = "/",
      .method = HTTP_GET,
      .handler = index_handler,
  };
  httpd_uri_t ws = {
      .uri = "/ws",
      .method = HTTP_GET,
      .handler = ws_handler,
      .is_websocket = true,
  };
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &index));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ws));
  return ESP_OK;
}

void web_server_publish(const gateway_frame_t *frame) {
  if (!server) {
    return;
  }

  char json[192];
  size_t length = gateway_frame_to_json(frame, json, sizeof(json));
  if (!length) {
    return;
  }

  size_t count = 8;
  int clients[8];
  if (httpd_get_client_list(server, &count, clients) != ESP_OK) {
    return;
  }

  httpd_ws_frame_t packet = {
      .final = true,
      .fragmented = false,
      .type = HTTPD_WS_TYPE_TEXT,
      .payload = (uint8_t *)json,
      .len = length,
  };
  for (size_t i = 0; i < count; ++i) {
    if (httpd_ws_get_fd_info(server, clients[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
      httpd_ws_send_frame_async(server, clients[i], &packet);
    }
  }
}
