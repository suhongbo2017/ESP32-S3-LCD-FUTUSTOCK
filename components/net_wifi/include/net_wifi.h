/**
 * @file net_wifi.h
 * @brief WiFi Station 初始化对外接口（默认 DHCP，可选静态 IP）
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 初始化 WiFi Station，联网参数来自 Kconfig（NET_WIFI_*，含可选静态 IP） */
esp_err_t net_wifi_init(void);

/** @brief 获取 STA netif（供 net_manager 使用，可能为 NULL） */
esp_netif_t *net_wifi_get_netif(void);

/** @brief 是否可上网：已关联 AP 且已获得 IP（DHCP） */
bool net_wifi_is_connected(void);

/** @brief 获取当前 IP 信息 */
esp_err_t net_wifi_get_ip_info(esp_netif_ip_info_t *info);

#ifdef __cplusplus
}
#endif