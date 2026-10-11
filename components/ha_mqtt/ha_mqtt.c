/**
 * @file ha_mqtt.c
 * @brief Home Assistant MQTT 接入实现（esp-mqtt 封装）
 *
 * 线程模型：
 *  - esp-mqtt 回调（MQTT_EVENT_DATA）只做「解析 JSON + 入队（非阻塞）」，
 *    【绝不在回调里操作 LVGL / 阻塞】，避免拖死 MQTT 任务。
 *  - 独立投递任务从队列取出通知 → 调用注册的 UI handler（此时才碰 LVGL，
 *    由 UI 侧持 lvgl_port_lock 保护）。
 *  - 心跳任务周期发布状态 JSON；收到通知后立即补发一次，保证 HA 的
 *    notify 实体及时更新。
 *
 * 数据来源：net_manager_get_status()（ETH 优先，WiFi 兜底已由 net_manager 负责）。
 */
#include "ha_mqtt.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "tf_storage.h"          /* TF 卡历史记录（传感器/通知落盘） */
#include "app_cfg.h"             /* NVS 运行时可改参数 */
#include "sensor_history.h"      /* 温湿度历史（Web 曲线） */
#include "net_manager.h"

static const char *TAG = "ha_mqtt";

#define HA_NOTIFY_QUEUE_LEN   4
#define HA_NOTIFY_TASK_STACK  4096
#define HA_NOTIFY_TASK_PRIO   6
#define HA_HEARTBEAT_STACK    4096
#define HA_HEARTBEAT_PRIO     4
#define HA_MAX_HANDLERS       2
#define HA_SENSOR_QUEUE_LEN   4
#define HA_SENSOR_TASK_STACK  4096
#define HA_SENSOR_TASK_PRIO   5

/* ---------- 内部状态 ---------- */
static esp_mqtt_client_handle_t s_client = NULL;
static QueueHandle_t s_notify_q = NULL;
static TaskHandle_t s_notify_task = NULL;
static TaskHandle_t s_heartbeat_task = NULL;
static volatile bool s_connected = false;
static ha_notify_handler_t s_handlers[HA_MAX_HANDLERS];
static void *s_handler_ctx[HA_MAX_HANDLERS];
static ha_notify_t s_last_notify;           /*!< 最近一条通知（并入 state 发布） */
static QueueHandle_t s_sensor_q = NULL;
static TaskHandle_t  s_sensor_task = NULL;
static ha_sensor_handler_t s_sensor_handlers[HA_MAX_HANDLERS];
static void *s_sensor_ctx[HA_MAX_HANDLERS];
static ha_sensor_t s_last_sensor;           /*!< 最近一次传感器数据（rx_us==0 表示未收到过） */

/* 前向声明（MQTT 事件回调位于定义之前） */
static void parse_sensor(const char *data, size_t len, ha_sensor_t *out);

/* ---------- 通知解析 ---------- */

/** 解析 notify JSON（在 MQTT 回调中调用，保持轻量） */
static void parse_notify(const char *data, size_t len, ha_notify_t *out)
{
    memset(out, 0, sizeof(*out));
    out->level = HA_NOTIFY_INFO;

    /* 拷贝到可解析缓冲（data 不保证以 \0 结尾） */
    char *buf = malloc(len + 1);
    if (buf == NULL) {
        /* 退路：原文截断进 message */
        snprintf(out->message, sizeof(out->message), "%.63s", data);
        return;
    }
    memcpy(buf, data, len);
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        snprintf(out->message, sizeof(out->message), "%.63s", data);
        return;
    }

    cJSON *t = cJSON_GetObjectItem(root, "title");
    cJSON *m = cJSON_GetObjectItem(root, "message");
    cJSON *l = cJSON_GetObjectItem(root, "level");
    if (cJSON_IsString(t)) {
        snprintf(out->title, sizeof(out->title), "%s", t->valuestring);
    }
    if (cJSON_IsString(m)) {
        snprintf(out->message, sizeof(out->message), "%s", m->valuestring);
    }
    if (cJSON_IsString(l)) {
        if (strcmp(l->valuestring, "warn") == 0) {
            out->level = HA_NOTIFY_WARN;
        } else if (strcmp(l->valuestring, "error") == 0) {
            out->level = HA_NOTIFY_ERROR;
        }
    }
    cJSON_Delete(root);
}

/* ---------- 状态发布 ---------- */

