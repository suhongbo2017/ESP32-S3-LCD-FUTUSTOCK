/**
 * @file ha_mqtt.h
 * @brief Home Assistant MQTT 接入对外接口（esp-mqtt 封装）
 *
 * 职责：
 *  - 连接本地 broker（Kconfig：HA_MQTT_BROKER_HOST/PORT/...，匿名可用）
 *  - 订阅 ha/esp32gw/notify，解析 JSON 通知 → 队列投递给 UI（不阻塞 MQTT 回调）
 *  - 周期发布 ha/esp32gw/state（uplink/ip/uptime/online + 最近通知）
 *  - MQTT Discovery：向 homeassistant/... 发布配置，HA 自动生成实体
 *  - 断线重连由 esp-mqtt 自动处理；连接使用默认 netif（ETH 优先，net_manager 已切换）
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 通知级别（对应 JSON level 字段） */
typedef enum {
    HA_NOTIFY_INFO = 0,   /*!< info */
    HA_NOTIFY_WARN,       /*!< warn */
    HA_NOTIFY_ERROR,      /*!< error */
} ha_notify_level_t;

/** HA 推送通知（JSON: {"title","message","level"}） */
typedef struct {
    char title[24];          /*!< 标题（短） */
    char message[96];        /*!< 正文 */
    ha_notify_level_t level; /*!< 级别 */
} ha_notify_t;

/** 通知处理器：由 ha_mqtt 内部任务调用（非 esp-mqtt 回调上下文），可安全操作 LVGL */
typedef void (*ha_notify_handler_t)(const ha_notify_t *notify, void *user_ctx);

/** 传感器数据（JSON: {"temp","hum","rssi","uptime"}） */
typedef struct {
    float   temp;      /*!< 温度 °C */
    float   hum;       /*!< 湿度 % */
    int     rssi;      /*!< 传感器 WiFi RSSI */
    int64_t rx_us;     /*!< 本板接收时刻（esp_timer，微秒）；0 = 从未收到 */
} ha_sensor_t;

/** 传感器处理器：由 ha_mqtt 内部任务调用（非 MQTT 回调），可安全操作 LVGL */
typedef void (*ha_sensor_handler_t)(const ha_sensor_t *sensor, void *user_ctx);

/**
 * @brief 启动 Home Assistant MQTT 接入
 *
 * 创建 esp-mqtt 客户端、通知队列与心跳/投递任务。
 * 建议在 net_manager_init() 之后调用（连接走默认 netif）。
 */
esp_err_t ha_mqtt_init(void);

/**
 * @brief 注册通知处理器（看板显示用，最多 2 个）
 *
 * 处理器在 ha_mqtt 的投递任务上下文中执行，内部可用 lvgl_port_lock 操作 UI。
 */
esp_err_t ha_mqtt_register_notify_handler(ha_notify_handler_t cb, void *user_ctx);

/**
 * @brief 注册传感器数据处理器（看板显示温湿度用，最多 2 个）
 *
 * 处理器在 ha_mqtt 的投递任务上下文中执行，内部可用 lvgl_port_lock 操作 UI。
 */
esp_err_t ha_mqtt_register_sensor_handler(ha_sensor_handler_t cb, void *user_ctx);

/** @brief 取最近一次传感器数据（从未收到时 rx_us==0） */
void ha_mqtt_get_last_sensor(ha_sensor_t *out);

/**
 * @brief 发布一条告警到 HA（主题 CONFIG_HA_MQTT_TOPIC_ALERT）
 *
 * JSON: {"type":"sensor_offline","message":"...","uptime":123}
 * 供 HA 用 MQTT 触发器自动化转发（如手机推送）。未连接时返回错误。
 */
esp_err_t ha_mqtt_publish_alert(const char *type, const char *message);

/** @brief MQTT 当前是否已连接 broker */
bool ha_mqtt_is_connected(void);

#ifdef __cplusplus
}
#endif