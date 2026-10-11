# ---------------------------------------------------------------------------
# main：应用入口（启动 NVS / 事件循环 / 双网 / 双网管理器 / 本地看板）
# ---------------------------------------------------------------------------
idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES net_eth net_wifi net_manager market_data ui_dashboard ha_mqtt tf_storage web_console app_cfg sensor_history
             nvs_flash esp_event log esp_netif esp_common freertos lwip
)