/** 组装并发布 ha/esp32gw/state（JSON），retain 按需 */
static void publish_state(int retain)
{
    if (s_client == NULL || !s_connected) {
        return;
    }
    net_manager_status_t st = net_manager_get_status();
    uint64_t sec = esp_timer_get_time() / 1000000ULL;

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return;
    }
    cJSON_AddBoolToObject(root, "online", true);
    cJSON_AddStringToObject(root, "uplink", net_manager_backend_name(st.active));
    cJSON_AddStringToObject(root, "ip", st.active_ip);
    cJSON_AddNumberToObject(root, "uptime", (double)sec);
    cJSON_AddStringToObject(root, "notify_title", s_last_notify.title);
    cJSON_AddStringToObject(root, "notify_message", s_last_notify.message);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return;
    }
    esp_mqtt_client_publish(s_client, CONFIG_HA_MQTT_TOPIC_STATE,
                            payload, (int)strlen(payload), 0, retain);
    free(payload);
}

/* ---------- MQTT Discovery ---------- */

#if defined(CONFIG_HA_MQTT_DISCOVERY_ENABLE)
/** 发布一个 sensor 的 discovery 配置 */
static void discovery_sensor(const char *obj_id, const char *name,
                             const char *value_template)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return;
    }
    cJSON_AddStringToObject(root, "name", name);
    cJSON_AddStringToObject(root, "object_id", obj_id);
    cJSON_AddStringToObject(root, "unique_id", obj_id);
    cJSON_AddStringToObject(root, "state_topic", CONFIG_HA_MQTT_TOPIC_STATE);
    cJSON_AddStringToObject(root, "value_template", value_template);
    /* 在线状态：state JSON 的 online 字段 */
    cJSON_AddStringToObject(root, "availability_topic", CONFIG_HA_MQTT_TOPIC_STATE);
    cJSON_AddStringToObject(root, "availability_template",
                            "{{ 'online' if value_json.online|default(false) else 'offline' }}");
    cJSON *dev = cJSON_AddObjectToObject(root, "device");
    cJSON_AddStringToObject(dev, "identifiers", "esp32gw_device");
    cJSON_AddStringToObject(dev, "name", CONFIG_HA_MQTT_DEVICE_NAME);
    cJSON_AddStringToObject(dev, "model", "esp32s3_gateway");
    cJSON_AddStringToObject(dev, "manufacturer", "DIY");

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return;
    }

    char topic[96];
    snprintf(topic, sizeof(topic), "homeassistant/sensor/esp32gw_%s/config", obj_id);
    esp_mqtt_client_publish(s_client, topic, payload, (int)strlen(payload), 0, 1);
    free(payload);
    ESP_LOGI(TAG, "Discovery 已发布：%s", obj_id);
}

/** 连接成功后发布所有 discovery（HA 幂等，失败重连后会重发） */
static void publish_discovery(void)
{
    discovery_sensor("uplink", "GW Uplink", "{{ value_json.uplink }}");
    discovery_sensor("ip", "GW IP", "{{ value_json.ip }}");
    discovery_sensor("uptime", "GW Uptime", "{{ value_json.uptime }}");
    discovery_sensor("notify", "GW Notify", "{{ value_json.notify_title }}");
}
#endif /* CONFIG_HA_MQTT_DISCOVERY_ENABLE */

/* ---------- esp-mqtt 事件回调（轻量：只解析 + 入队） ---------- */

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)handler_args;
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "已连接 broker：%s:%d（client=%s）",
                 (app_cfg_get()->mqtt_host), (app_cfg_get()->mqtt_port),
                 CONFIG_HA_MQTT_CLIENT_ID[0] ? CONFIG_HA_MQTT_CLIENT_ID : "(auto)");
        /* 订阅通知主题（QoS1：至少一次，防丢通知） */
        esp_mqtt_client_subscribe_single(s_client, (app_cfg_get()->topic_notify), 1);
        ESP_LOGI(TAG, "已订阅：%s", (app_cfg_get()->topic_notify));
        /* 订阅传感器数据主题（QoS1） */
        esp_mqtt_client_subscribe_single(s_client, (app_cfg_get()->topic_sensor), 1);
        ESP_LOGI(TAG, "已订阅：%s", (app_cfg_get()->topic_sensor));
#if defined(CONFIG_HA_MQTT_DISCOVERY_ENABLE)
        publish_discovery();
