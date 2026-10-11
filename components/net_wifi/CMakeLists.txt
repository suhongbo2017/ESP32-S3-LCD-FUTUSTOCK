# ---------------------------------------------------------------------------
# net_wifi：WiFi Station 初始化（DHCP，SSID/密码来自 Kconfig）
# ---------------------------------------------------------------------------
idf_component_register(
    SRCS "net_wifi.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event log esp_common app_cfg
)