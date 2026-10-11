/**
 * @file web_console.h
 * @brief 本地 Web 控制台：状态查看 / GPIO 控制 / TF 日志下载
 *
 * 依赖已初始化的 net_manager / ha_mqtt / tf_storage（调用顺序见 app_main）。
 * 监听默认 netif（ETH 或 WiFi 均可访问）。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 启动 HTTP 服务与 mDNS（幂等） */
esp_err_t web_console_init(void);

#ifdef __cplusplus
}
#endif