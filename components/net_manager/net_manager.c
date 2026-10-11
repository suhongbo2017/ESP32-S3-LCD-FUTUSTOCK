/**
 * @file net_manager.c
 * @brief 双网冗余管理器：以太网优先、WiFi 兜底，周期轮询并自动切换默认路由
 *
 * 设计说明：
 *  - 采用「周期轮询 + 状态快照」模型：每 3s 读取 eth/wifi 的就绪标志，
 *    按优先级（可扩展为策略接口）挑选第一个可用的上行链路，
 *    并通过 esp_netif_set_default_netif() 切换 lwIP 默认路由。
 *  - 相比纯事件驱动，轮询天然带“去抖”效果，且能自愈：
 *    断网恢复 / WiFi 重连成功等场景无需额外联动，下一次轮询即自动切换回来。
 *  - 链路变化时通过监听器回调通知上层（网络看板 / MQTT 断线补传等）。
 *  - TODO(后续里程碑)：
 *      1) 切换滞回（连续 N 次采样稳定后才切换，避免抖动）；
 *      2) 活跃链路长时间无流量时主动探测（ARP/DNS 探活）；
 *      3) 策略接口外部化（优先级可配置、允许“仅用 WiFi”等）；
 *      4) 断网数据缓存与补传的作业队列接口。
 */
#include "net_manager.h"

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "net_eth.h"
#include "net_wifi.h"

static const char *TAG = "net_manager";

#define NET_MANAGER_POLL_MS          3000 /*!< 轮询周期 */
#define NET_MANAGER_TASK_STACK       4096 /*!< 管理器任务栈大小 */
#define NET_MANAGER_TASK_PRIO        5
#define NET_MANAGER_MAX_LISTENERS    8    /*!< 监听器数量上限 */

/* 默认优先级：有线 > Wi-Fi。后续可扩展成可配置策略。 */
static const net_backend_t s_priority[2] = { NET_BACKEND_ETH, NET_BACKEND_WIFI };
#define NET_MANAGER_BACKEND_CNT (sizeof(s_priority) / sizeof(s_priority[0]))

static net_manager_status_t s_status;                       /*!< 状态快照 */
static SemaphoreHandle_t     s_mutex  = NULL;
static net_manager_cb_t      s_listeners[NET_MANAGER_MAX_LISTENERS];
static void                *s_listener_ctx[NET_MANAGER_MAX_LISTENERS];
static TaskHandle_t          s_task   = NULL;

/* ---------------------------------------------------------------------------
 * 内部工具
 * ------------------------------------------------------------------------- */

static esp_netif_t *backend_get_netif(net_backend_t backend)
{
    switch (backend) {
    case NET_BACKEND_ETH:   return net_eth_get_netif();
    case NET_BACKEND_WIFI:  return net_wifi_get_netif();
    default:                return NULL;
    }
}

static bool backend_is_ready(net_backend_t backend)
{
    switch (backend) {
    case NET_BACKEND_ETH:   return net_eth_is_ready();
    case NET_BACKEND_WIFI:  return net_wifi_is_connected();
    default:                return false;
    }
}

/** 抓取当前活跃链路的 IP 字符串，写入快照 */
static void snapshot_active_ip(void)
{
    esp_netif_t *netif = backend_get_netif(s_status.active);
    if (netif != NULL) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(netif, &info) == ESP_OK) {
            esp_ip4addr_ntoa(&info.ip, s_status.active_ip, sizeof(s_status.active_ip));
            return;
        }
    }
    s_status.active_ip[0] = '\0';
}

static void notify_listeners(void)
{
    for (int i = 0; i < NET_MANAGER_MAX_LISTENERS; i++) {
        if (s_listeners[i] != NULL) {
            s_listeners[i](&s_status, s_listener_ctx[i]);
        }
    }
}

/* ---------------------------------------------------------------------------
 * 核心决策
 * ------------------------------------------------------------------------- */

