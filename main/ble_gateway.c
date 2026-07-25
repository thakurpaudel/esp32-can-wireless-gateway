#include "ble_gateway.h"

#if CONFIG_BT_NIMBLE_ENABLED

#include <string.h>

#include "can_gateway.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble";
static uint16_t connection_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t value_handle;
static uint8_t own_address_type;

static const ble_uuid128_t service_uuid =
    BLE_UUID128_INIT(0x10, 0x9b, 0x4f, 0x20, 0xea, 0x3a, 0x45, 0xa2, 0xb2, 0x45,
                     0x0f, 0x24, 0x98, 0x11, 0x00, 0x11);
static const ble_uuid128_t frame_uuid =
    BLE_UUID128_INIT(0x10, 0x9b, 0x4f, 0x20, 0xea, 0x3a, 0x45, 0xa2, 0xb2, 0x45,
                     0x0f, 0x24, 0x98, 0x11, 0x00, 0x12);
static const ble_uuid128_t command_uuid =
    BLE_UUID128_INIT(0x10, 0x9b, 0x4f, 0x20, 0xea, 0x3a, 0x45, 0xa2, 0xb2, 0x45,
                     0x0f, 0x24, 0x98, 0x11, 0x00, 0x13);

static int characteristic_access(uint16_t conn, uint16_t attr,
                                 struct ble_gatt_access_ctxt *context,
                                 void *arg) {
  (void)conn;
  (void)attr;
  (void)context;
  (void)arg;
  return 0;
}

static int command_access(uint16_t conn, uint16_t attr,
                          struct ble_gatt_access_ctxt *context, void *arg) {
  (void)conn;
  (void)attr;
  (void)arg;

  uint16_t length = OS_MBUF_PKTLEN(context->om);
  uint8_t packet[15];
  if (length == 0 || length > sizeof(packet) ||
      os_mbuf_copydata(context->om, 0, length, packet) != 0) {
    return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
  }

  if (packet[0] == 0x01) {
    if (length < 7) {
      return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    uint8_t dlc = packet[2];
    bool remote = (packet[1] & 0x02) != 0;
    if (dlc > 8 || length != (uint16_t)(7 + (remote ? 0 : dlc))) {
      return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    gateway_frame_t frame = {
        .id = (uint32_t)packet[3] | ((uint32_t)packet[4] << 8) |
              ((uint32_t)packet[5] << 16) | ((uint32_t)packet[6] << 24),
        .dlc = remote ? 0 : dlc,
        .extended = (packet[1] & 0x01) != 0,
        .remote = remote,
        .transmitted = true,
    };
    if (!remote) {
      memcpy(frame.data, packet + 7, dlc);
    }
    return can_gateway_send(&frame) == ESP_OK ? 0 : BLE_ATT_ERR_UNLIKELY;
  }

  if (packet[0] == 0x02 && length == 5) {
    uint32_t bitrate = (uint32_t)packet[1] | ((uint32_t)packet[2] << 8) |
                       ((uint32_t)packet[3] << 16) |
                       ((uint32_t)packet[4] << 24);
    return can_gateway_set_bitrate(bitrate) == ESP_OK ? 0
                                                      : BLE_ATT_ERR_UNLIKELY;
  }

  return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
}

static const struct ble_gatt_svc_def services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &service_uuid.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {
                    .uuid = &frame_uuid.u,
                    .access_cb = characteristic_access,
                    .val_handle = &value_handle,
                    .flags = BLE_GATT_CHR_F_NOTIFY,
                },
                {
                    .uuid = &command_uuid.u,
                    .access_cb = command_access,
                    .flags = BLE_GATT_CHR_F_WRITE,
                },
                {0},
            },
    },
    {0},
};

static void advertise(void);

static int gap_event(struct ble_gap_event *event, void *arg) {
  (void)arg;
  switch (event->type) {
  case BLE_GAP_EVENT_CONNECT:
    if (event->connect.status == 0) {
      connection_handle = event->connect.conn_handle;
      ESP_LOGI(TAG, "BLE client connected");
    } else {
      advertise();
    }
    break;
  case BLE_GAP_EVENT_DISCONNECT:
    connection_handle = BLE_HS_CONN_HANDLE_NONE;
    advertise();
    break;
  case BLE_GAP_EVENT_ADV_COMPLETE:
    advertise();
    break;
  default:
    break;
  }
  return 0;
}

static void advertise(void) {
  struct ble_hs_adv_fields fields = {0};
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  fields.uuids128 = (ble_uuid128_t *)&service_uuid;
  fields.num_uuids128 = 1;
  fields.uuids128_is_complete = 1;
  ble_gap_adv_set_fields(&fields);

  struct ble_hs_adv_fields response = {0};
  response.name = (const uint8_t *)ble_svc_gap_device_name();
  response.name_len = strlen((const char *)response.name);
  response.name_is_complete = 1;
  ble_gap_adv_rsp_set_fields(&response);

  struct ble_gap_adv_params params = {0};
  params.conn_mode = BLE_GAP_CONN_MODE_UND;
  params.disc_mode = BLE_GAP_DISC_MODE_GEN;
  ble_gap_adv_start(own_address_type, NULL, BLE_HS_FOREVER, &params, gap_event,
                    NULL);
}

static void on_sync(void) {
  if (ble_hs_id_infer_auto(0, &own_address_type) != 0) {
    ESP_LOGE(TAG, "Could not determine BLE address");
    return;
  }
  advertise();
}

static void host_task(void *arg) {
  (void)arg;
  nimble_port_run();
  nimble_port_freertos_deinit();
}

esp_err_t ble_gateway_start(void) {
  esp_err_t err = nimble_port_init();
  if (err != ESP_OK) {
    return err;
  }
  ble_svc_gap_init();
  ble_svc_gatt_init();
  ble_svc_gap_device_name_set("ESP32-CAN");
  ble_gatts_count_cfg(services);
  ble_gatts_add_svcs(services);
  ble_hs_cfg.sync_cb = on_sync;
  nimble_port_freertos_init(host_task);
  return ESP_OK;
}

void ble_gateway_publish(const gateway_frame_t *frame) {
  if (connection_handle == BLE_HS_CONN_HANDLE_NONE) {
    return;
  }

  uint32_t timestamp_ms = (uint32_t)(frame->timestamp_us / 1000);
  uint8_t packet[19] = {
      0x81,
      (frame->extended ? 0x01 : 0) | (frame->remote ? 0x02 : 0) |
          (frame->transmitted ? 0x04 : 0),
      frame->dlc,
      (uint8_t)frame->id,
      (uint8_t)(frame->id >> 8),
      (uint8_t)(frame->id >> 16),
      (uint8_t)(frame->id >> 24),
      (uint8_t)timestamp_ms,
      (uint8_t)(timestamp_ms >> 8),
      (uint8_t)(timestamp_ms >> 16),
      (uint8_t)(timestamp_ms >> 24),
  };
  memcpy(packet + 11, frame->data, frame->dlc);
  struct os_mbuf *payload = ble_hs_mbuf_from_flat(packet, sizeof(packet));
  if (payload) {
    ble_gatts_notify_custom(connection_handle, value_handle, payload);
  }
}

#else

esp_err_t ble_gateway_start(void) { return ESP_ERR_NOT_SUPPORTED; }

void ble_gateway_publish(const gateway_frame_t *frame) { (void)frame; }

#endif
