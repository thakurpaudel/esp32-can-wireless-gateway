idf_component_register(
    SRCS
        "app_main.c"
        "can_gateway.c"
        "wifi_manager.c"
        "web_server.c"
        "ble_gateway.c"
    INCLUDE_DIRS "include"
    EMBED_FILES "web/index.html"
    REQUIRES
        driver
        esp_event
        esp_http_server
        esp_netif
        esp_wifi
        json
        mdns
        nvs_flash
        bt
)
