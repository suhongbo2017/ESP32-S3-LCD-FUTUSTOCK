/**
 * @file app_main.c
 * @brief 双网冗余 IoT 网关（方案 A）—— 应用入口（网络骨架里程碑）
 *
 * 启动顺序：
 *   1) NVS 初始化（WiFi 驱动等依赖）
 *   2) 默认事件循环
 *   3) net_eth_init   —— W5500 有线（主链路）
 *   4) net_wifi_init  —— WiFi Station（备用链路）
 *   5) net_manager_init —— 双网优先级/切换（ETH 优先，WIFI 兜底）
 *   6) ha_mqtt_init  —— Home Assistant MQTT 接入（本地 broker，通知/状态/Discovery）
 *   7) ui_dashboard_init —— ST7735S + LVGL 本地看板（须在 net_eth_init 之后，
 *                           共享复位线约束；数据来自 net_manager + ha_mqtt 通知）
 *   8) 演示任务：周期打印网络状态（与看板互补）
 *
 * 说明：里程碑 C 已接入 HA MQTT；TF 卡记录仍在后续里程碑。
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <dirent.h>
#include <sys/time.h>

#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "net_eth.h"
#include "net_wifi.h"
#include "net_manager.h"
#include "market_data.h"
#include "ha_mqtt.h"
#include "ui_dashboard.h"
#include "tf_storage.h"
#include "web_console.h"
#include "app_cfg.h"
#include "sensor_history.h"
#include "esp_netif_sntp.h"

static const char *TAG = "app_main";

/* 传感器掉线告警状态（状态变化时只触发一次） */
static bool s_sensor_alerted = false;

/* ---------------------------------------------------------------------------
 * 时间同步（TF 记录时间戳用）
 *   1) 先尝试 SNTP（需外网 UDP/123）
 *   2) 失败则由 time_sync_task 改用 HTTP Date 响应头校时
 *      （仅需局域网内 HTTP 服务；复用 HA 主机，因为 broker 与 HA 同机）
 * ------------------------------------------------------------------------- */
#define TIME_SYNC_HTTP_HOST  CONFIG_HA_MQTT_BROKER_HOST   /* HA 与 broker 在同一台机器 */
#define TIME_SYNC_HTTP_PORT  8123                          /* Home Assistant 端口 */
#define TIME_VALID_EPOCH     1700000000LL                  /* > 2023-11 视为已校时 */

/** 公历 → 1970-01-01 起的天数（Howard Hinnant 算法） */
static int days_from_civil(int y, unsigned m, unsigned d)
{
    y -= (m <= 2);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int)doe - 719468;
}

/** 从 HTTP 响应的 Date 头解析并设置系统时间 */
static bool sync_time_from_http(void)
{
    struct sockaddr_in sa = { 0 };
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(TIME_SYNC_HTTP_PORT);
    if (inet_pton(AF_INET, TIME_SYNC_HTTP_HOST, &sa.sin_addr) != 1) {
        return false;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    bool ok = false;
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
        const char *req = "HEAD / HTTP/1.0\r\nHost: ha\r\nConnection: close\r\n\r\n";
        if (send(fd, req, strlen(req), 0) > 0) {
            char buf[1024];
            int n = recv(fd, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = '\0';
                char *p = strstr(buf, "Date:");
                if (p != NULL) {
                    p += 5;
                    while (*p == ' ') p++;
                    int d, y, h, mi, se;
                    char mon[4] = { 0 };
                    if (sscanf(p, "%*3s, %d %3s %d %d:%d:%d", &d, mon, &y, &h, &mi, &se) == 6) {
                        static const char *MN = "JanFebMarAprMayJunJulAugSepOctNovDec";
                        const char *mp = strstr(MN, mon);
                        int mnum = (mp != NULL) ? (int)((mp - MN) / 3) + 1 : 0;
                        if (mnum >= 1) {
                            long long epoch = (long long)days_from_civil(y, (unsigned)mnum, (unsigned)d) * 86400LL
                                              + h * 3600LL + mi * 60LL + se;
                            struct timeval now = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
                            ok = (settimeofday(&now, NULL) == 0);
                        }
                    }
                }
            }
        }
    }
    close(fd);
    return ok;
}

