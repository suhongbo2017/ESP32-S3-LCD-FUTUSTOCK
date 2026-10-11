idf_component_register(
    SRCS "market_data.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client esp-tls json esp_timer esp_netif app_cfg log
)
