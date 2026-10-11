/**
 * @file net_wifi.c
 * @brief WiFi Station 初始化：创建 netif、注册事件、按 Kconfig 配置并连接
 *
 * 注意：
 *  - 使用本组件前需先完成 NVS 初始化（见 main/app_main.c）。
 *  - 首次连接由 WIFI_EVENT_STA_START 触发；断线按 CONFIG_NET_WIFI_MAX_RETRY
 *    次数自动重连，达到上限后暂停，等待上层（net_manager）指示恢复。
 *  - IP 获取默认走 DHCP（esp_netif_create_default_wifi_sta 自带客户端）；
 *    可选静态 IP（Kconfig：CONFIG_NET_WIFI_USE_STATIC_IP，默认关闭）。
 */
#include "net_wifi.h"
#include "app_cfg.h"

#include <string.h>

#include "esp_err.h"
#include "esp_check.h"    /* ESP_RETURN_ON_ERROR */
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"

static const char *TAG = "net_wifi";

static esp_netif_t *s_netif    = NULL;
static int          s_retry    = 0;      /*!< 连续断线重连计数 */
static volatile bool s_associated = false;
static volatile bool s_ip_ready    = false;

/** WiFi 联网参数（目前来自 Kconfig；可选静态 IP，默认 DHCP） */
typedef struct {
    bool use_static_ip;            /*!< true = 静态 IP；false = DHCP */
    esp_netif_ip_info_t ip_info;   /*!< 静态 IP 信息（地址/掩码/网关） */
    esp_ip4_addr_t dns_server;     /*!< 静态 IP 时的 DNS */
} net_wifi_cfg_t;
static net_wifi_cfg_t s_cfg;

/* ---------------------------------------------------------------------------
 * 配置载入 & 静态 IP
 * ------------------------------------------------------------------------- */

/** 从 Kconfig 载入联网参数（静态 IP 部分为可选项） */
static void load_default_config(net_wifi_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
#if defined(CONFIG_NET_WIFI_USE_STATIC_IP)
    cfg->use_static_ip = true;
    esp_netif_str_to_ip4(CONFIG_NET_WIFI_STATIC_IP, &cfg->ip_info.ip);
    esp_netif_str_to_ip4(CONFIG_NET_WIFI_NETMASK,   &cfg->ip_info.netmask);
    esp_netif_str_to_ip4(CONFIG_NET_WIFI_GATEWAY,   &cfg->ip_info.gw);
    esp_netif_str_to_ip4(CONFIG_NET_WIFI_DNS,       &cfg->dns_server);
#endif
}

/** 应用静态 IP：阻断 DHCP，写入地址与 DNS（初始化 / 关联成功 / 获得 IP 时均调用） */
static void apply_static_ip(void)
{
    if (s_netif == NULL) {
        return;
    }
    esp_netif_dhcpc_stop(s_netif);
    esp_netif_set_ip_info(s_netif, &s_cfg.ip_info);
    esp_netif_dns_info_t dns = {
        .ip = {
            .type       = ESP_IPADDR_TYPE_V4,
            .u_addr.ip4 = s_cfg.dns_server,
        },
    };
    /* IDF v5.5：旧的 esp_netif_dns_setserver 已移除，改用 esp_netif_set_dns_info */
    esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns);
}

