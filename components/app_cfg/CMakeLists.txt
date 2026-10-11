# ---------------------------------------------------------------------------
# app_cfg：运行时可改参数（NVS），Web 控制台写入、net_wifi/ha_mqtt 读取
# ---------------------------------------------------------------------------
idf_component_register(
    SRCS "app_cfg.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash log esp_common
)