/**
 * @file app_cfg.c
 * @brief 运行时可改参数实现（NVS 覆盖 Kconfig 默认值）
 */
#include "app_cfg.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_check.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "app_cfg";
#define NVS_NS   "gwcfg"

static app_cfg_t s_cfg;
static bool      s_from_nvs = false;
static bool      s_inited   = false;

static void load_defaults(void)
{
    snprintf(s_cfg.wifi_ssid,    sizeof(s_cfg.wifi_ssid),    "%s", CONFIG_NET_WIFI_SSID);
    snprintf(s_cfg.wifi_pass,    sizeof(s_cfg.wifi_pass),    "%s", CONFIG_NET_WIFI_PASSWORD);
    snprintf(s_cfg.mqtt_host,    sizeof(s_cfg.mqtt_host),    "%s", CONFIG_HA_MQTT_BROKER_HOST);
    s_cfg.mqtt_port = CONFIG_HA_MQTT_PORT;
    snprintf(s_cfg.topic_notify, sizeof(s_cfg.topic_notify), "%s", CONFIG_HA_MQTT_TOPIC_NOTIFY);
    snprintf(s_cfg.topic_sensor, sizeof(s_cfg.topic_sensor), "%s", CONFIG_HA_MQTT_TOPIC_SENSOR);
}

/** 读字符串项：存在则覆盖 */
static bool load_str(nvs_handle_t h, const char *key, char *dst, size_t dst_sz)
{
    size_t len = dst_sz;
    esp_err_t err = nvs_get_str(h, key, dst, &len);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "NVS 覆盖 %s = \"%s\"", key, dst);
        return true;
    }
    return false;
}

esp_err_t app_cfg_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    load_defaults();

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        s_inited = true;
        ESP_LOGI(TAG, "无 NVS 配置，全部使用 Kconfig 默认值");
        return ESP_OK;
    }

    bool any = false;
    any |= load_str(h, "wifi_ssid",    s_cfg.wifi_ssid,    sizeof(s_cfg.wifi_ssid));
    any |= load_str(h, "wifi_pass",    s_cfg.wifi_pass,    sizeof(s_cfg.wifi_pass));
    any |= load_str(h, "mqtt_host",    s_cfg.mqtt_host,    sizeof(s_cfg.mqtt_host));
    any |= load_str(h, "topic_notify", s_cfg.topic_notify, sizeof(s_cfg.topic_notify));
    any |= load_str(h, "topic_sensor", s_cfg.topic_sensor, sizeof(s_cfg.topic_sensor));

    int32_t port = 0;
    if (nvs_get_i32(h, "mqtt_port", &port) == ESP_OK && port > 0 && port < 65536) {
        s_cfg.mqtt_port = (int)port;
        ESP_LOGI(TAG, "NVS 覆盖 mqtt_port = %d", (int)port);
        any = true;
    }
    nvs_close(h);

    s_from_nvs = any;
    s_inited   = true;
    ESP_LOGI(TAG, "配置已加载（%s）", any ? "含 NVS 覆盖" : "全部 Kconfig 默认");
    return ESP_OK;
}

const app_cfg_t *app_cfg_get(void)
{
    return &s_cfg;
}

bool app_cfg_from_nvs(void)
{
    return s_from_nvs;
}

esp_err_t app_cfg_set(const char *key, const char *val)
{
    if (key == NULL || val == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char *dst = NULL;
    size_t dst_sz = 0;
    if      (strcmp(key, "wifi_ssid")    == 0) { dst = s_cfg.wifi_ssid;    dst_sz = sizeof(s_cfg.wifi_ssid); }
    else if (strcmp(key, "wifi_pass")    == 0) { dst = s_cfg.wifi_pass;    dst_sz = sizeof(s_cfg.wifi_pass); }
    else if (strcmp(key, "mqtt_host")    == 0) { dst = s_cfg.mqtt_host;    dst_sz = sizeof(s_cfg.mqtt_host); }
    else if (strcmp(key, "topic_notify") == 0) { dst = s_cfg.topic_notify; dst_sz = sizeof(s_cfg.topic_notify); }
    else if (strcmp(key, "topic_sensor") == 0) { dst = s_cfg.topic_sensor; dst_sz = sizeof(s_cfg.topic_sensor); }
    else {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(val) >= dst_sz) {
        return ESP_ERR_INVALID_SIZE;
    }
    snprintf(dst, dst_sz, "%s", val);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return ESP_FAIL;
    }
    esp_err_t err = nvs_set_str(h, key, val);
    if (err == ESP_OK) {
        err = nvs_commit(h);
        s_from_nvs = true;
    }
    nvs_close(h);
    ESP_LOGI(TAG, "已保存 %s = \"%s\"（%s）", key, val, esp_err_to_name(err));
    return err;
}

esp_err_t app_cfg_set_int(const char *key, int val)
{
    if (key == NULL || strcmp(key, "mqtt_port") != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (val <= 0 || val > 65535) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg.mqtt_port = val;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return ESP_FAIL;
    }
    esp_err_t err = nvs_set_i32(h, key, (int32_t)val);
    if (err == ESP_OK) {
        err = nvs_commit(h);
        s_from_nvs = true;
    }
    nvs_close(h);
    ESP_LOGI(TAG, "已保存 %s = %d（%s）", key, val, esp_err_to_name(err));
    return err;
}

esp_err_t app_cfg_reset(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return ESP_FAIL;
    }
    esp_err_t err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        load_defaults();
        s_from_nvs = false;
        ESP_LOGI(TAG, "已清除 NVS 配置（恢复 Kconfig 默认，重启生效）");
    }
    return err;
}