/* ---------------------------------------------------------------------------
 * 事件回调
 * ------------------------------------------------------------------------- */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)arg; (void)base;

    switch (event_id) {
    case WIFI_EVENT_STA_START:
        ESP_LOGI(TAG, "STA 启动，开始连接 %s ...", (app_cfg_get()->wifi_ssid));
        esp_wifi_connect();
        break;

    case WIFI_EVENT_STA_CONNECTED:
        s_associated = true;
        if (s_cfg.use_static_ip) {
            apply_static_ip();           /* 关联成功即应用静态地址，防 DHCP 覆盖 */
            s_ip_ready = true;           /* 静态配置立即可用 */
            ESP_LOGI(TAG, "已关联 AP：%s（静态 IP）", (app_cfg_get()->wifi_ssid));
        } else {
            ESP_LOGI(TAG, "已关联 AP：%s，等待分配 IP...", (app_cfg_get()->wifi_ssid));
        }
        break;

    case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t *evt =
            (wifi_event_sta_disconnected_t *)event_data;
        s_associated = false;

        if (s_retry < CONFIG_NET_WIFI_MAX_RETRY) {
            s_retry++;
            ESP_LOGW(TAG, "WiFi 断开（reason=%d），第 %d/%d 次重连...",
                     evt->reason, s_retry, CONFIG_NET_WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "达到最大重连次数，暂停自动重试（后续由 net_manager 周期恢复）");
        }
        break;
    }

    default:
        break;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    (void)arg; (void)base;

    if (event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
        char ip[16] = {0};
        esp_ip4addr_ntoa(&evt->ip_info.ip, ip, sizeof(ip));
        if (s_cfg.use_static_ip) {
            apply_static_ip();          /* 兜底：若 DHCP 抢跑，立即改回静态地址 */
        }
        s_retry    = 0;                 /* 成功连接后清零重连计数 */
        s_ip_ready = true;
        ESP_LOGI(TAG, "WiFi 网络就绪：IP=%s", ip);
    } else if (event_id == IP_EVENT_STA_LOST_IP) {
        s_ip_ready = false;
        ESP_LOGW(TAG, "WiFi IP 已丢失");
    }
}

/* ---------------------------------------------------------------------------
 * 对外接口
 * ------------------------------------------------------------------------- */

esp_err_t net_wifi_init(void)
{
    /* 1) 创建 STA netif（自带 DHCP 客户端） */
    s_netif = esp_netif_create_default_wifi_sta();
    if (s_netif == NULL) {
        ESP_LOGE(TAG, "创建 STA netif 失败");
        return ESP_FAIL;
    }

    /* 1.1) 读取联网参数并应用（静态 IP 可选，默认 DHCP） */
    load_default_config(&s_cfg);
    if (s_cfg.use_static_ip) {
        apply_static_ip();
        ESP_LOGI(TAG, "WiFi 使用静态 IP 配置");
    }

    /* 2) WiFi 驱动初始化（前置条件：NVS 已初始化，由 app_main 保证） */
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_cfg), TAG, "esp_wifi_init 失败");

    /* 3) 注册事件处理器 */
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   wifi_event_handler, NULL),
                        TAG, "WIFI_EVENT 处理器注册失败");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   ip_event_handler, NULL),
                        TAG, "IP_EVENT(STA_GOT_IP) 处理器注册失败");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP,
                                                   ip_event_handler, NULL),
                        TAG, "IP_EVENT(STA_LOST_IP) 处理器注册失败");

    /* 4) 配置 STA 参数 */
    wifi_config_t sta_cfg = { 0 };
    strlcpy((char *)sta_cfg.sta.ssid,     (app_cfg_get()->wifi_ssid),
            sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, (app_cfg_get()->wifi_pass),
            sizeof(sta_cfg.sta.password));
    if (strlen((app_cfg_get()->wifi_pass)) == 0) {
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;          /* 开放网络 */
    } else {
        /* 阈值取 WPA_PSK(2)：兼容 WPA / WPA2 / WPA-WPA2 混合 / WPA2-WPA3 热点。
         * !!! 切勿设为 WPA2_WPA3_PSK(7)：纯 WPA2(3) 或 WPA/WPA2 混合(4) 的 AP 会因
         *     “authmode threshold” 被拒，表现为 reason=211
         *     （NO_AP_FOUND_IN_AUTHMODE_THRESHOLD）—— 本项目踩过的坑。 */
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "设置 STA 模式失败");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg), TAG, "设置 STA 配置失败");

    /* 5) 启动（首次连接由 WIFI_EVENT_STA_START 触发） */
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start 失败");

    ESP_LOGI(TAG, "WiFi(STA) 初始化完成：SSID=%s", (app_cfg_get()->wifi_ssid));
    return ESP_OK;
}

esp_netif_t *net_wifi_get_netif(void)
{
    return s_netif;
}

bool net_wifi_is_connected(void)
{
    return s_associated && s_ip_ready;
}

esp_err_t net_wifi_get_ip_info(esp_netif_ip_info_t *info)
{
    if (s_netif == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_netif_get_ip_info(s_netif, info);
}