/* 内存探针：定位哪个初始化步骤吃掉内部堆（崩溃排查用） */
static void log_heap(const char *step)
{
    ESP_LOGI(TAG, "[heap] %-16s free=%6u block=%6u min=%6u | psram=%u",
             step,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

/* ---------------------------------------------------------------------------
 * 开机回灌：从 TF 日志的最新一天 CSV 读 recent sensor 记录，填历史缓冲
 *   时间基准用“相对最新一条的差值”，因此无需等系统时钟校好
 * ------------------------------------------------------------------------- */
#define SEED_MAX 240

typedef struct { long long epoch; float temp; float hum; } seed_rec_t;

static bool parse_log_time(const char *s, long long *out)
{
    int Y, M, D, h, mi, se;
    if (sscanf(s, "%d-%d-%d %d:%d:%d", &Y, &M, &D, &h, &mi, &se) != 6) {
        return false;
    }
    *out = (long long)days_from_civil(Y, (unsigned)M, (unsigned)D) * 86400LL
           + h * 3600LL + mi * 60LL + se;
    return true;
}

static void seed_history_from_log(void)
{
    if (!tf_storage_is_ready()) {
        return;
    }
    const char *dir = tf_storage_log_dir();

    /* 选目录里名字最大的 log_YYYYMMDD.csv（日期命名，字典序即时间序） */
    char best[64] = { 0 };
    DIR *d = opendir(dir);
    if (d == NULL) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "log_2", 5) != 0 || strstr(e->d_name, ".csv") == NULL) {
            continue;
        }
        if (strcmp(e->d_name, best) > 0) {
            strncpy(best, e->d_name, sizeof(best) - 1);
        }
    }
    closedir(d);
    if (best[0] == '\0') {
        return;
    }

    char path[192];
    snprintf(path, sizeof(path), "%s/%s", dir, best);
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return;
    }

    seed_rec_t *recs = malloc(sizeof(seed_rec_t) * SEED_MAX);
    if (recs == NULL) {
        fclose(f);
        return;
    }
    int n = 0;              /* 环形：保留最后 SEED_MAX 条 */
    int total = 0;
    char line[192];
    while (fgets(line, sizeof(line), f) != NULL) {
        char *c1 = strchr(line, ',');
        if (c1 == NULL) continue;
        char *c2 = strchr(c1 + 1, ',');
        if (c2 == NULL) continue;
        if (strncmp(c1 + 1, "sensor", 6) != 0) continue;
        long long ep;
        if (!parse_log_time(line, &ep)) continue;
        float t, h;
        if (sscanf(c2 + 1, "temp=%f hum=%f", &t, &h) != 2) continue;
        recs[total % SEED_MAX].epoch = ep;
        recs[total % SEED_MAX].temp = t;
        recs[total % SEED_MAX].hum = h;
        total++;
    }
    fclose(f);
    n = (total < SEED_MAX) ? total : SEED_MAX;
    if (n >= 2) {
        long long newest = 0;
        for (int i = 0; i < n; i++) {
            if (recs[i].epoch > newest) newest = recs[i].epoch;
        }
        int start = (total > SEED_MAX) ? (total % SEED_MAX) : 0;
        for (int i = 0; i < n; i++) {
            seed_rec_t *r = &recs[(start + i) % SEED_MAX];
            long long age_s = newest - r->epoch;
            if (age_s < 0) age_s = 0;
            sensor_history_add_ts(r->temp, r->hum, (int64_t)age_s * 1000000LL);
        }
        ESP_LOGI(TAG, "已从 %s 回灌 %d 条历史（跨重启连续）", best, n);
    } else if (total > 0) {
        ESP_LOGI(TAG, "日志中仅 %d 条 sensor 记录，暂不回灌", total);
    }
    free(recs);
}

static void time_sync_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < 30; i++) {          /* 最多约 5 分钟 */
        vTaskDelay(pdMS_TO_TICKS(10000));
        if (time(NULL) > TIME_VALID_EPOCH) {
            ESP_LOGI(TAG, "时间已校（SNTP）");
            vTaskDelete(NULL);
        }
        if (sync_time_from_http()) {
            time_t t = time(NULL);
            struct tm tmv;
            localtime_r(&t, &tmv);
            char b[32];
            strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &tmv);
            ESP_LOGI(TAG, "时间已通过 HTTP Date 校准：%s", b);
            vTaskDelete(NULL);
        }
    }
    ESP_LOGW(TAG, "时间同步失败（TF 记录将用 uptime 作时间戳）");
    vTaskDelete(NULL);
}

/* ---------------------------------------------------------------------------
 * NVS：WiFi 存储、后续 MQTT/NVS 业务参数都依赖它
 * ------------------------------------------------------------------------- */
static esp_err_t nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* NVS 分区损坏或版本升级：擦除重建（会丢失已存参数） */
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

/* ---------------------------------------------------------------------------
 * 演示：订阅链路切换通知（后续 MQTT 断线补传、LCD 状态图标都挂在这里）
 * ------------------------------------------------------------------------- */