/** 评估双网状态并按优先级执行切换（轮询任务调用） */
static void net_manager_update(void)
{
    if (s_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* 1) 刷新两侧就绪标志 */
    s_status.eth_ready  = backend_is_ready(NET_BACKEND_ETH);
    s_status.wifi_ready = backend_is_ready(NET_BACKEND_WIFI);

    /* 2) 按优先级挑选第一个就绪的链路 */
    net_backend_t candidate = NET_BACKEND_NONE;
    for (int i = 0; i < NET_MANAGER_BACKEND_CNT; i++) {
        if (backend_is_ready(s_priority[i])) {
            candidate = s_priority[i];
            break;
        }
    }

    /* 3) 链路发生变化才执行切换（避免反复抖动） */
    if (candidate != s_status.active) {
        net_backend_t old = s_status.active;

        if (candidate != NET_BACKEND_NONE) {
            esp_netif_t *netif = backend_get_netif(candidate);
            if (netif != NULL) {
                /* 切换 lwIP 默认接口（同时更换默认路由），上层业务无需感知 */
                esp_netif_set_default_netif(netif);
            }
            s_status.active = candidate;
            snapshot_active_ip();
            ESP_LOGI(TAG, "上行链路切换：%s -> %s（IP=%s）",
                     net_manager_backend_name(old),
                     net_manager_backend_name(candidate),
                     s_status.active_ip[0] ? s_status.active_ip : "-");
        } else {
            /* 双网均不可用：业务应进入离线模式（LCD 提示 / 数据缓存） */
            s_status.active    = NET_BACKEND_NONE;
            s_status.active_ip[0] = '\0';
            ESP_LOGW(TAG, "有线与 Wi-Fi 均不可用，进入离线状态");
        }

        notify_listeners();
    }

    xSemaphoreGive(s_mutex);
}

/* ---------------------------------------------------------------------------
 * 轮询任务 & 对外接口
 * ------------------------------------------------------------------------- */

static void net_manager_task(void *arg)
{
    (void)arg;
    for (;;) {
        net_manager_update();                     /* 先立即评估一次 */
        vTaskDelay(pdMS_TO_TICKS(NET_MANAGER_POLL_MS));
    }
}

esp_err_t net_manager_init(void)
{
    if (s_mutex != NULL) {
        return ESP_OK;                            /* 已初始化 */
    }
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "互斥锁创建失败");
        return ESP_ERR_NO_MEM;
    }

    memset(&s_status, 0, sizeof(s_status));
    s_status.active = NET_BACKEND_NONE;
    memset(s_listeners,     0, sizeof(s_listeners));
    memset(s_listener_ctx,  0, sizeof(s_listener_ctx));

    if (s_task == NULL) {
        xTaskCreate(net_manager_task, "net_mgr", NET_MANAGER_TASK_STACK,
                    NULL, NET_MANAGER_TASK_PRIO, &s_task);
    }

    ESP_LOGI(TAG, "net_manager 已启动：优先级 ETH > WiFi，轮询周期 %d ms",
             NET_MANAGER_POLL_MS);
    return ESP_OK;
}

net_manager_status_t net_manager_get_status(void)
{
    net_manager_status_t st;
    memset(&st, 0, sizeof(st));
    if (s_mutex != NULL) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        st = s_status;
        xSemaphoreGive(s_mutex);
    }
    return st;
}

net_backend_t net_manager_get_active(void)
{
    return net_manager_get_status().active;
}

esp_err_t net_manager_register_listener(net_manager_cb_t cb, void *user_ctx)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < NET_MANAGER_MAX_LISTENERS; i++) {
        if (s_listeners[i] == NULL) {
            s_listeners[i]    = cb;
            s_listener_ctx[i] = user_ctx;
            ESP_LOGI(TAG, "已注册链路切换监听器（槽位 %d/%d）",
                     i + 1, NET_MANAGER_MAX_LISTENERS);
            return ESP_OK;
        }
    }
    ESP_LOGE(TAG, "监听器数量已达上限（%d）", NET_MANAGER_MAX_LISTENERS);
    return ESP_ERR_NO_MEM;
}

const char *net_manager_backend_name(net_backend_t backend)
{
    switch (backend) {
    case NET_BACKEND_ETH:   return "ETH";
    case NET_BACKEND_WIFI:  return "WiFi";
    default:                return "None";
    }
}