#endif
        publish_state(1);   /* retain 上线状态，HA 重启后也能确认在线 */
        break;

    case MQTT_EVENT_DATA:
        /* 只处理通知主题；同时忽略分片（current_data_offset>0） */
        if (ev->data_len > 0 && ev->current_data_offset == 0 &&
            strncmp(ev->topic, (app_cfg_get()->topic_notify), ev->topic_len) == 0) {
            ha_notify_t n;
            parse_notify(ev->data, ev->data_len, &n);
            s_last_notify = n;                       /* 缓存最近通知 */
            if (xQueueSend(s_notify_q, &n, pdMS_TO_TICKS(10)) != pdTRUE) {
                ESP_LOGW(TAG, "通知队列已满，丢弃一条显示");
            } else {
                ESP_LOGI(TAG, "收到通知：[%s] %s", n.title, n.message);
                tf_storage_log("notify", "title=%s,msg=%s,lvl=%d",
                               n.title, n.message, (int)n.level);
            }
            publish_state(0);                        /* 立即补发，更新 HA notify 实体 */
        } else if (ev->data_len > 0 && ev->current_data_offset == 0 &&
                   strncmp(ev->topic, (app_cfg_get()->topic_sensor), ev->topic_len) == 0) {
            ha_sensor_t sn = s_last_sensor;          /* 缺失字段保留旧值 */
            parse_sensor(ev->data, ev->data_len, &sn);
            s_last_sensor = sn;
            if (xQueueSend(s_sensor_q, &sn, pdMS_TO_TICKS(10)) != pdTRUE) {
                ESP_LOGW(TAG, "传感器队列已满，丢弃一条");
            } else {
                ESP_LOGI(TAG, "收到传感器数据：%.1f°C / %.1f%% / rssi %d（TF 已写 %lu 条）",
                         sn.temp, sn.hum, sn.rssi,
                         (unsigned long)tf_storage_written_count());
                tf_storage_log("sensor", "temp=%.2f,hum=%.2f,rssi=%d",
                               sn.temp, sn.hum, sn.rssi);
                sensor_history_add(sn.temp, sn.hum);   /* 供 Web 曲线图 */
            }
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "MQTT 已断开（esp-mqtt 将自动重连）");
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "MQTT 错误（esp-mqtt 将自动重连）");
        break;

    default:
        break;
    }
}

/* ---------- 通知投递任务（UI 操作发生在该任务上下文，而非 MQTT 回调） ---------- */

static void notify_task(void *arg)
{
    (void)arg;
    ha_notify_t n;
    for (;;) {
        if (xQueueReceive(s_notify_q, &n, portMAX_DELAY) == pdTRUE) {
            for (int i = 0; i < HA_MAX_HANDLERS; i++) {
                if (s_handlers[i] != NULL) {
                    s_handlers[i](&n, s_handler_ctx[i]);
                }
            }
        }
    }
}

/* ---------- 传感器数据：解析 / 投递 ---------- */

/** 解析传感器 JSON（MQTT 回调中调用，保持轻量；缺失字段保留旧值） */
static void parse_sensor(const char *data, size_t len, ha_sensor_t *out)
{
    char buf[192];
    size_t n = (len < sizeof(buf) - 1) ? len : sizeof(buf) - 1;
    memcpy(buf, data, n);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        ESP_LOGW(TAG, "传感器 JSON 解析失败");
        return;
    }
    cJSON *t = cJSON_GetObjectItem(root, "temp");
    cJSON *h = cJSON_GetObjectItem(root, "hum");
    cJSON *r = cJSON_GetObjectItem(root, "rssi");
    if (cJSON_IsNumber(t)) out->temp = (float)t->valuedouble;
    if (cJSON_IsNumber(h)) out->hum  = (float)h->valuedouble;
    if (cJSON_IsNumber(r)) out->rssi = r->valueint;
    out->rx_us = esp_timer_get_time();
    cJSON_Delete(root);
}

/** 传感器投递任务（UI 操作在此任务上下文，而非 MQTT 回调） */
static void sensor_task(void *arg)
{
    (void)arg;
    ha_sensor_t sn;
    for (;;) {
        if (xQueueReceive(s_sensor_q, &sn, portMAX_DELAY) == pdTRUE) {
            for (int i = 0; i < HA_MAX_HANDLERS; i++) {
                if (s_sensor_handlers[i] != NULL) {
                    s_sensor_handlers[i](&sn, s_sensor_ctx[i]);
                }
            }
        }
    }
}

/* ---------- 心跳任务（周期发布状态；连接为 false 时跳过） ---------- */

static void heartbeat_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_HA_MQTT_HEARTBEAT_MS));
        publish_state(0);
    }
}