static void on_uplink_changed(net_manager_status_t *status, void *user_ctx)
{
    (void)user_ctx;
    ESP_LOGI(TAG, "[监听器] 上行链路 -> %s（IP=%s）",
             net_manager_backend_name(status->active),
             status->active_ip[0] ? status->active_ip : "-");
    /* 链路切换落盘（TF 记录） */
    tf_storage_log("net", "uplink=%s,ip=%s,eth=%d,wifi=%d",
                   net_manager_backend_name(status->active),
                   status->active_ip[0] ? status->active_ip : "-",
                   (int)status->eth_ready, (int)status->wifi_ready);

    /* 诊断：打印默认 netif 的 DNS（SNTP 名字解析依赖它） */
    esp_netif_t *nif = esp_netif_get_default_netif();
    if (nif != NULL) {
        esp_netif_dns_info_t d;
        char ipbuf[20] = "-";
        if (esp_netif_get_dns_info(nif, ESP_NETIF_DNS_MAIN, &d) == ESP_OK &&
            d.ip.type == ESP_IPADDR_TYPE_V4) {
            esp_ip4addr_ntoa(&d.ip.u_addr.ip4, ipbuf, sizeof(ipbuf));
        }
        ESP_LOGI(TAG, "DNS 主服务器: %s", ipbuf);
    }
}

/* ---------------------------------------------------------------------------
 * 演示：周期打印网络状态（后续交给 LCD 看板）
 * ------------------------------------------------------------------------- */
