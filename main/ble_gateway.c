#include "ble_gateway.h"

#if CONFIG_BT_NIMBLE_ENABLED

#include <string.h>

#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble";
static uint16_t connection_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t value_handle;
static uint8_t own_address_type;

static const ble_uuid128_t service_uuid =
    BLE_UUID128_INIT(0x10, 0x9b, 0x4f, 0x20, 0xea, 0x3a, 0x45, 0xa2, 0xb2, 0x45,
                     0x0f, 0x24, 0x98, 0x11, 0x00, 0x01);
static const ble_uuid128_t frame_uuid =
    BLE_UUID128_INIT(0x10, 0x9b, 0x4f, 0x20, 0xea, 0x3a, 0x45, 0xa2, 0xb2, 0x45,
                     0x0f, 0x24, 0x98, 0x11, 0x00, 0x02);

static int characteristic_access(uint16_t conn, uint16_t attr,
                                 struct ble_gatt_access_ctxt *context,
                                 void *arg) {
  (void)conn;
  (void)attr;
  (void)context;
  (void)arg;
  return 0;
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
  fields.name = (const uint8_t *)ble_svc_gap_device_name();
  fields.name_len = strlen((const char *)fields.name);
  fields.name_is_complete = 1;
  ble_gap_adv_set_fields(&fields);

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

  char json[192];
  size_t length = gateway_frame_to_json(frame, json, sizeof(json));
  struct os_mbuf *payload = ble_hs_mbuf_from_flat(json, length);
  if (payload) {
    ble_gatts_notify_custom(connection_handle, value_handle, payload);
  }
}

#else

esp_err_t ble_gateway_start(void) { return ESP_ERR_NOT_SUPPORTED; }

void ble_gateway_publish(const gateway_frame_t *frame) { (void)frame; }

#endif
