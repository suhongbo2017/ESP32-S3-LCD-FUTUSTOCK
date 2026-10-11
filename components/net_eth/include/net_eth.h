/**
 * @file net_eth.h
 * @brief W5500 有线以太网对外接口（esp_eth + SPI，DHCP/静态IP 双模式）
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 以太网运行配置
 *
 * 可运行时传入；若 net_eth_init 参数为 NULL，则使用 Kconfig 默认值
 * （可用 net_eth_get_default_config() 取出后按需修改，例如从 NVS 恢复用户设置）。
 */
typedef struct {
    bool use_static_ip;            /*!< true = 静态 IP；false = DHCP */
    esp_netif_ip_info_t ip_info;   /*!< 静态 IP 信息（地址/掩码/网关），DHCP 时忽略 */
    esp_ip4_addr_t dns_server;     /*!< 静态 IP 时的 DNS，DHCP 时忽略 */
} net_eth_config_t;

/** @brief 从 Kconfig 填充默认配置 */
void net_eth_get_default_config(net_eth_config_t *cfg);

/** @brief 初始化 W5500 有线以太网（SPI 总线 + 驱动 + netif + 事件处理） */
esp_err_t net_eth_init(const net_eth_config_t *cfg);

/** @brief 获取以太网 netif（供 net_manager 切换默认路由使用，可能为 NULL） */
esp_netif_t *net_eth_get_netif(void);

/** @brief 物理链路是否 UP（网线是否插好） */
bool net_eth_is_link_up(void);

/** @brief 是否可上网：链路 UP 且已获得（或已配置）IP */
bool net_eth_is_ready(void);

/** @brief 获取当前 IP 信息 */
esp_err_t net_eth_get_ip_info(esp_netif_ip_info_t *info);

#ifdef __cplusplus
}
#endif