static void status_task(void *arg)
{
    (void)arg;
    int tick = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        net_manager_status_t st = net_manager_get_status();
        ESP_LOGI(TAG, "状态：上行=%s（IP=%s）| 有线可用=%d Wi-Fi可用=%d",
                 net_manager_backend_name(st.active),
                 st.active_ip[0] ? st.active_ip : "-",
                 (int)st.eth_ready, (int)st.wifi_ready);

        /* 每 60s 打印内存状况：用于发现泄漏/碎片/耗尽（崩溃排查关键） */
        if (++tick % 6 == 0) {
            ESP_LOGI(TAG, "内存: 内部free=%u 最大块=%u 最低水位=%u | DMA-free=%u | PSRAM-free=%u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        }

        /* 传感器掉线告警：看板弹通知 + MQTT 告警（HA 可转发手机）+ TF 记录 */
        ha_sensor_t sn;
        ha_mqtt_get_last_sensor(&sn);
        bool offline = (sn.rx_us == 0) ||
                       ((esp_timer_get_time() - sn.rx_us) >
                        (int64_t)CONFIG_HA_MQTT_SENSOR_ALERT_MS * 1000);
        if (offline && !s_sensor_alerted) {
            s_sensor_alerted = true;
            ESP_LOGW(TAG, "传感器掉线（超 %d ms 无数据）", CONFIG_HA_MQTT_SENSOR_ALERT_MS);
            ui_dashboard_show_notify("传感器掉线", "超时未收到传感器数据", 2);
            ha_mqtt_publish_alert("sensor_offline", "超时未收到传感器数据");
            tf_storage_log("alert", "type=sensor_offline");
        } else if (!offline && s_sensor_alerted) {
            s_sensor_alerted = false;
            ESP_LOGI(TAG, "传感器已恢复上报");
            ui_dashboard_show_notify("传感器恢复", "数据已恢复上报", 0);
            ha_mqtt_publish_alert("sensor_recovered", "传感器数据已恢复");
            tf_storage_log("alert", "type=sensor_recovered");
        }
    }
}

static void market_data_task(void *arg)
{
    (void)arg;
    market_data_register_refresh_task();
    bool refreshed = false;
    for (;;) {
        net_manager_status_t status = net_manager_get_status();
        if (status.active != NET_BACKEND_NONE) {
            esp_err_t err = market_data_refresh();
            refreshed = (err == ESP_OK);
        } else {
            ESP_LOGW(TAG, "行情刷新跳过：暂无可用网络");
        }

        /* 定时刷新与 Web 手动刷新共用同一任务，避免并发访问行情接口。 */
        uint32_t wait_ms = refreshed ? CONFIG_MARKET_DATA_REFRESH_MS : 5000;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
    }
}

/* ---------------------------------------------------------------------------
 * 入口
 * ------------------------------------------------------------------------- */
void app_main(void)
{
    ESP_ERROR_CHECK(nvs_init());

    /* 初始化 esp_netif / LWIP 协议栈（必须！否则 tcpip_mbox 无效，
       以太网收到数据包时会断言 tcpip_inpkt: Invalid mbox 并重启） */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    log_heap("netif");

    /* 0) 运行时可改参数（NVS 优先，否则用 Kconfig 默认；Web 控制台可修改） */
    ESP_ERROR_CHECK(app_cfg_init());
    log_heap("app_cfg");

    /* 1) 有线主链路（LCD RST 与 W5500 共用 GPIO15，面板初始化须在其后） */
    ESP_ERROR_CHECK(net_eth_init(NULL));
    log_heap("net_eth");

#if defined(CONFIG_LCD_ST7735_SCROLL_TEST) || defined(CONFIG_LCD_ST7735_TEST_PATTERN)
    /* 诊断固件必须在创建行情、状态、时间同步等任务前退出正常启动路径。
       这些任务可能访问尚未初始化的 LVGL；测试模式本身由 LCD 函数阻塞运行。 */
    ESP_LOGI(TAG, "LCD 诊断模式已启用，跳过网络业务/LVGL 初始化");
    if (ui_dashboard_init() != ESP_OK) {
        ESP_LOGE(TAG, "LCD 诊断模式初始化失败");
    }
    return;
#endif

    /* 2) Wi-Fi 备用链路（SSID/密码在 menuconfig 中配置） */
    ESP_ERROR_CHECK(net_wifi_init());
    log_heap("net_wifi");

    /* 3) 双网管理器：以太网优先、Wi-Fi 兜底、断线自动切换 */
    ESP_ERROR_CHECK(net_manager_init());
    ESP_ERROR_CHECK(market_data_init());
    xTaskCreate(market_data_task, "market_data", 12288, NULL, 4, NULL);
    log_heap("net_manager");

    /* 4) 链路切换监听与周期状态打印 */
    ESP_ERROR_CHECK(net_manager_register_listener(on_uplink_changed, NULL));
    xTaskCreate(status_task, "status", 4096, NULL, 5, NULL);

    /* 5) SNTP 校时（TF 记录时间戳来源；无外网时 tf_storage 自动退化为 uptime） */
    setenv("TZ", "CST-8", 1);   /* 中国标准时间 */
    tzset();
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    sntp_cfg.start = true;
    esp_err_t sntp_ret = esp_netif_sntp_init(&sntp_cfg);
    ESP_LOGI(TAG, "SNTP 校时启动（ntp.aliyun.com）：%s", esp_err_to_name(sntp_ret));
    /* 备用：局域网 HTTP Date 校时（不需要外网） */
    xTaskCreate(time_sync_task, "time_sync", 4096, NULL, 4, NULL);

    /* 6) TF 卡历史记录（未插卡自动降级，不影响其它功能） */
    if (tf_storage_init() != ESP_OK) {
        ESP_LOGW(TAG, "TF 记录不可用（系统继续运行）");
    } else {
        net_manager_status_t st0 = net_manager_get_status();
        tf_storage_log("boot", "uplink=%s,ip=%s",
                       net_manager_backend_name(st0.active),
                       st0.active_ip[0] ? st0.active_ip : "-");
        seed_history_from_log();   /* 回灌历史，曲线跨重启连续 */
    }
    log_heap("tf_storage");

    /* 7) Home Assistant MQTT 接入（连接走默认 netif=ETH） */
    ESP_ERROR_CHECK(ha_mqtt_init());
    log_heap("ha_mqtt");

    /* 8) 本地看板（ST7735S + LVGL）—— 必须在 net_eth_init() 之后（共享复位线约束）；
          内部会注册 ha_mqtt 通知/传感器处理器。
          注意：失败时仅告警不 abort（否则会陷入重启循环，Web 控制台也无法用） */
    if (ui_dashboard_init() != ESP_OK) {
        ESP_LOGE(TAG, "看板初始化失败（系统继续运行，Web 控制台/MQTT 不受影响）");
    }
    log_heap("ui_dashboard");

    /* 9) 本地 Web 控制台（状态页 / GPIO / TF 日志）+ mDNS */
    if (web_console_init() != ESP_OK) {
        ESP_LOGW(TAG, "Web 控制台启动失败（系统继续运行）");
    }
    log_heap("web_console");

    ESP_LOGI(TAG, "==========================================================");
    ESP_LOGI(TAG, " 双网冗余 IoT 网关：双网 + 看板 + HA MQTT + TF 记录（方案 A）");
    ESP_LOGI(TAG, " 有线 ETH 优先，Wi-Fi 兜底；LCD 看板 + HA 通知/状态上报");
    ESP_LOGI(TAG, "==========================================================");
}