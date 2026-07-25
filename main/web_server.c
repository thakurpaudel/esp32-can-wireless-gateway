#include "web_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "can_gateway.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "web";
static httpd_handle_t server;

extern const unsigned char index_html_start[] asm("_binary_index_html_start");
extern const unsigned char index_html_end[] asm("_binary_index_html_end");

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

static esp_err_t status_handler(httpd_req_t *request) {
  char response[64];
  int length = snprintf(response, sizeof(response), "{\"bitrate\":%lu}",
                        (unsigned long)can_gateway_get_bitrate());
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, response, length);
}

static esp_err_t bitrate_handler(httpd_req_t *request) {
  if (request->content_len <= 0 || request->content_len >= 64) {
    httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid request");
    return ESP_OK;
  }

  char payload[64];
  int received = httpd_req_recv(request, payload, (size_t)request->content_len);
  if (received <= 0) {
    httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Missing request body");
    return ESP_OK;
  }
  payload[received] = '\0';

  char *value = strstr(payload, "bitrate");
  value = value ? strchr(value, ':') : NULL;
  if (!value) {
    httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Missing bitrate");
    return ESP_OK;
  }

  uint32_t bitrate = (uint32_t)strtoul(value + 1, NULL, 10);
  esp_err_t err = can_gateway_set_bitrate(bitrate);
  if (err != ESP_OK) {
    httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                        "Use 125000, 250000, 500000, or 1000000");
    return ESP_OK;
  }

  char response[72];
  int length = snprintf(response, sizeof(response),
                        "{\"accepted\":true,\"requested_bitrate\":%lu}",
                        (unsigned long)bitrate);
  httpd_resp_set_status(request, "202 Accepted");
  httpd_resp_set_type(request, "application/json");
  return httpd_resp_send(request, response, length);
}

static esp_err_t send_handler(httpd_req_t *request) {
  if (request->content_len <= 0 || request->content_len >= 256) {
    httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid request");
    return ESP_OK;
  }

  char payload[256];
  size_t total = 0;
  while (total < (size_t)request->content_len) {
    int received = httpd_req_recv(request, payload + total,
                                  (size_t)request->content_len - total);
    if (received <= 0) {
      httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                          "Incomplete request body");
      return ESP_OK;
    }
    total += (size_t)received;
  }
  payload[total] = '\0';

  cJSON *root = cJSON_Parse(payload);
  cJSON *id = root ? cJSON_GetObjectItemCaseSensitive(root, "id") : NULL;
  cJSON *extended =
      root ? cJSON_GetObjectItemCaseSensitive(root, "extended") : NULL;
  cJSON *remote =
      root ? cJSON_GetObjectItemCaseSensitive(root, "remote") : NULL;
  cJSON *data = root ? cJSON_GetObjectItemCaseSensitive(root, "data") : NULL;

  if (!cJSON_IsNumber(id) || !cJSON_IsBool(extended) || !cJSON_IsBool(remote) ||
      !cJSON_IsArray(data) || cJSON_GetArraySize(data) > 8) {
    cJSON_Delete(root);
    httpd_resp_send_err(
        request, HTTPD_400_BAD_REQUEST,
        "Expected id, extended, remote, and up to 8 data bytes");
    return ESP_OK;
  }

  bool is_extended = cJSON_IsTrue(extended);
  double id_value = id->valuedouble;
  uint32_t maximum_id = is_extended ? 0x1FFFFFFF : 0x7FF;
  bool valid =
      id_value >= 0 && id_value <= maximum_id && id_value == (uint32_t)id_value;

  gateway_frame_t frame = {
      .id = valid ? (uint32_t)id_value : 0,
      .dlc = (uint8_t)cJSON_GetArraySize(data),
      .extended = is_extended,
      .remote = cJSON_IsTrue(remote),
      .transmitted = true,
  };
  if (frame.remote) {
    frame.dlc = 0;
  }

  for (uint8_t i = 0; valid && i < frame.dlc; ++i) {
    cJSON *byte = cJSON_GetArrayItem(data, i);
    valid = cJSON_IsNumber(byte) && byte->valuedouble >= 0 &&
            byte->valuedouble <= 255 &&
            byte->valuedouble == (uint8_t)byte->valuedouble;
    if (valid) {
      frame.data[i] = (uint8_t)byte->valuedouble;
    }
  }
  cJSON_Delete(root);

  esp_err_t err = valid ? can_gateway_send(&frame) : ESP_ERR_INVALID_ARG;
  if (err != ESP_OK) {
    const char *message = err == ESP_ERR_NO_MEM
                              ? "Transmit queue is full"
                              : "Invalid CAN ID or data bytes";
    httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, message);
    return ESP_OK;
  }

  httpd_resp_set_status(request, "202 Accepted");
  httpd_resp_set_type(request, "application/json");
  return httpd_resp_sendstr(request, "{\"accepted\":true}");
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
  httpd_uri_t status = {
      .uri = "/api/status",
      .method = HTTP_GET,
      .handler = status_handler,
  };
  httpd_uri_t bitrate = {
      .uri = "/api/bitrate",
      .method = HTTP_POST,
      .handler = bitrate_handler,
  };
  httpd_uri_t send = {
      .uri = "/api/send",
      .method = HTTP_POST,
      .handler = send_handler,
  };
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &index));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ws));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &bitrate));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &send));
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
