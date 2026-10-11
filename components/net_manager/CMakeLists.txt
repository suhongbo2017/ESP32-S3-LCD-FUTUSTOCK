# ---------------------------------------------------------------------------
# net_manager：双网冗余管理器（以太网优先、WiFi 兜底，自动切换默认路由）
# ---------------------------------------------------------------------------
idf_component_register(
    SRCS "net_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES net_eth net_wifi esp_netif log freertos esp_common
)