/* ---------- 对外接口 ---------- */

esp_err_t ha_mqtt_init(void)
{
    if (s_client != NULL) {
        return ESP_OK;    /* 已初始化 */
    }

    /* 1) 通知队列 + 投递任务（UI 与 MQTT 解耦的关键） */
    s_notify_q = xQueueCreate(HA_NOTIFY_QUEUE_LEN, sizeof(ha_notify_t));
    if (s_notify_q == NULL) {
        ESP_LOGE(TAG, "通知队列创建失败");
        return ESP_ERR_NO_MEM;
    }
    xTaskCreate(notify_task, "ha_notify", HA_NOTIFY_TASK_STACK, NULL,
                HA_NOTIFY_TASK_PRIO, &s_notify_task);

    /* 1.1) 传感器数据队列 + 投递任务 */
    s_sensor_q = xQueueCreate(HA_SENSOR_QUEUE_LEN, sizeof(ha_sensor_t));
    if (s_sensor_q == NULL) {
        ESP_LOGE(TAG, "传感器队列创建失败");
        return ESP_ERR_NO_MEM;
    }
    xTaskCreate(sensor_task, "ha_sensor", HA_SENSOR_TASK_STACK, NULL,
                HA_SENSOR_TASK_PRIO, &s_sensor_task);

    /* 2) esp-mqtt 客户端配置（Kconfig；匿名时 user/pass 留空。
          本版 esp-mqtt 无 DEFAULT 宏，字段显式初始化；client_id 留空=库自动生成） */
    esp_mqtt_client_config_t mqtt_cfg = { 0 };
    mqtt_cfg.broker.address.hostname  = (app_cfg_get()->mqtt_host);
    mqtt_cfg.broker.address.port      = (app_cfg_get()->mqtt_port);
    mqtt_cfg.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
    if (CONFIG_HA_MQTT_CLIENT_ID[0] != '\0') {
        mqtt_cfg.credentials.client_id = CONFIG_HA_MQTT_CLIENT_ID;
    }
    if (CONFIG_HA_MQTT_USER[0] != '\0') {
        mqtt_cfg.credentials.username = CONFIG_HA_MQTT_USER;
        mqtt_cfg.credentials.authentication.password = CONFIG_HA_MQTT_PASSWORD;
    }

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "MQTT 客户端创建失败");
        return ESP_FAIL;
    }
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                                   mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_client));

    /* 3) 心跳任务（周期发布 state） */
    xTaskCreate(heartbeat_task, "ha_hb", HA_HEARTBEAT_STACK, NULL,
                HA_HEARTBEAT_PRIO, &s_heartbeat_task);

    ESP_LOGI(TAG, "HA MQTT 接入已启动：%s:%d，订阅 %s，发布 %s",
             (app_cfg_get()->mqtt_host), (app_cfg_get()->mqtt_port),
             (app_cfg_get()->topic_notify), CONFIG_HA_MQTT_TOPIC_STATE);
    return ESP_OK;
}

esp_err_t ha_mqtt_register_notify_handler(ha_notify_handler_t cb, void *user_ctx)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < HA_MAX_HANDLERS; i++) {
        if (s_handlers[i] == NULL) {
            s_handlers[i]    = cb;
            s_handler_ctx[i] = user_ctx;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t ha_mqtt_register_sensor_handler(ha_sensor_handler_t cb, void *user_ctx)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < HA_MAX_HANDLERS; i++) {
        if (s_sensor_handlers[i] == NULL) {
            s_sensor_handlers[i] = cb;
            s_sensor_ctx[i]      = user_ctx;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

void ha_mqtt_get_last_sensor(ha_sensor_t *out)
{
    if (out != NULL) {
        *out = s_last_sensor;
    }
}

esp_err_t ha_mqtt_publish_alert(const char *type, const char *message)
{
    if (s_client == NULL || !s_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", type ? type : "alert");
    cJSON_AddStringToObject(root, "message", message ? message : "");
    cJSON_AddNumberToObject(root, "uptime", (double)(esp_timer_get_time() / 1000000));
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int id = esp_mqtt_client_publish(s_client, CONFIG_HA_MQTT_TOPIC_ALERT,
                                     payload, 0, 1, 0);   /* QoS1，不 retain */
    free(payload);
    if (id < 0) {
        ESP_LOGW(TAG, "告警发布失败");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "已发布告警：%s（%s）", type, message ? message : "");
    return ESP_OK;
}

bool ha_mqtt_is_connected(void)
{
    return s_connected;
}