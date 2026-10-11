/**
 * @file app_cfg.h
 * @brief 运行时可改的参数（存 NVS，覆盖 Kconfig 默认值）
 *
 * 用途：Web 控制台修改 WiFi / MQTT 参数后，重启即生效，无需重新编译。
 * 默认值来自 Kconfig；NVS 有值则优先。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char wifi_ssid[64];
    char wifi_pass[64];
    char mqtt_host[64];
    int  mqtt_port;
    char topic_notify[64];
    char topic_sensor[64];
} app_cfg_t;

/** 初始化：以 Kconfig 为默认，若 NVS 有值则覆盖 */
esp_err_t app_cfg_init(void);

/** 当前生效配置（只读） */
const app_cfg_t *app_cfg_get(void);

/** 是否有值来自 NVS（false = 全部为 Kconfig 默认） */
bool app_cfg_from_nvs(void);

/** 设置字符串项并写入 NVS。key: wifi_ssid/wifi_pass/mqtt_host/topic_notify/topic_sensor */
esp_err_t app_cfg_set(const char *key, const char *val);

/** 设置整数项并写入 NVS。key: mqtt_port */
esp_err_t app_cfg_set_int(const char *key, int val);

/** 清除 NVS 中的自定义配置（恢复 Kconfig 默认，重启生效） */
esp_err_t app_cfg_reset(void);

#ifdef __cplusplus
}
#endif