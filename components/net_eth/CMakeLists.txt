# ---------------------------------------------------------------------------
# net_eth：W5500 有线以太网（esp_eth + SPI），DHCP/静态IP 双模式
# ---------------------------------------------------------------------------
idf_component_register(
    SRCS "net_eth.c"
    INCLUDE_DIRS "include"
    REQUIRES board esp_eth esp_netif esp_event log driver esp_common
)