/**
 * @file net_eth.c
 * @brief W5500 有线以太网初始化与事件处理（esp_eth + SPI）
 *
 * 说明：
 *  - W5500 驱动已内置于 ESP-IDF 的 esp_eth 组件（IDF >= 5.1）。
 *  - 本文件按 IDF >= 5.2 的 API 编写（esp_eth_driver_new 等）。
 *  - RST(GPIO15) 与 LCD_RST 共用一根线：上电时由本组件完成一次复位，
 *    该复位同时作用于 LCD，属正常现象；后续 LCD 组件不得再抢占此引脚。
 *  - 若 `esp_eth_mac_new_w5500` 在你的 IDF 版本中为旧式签名
 *    （形参为 spi_device_handle_t），请对照 README「API 兼容性」一节调整。
 */
#include "net_eth.h"

#include <string.h>

#include "esp_err.h"
#include "esp_check.h"          /* ESP_RETURN_ON_ERROR */
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_eth_netif_glue.h"  /* esp_eth_new_netif_glue */
#include "esp_mac.h"            /* esp_read_mac：为 W5500 设置 MAC */
#include "esp_eth_mac_spi.h"    /* IDF v5.5：W5500 MAC 声明（esp_eth_mac_new_w5500） */
#include "esp_eth_phy.h"        /* IDF v5.5：W5500 PHY 声明（esp_eth_phy_new_w5500） */
#include "driver/spi_master.h"
#include "board_pins.h"

static const char *TAG = "net_eth";

/* esp_eth 内部检测网线插拔的周期 */
#define ETH_LINK_CHECK_PERIOD_MS   2000

static esp_netif_t      *s_netif    = NULL;  /*!< 以太网 netif 实例 */
static esp_eth_handle_t  s_eth      = NULL;  /*!< 以太网驱动句柄 */
static net_eth_config_t  s_cfg;              /*!< 当前生效的网络配置 */
static volatile bool     s_link_up  = false; /*!< 物理链路是否 UP */
static volatile bool     s_ip_ready = false; /*!< 是否已获得（或已配置）IP */

/* ---------------------------------------------------------------------------
 * 事件回调
 * ------------------------------------------------------------------------- */

/** 以太网驱动事件（链路插拔等） */
static void eth_event_handler(void *arg, esp_event_base_t base,
                              int32_t event_id, void *event_data)
{
    (void)arg; (void)base; (void)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "以太网驱动已启动");
        break;

    case ETHERNET_EVENT_CONNECTED:
        s_link_up = true;
        ESP_LOGI(TAG, "网线已插入，准备获取 IP...");

        if (s_cfg.use_static_ip) {
            /* 静态 IP：阻断 DHCP 干扰，重新落定地址（静态配置立即可用） */
            esp_netif_dhcpc_stop(s_netif);
            esp_netif_set_ip_info(s_netif, &s_cfg.ip_info);
            s_ip_ready = true;
            ESP_LOGI(TAG, "已应用静态 IP");
        } else {
            /* DHCP：若 esp_netif 未自动启动 DHCP 客户端，这里兜底启动一次 */
            esp_err_t err = esp_netif_dhcpc_start(s_netif);
            if (err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
                ESP_LOGD(TAG, "esp_netif_dhcpc_start 返回: %s（可忽略）", esp_err_to_name(err));
            }
        }
        break;

    case ETHERNET_EVENT_DISCONNECTED:
        s_link_up  = false;
        s_ip_ready = false;
        ESP_LOGW(TAG, "网线断开（上行切换由 net_manager 决策）");
        break;

    case ETHERNET_EVENT_STOP:
        s_link_up  = false;
        s_ip_ready = false;
        ESP_LOGW(TAG, "以太网驱动已停止");
        break;

    default:
        break;
    }
}

/** IP 事件（DHCP 成功/失败） */
static void ip_event_handler(void *arg, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    (void)arg; (void)base;

    if (event_id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
        char ip[16] = {0}, mask[16] = {0}, gw[16] = {0};
        esp_ip4addr_ntoa(&evt->ip_info.ip,      ip,   sizeof(ip));
        esp_ip4addr_ntoa(&evt->ip_info.netmask, mask, sizeof(mask));
        esp_ip4addr_ntoa(&evt->ip_info.gw,      gw,   sizeof(gw));
        s_ip_ready = true;
        ESP_LOGI(TAG, "以太网已获得 IP：%s 掩码：%s 网关：%s", ip, mask, gw);
    } else if (event_id == IP_EVENT_ETH_LOST_IP) {
        s_ip_ready = false;
        ESP_LOGW(TAG, "以太网 IP 已丢失");
    }
}

/* ---------------------------------------------------------------------------
 * 对外接口
 * ------------------------------------------------------------------------- */

void net_eth_get_default_config(net_eth_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
#if defined(CONFIG_NET_ETH_USE_STATIC_IP)
    cfg->use_static_ip = true;
    esp_netif_str_to_ip4(CONFIG_NET_ETH_STATIC_IP, &cfg->ip_info.ip);
    esp_netif_str_to_ip4(CONFIG_NET_ETH_NETMASK,   &cfg->ip_info.netmask);
    esp_netif_str_to_ip4(CONFIG_NET_ETH_GATEWAY,   &cfg->ip_info.gw);
    esp_netif_str_to_ip4(CONFIG_NET_ETH_DNS,       &cfg->dns_server);
#endif
}

esp_err_t net_eth_init(const net_eth_config_t *cfg)
{
    if (cfg != NULL) {
        s_cfg = *cfg;                          /* 调用方指定配置（如持久化的用户设置） */
    } else {
        net_eth_get_default_config(&s_cfg);    /* 未指定则取 Kconfig 默认 */
    }

    /* 1) 初始化 SPI 总线。W5500 独占 SPI2，引脚即 FSPI 默认 IO_MUX（11/12/13），
          无需 GPIO Matrix 路由；LCD 届时走另一组引脚路由到 SPI2/SPI3。 */
    spi_bus_config_t buscfg = {
        .sclk_io_num     = BOARD_ETH_PIN_SCLK,
        .mosi_io_num     = BOARD_ETH_PIN_MOSI,
        .miso_io_num     = BOARD_ETH_PIN_MISO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4096,               /* W5500 收发缓冲 16KB，按帧分段读写 */
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BOARD_ETH_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO),
                        TAG, "SPI 总线初始化失败");

    /* 2) SPI 器件参数。器件由 esp_eth 内部 add_device，这里只提供时序/速率等参数。 */
    spi_device_interface_config_t devcfg = {
        .mode             = 0,                 /* W5500 支持 Mode 0/3，取 0 */
        .clock_speed_hz   = CONFIG_NET_ETH_SPI_CLK_HZ,
        .spics_io_num     = BOARD_ETH_PIN_CS,
        .queue_size       = 20,
        /* 不设 cs_ena_pretrans/posttrans（与 IDF esp_eth 官方 W5500 例程一致） */
    };

    /* 3) 创建 W5500 MAC / PHY（二者均内置于 esp_eth 组件） */
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_w5500_config_t w5500_config = {
        .int_gpio_num   = BOARD_ETH_PIN_INT,   /* GPIO14，W5500 中断（低有效） */
        .poll_period_ms = 0,                   /* 使用中断，不轮询 */
        .spi_host_id    = BOARD_ETH_SPI_HOST,  /* SPI2 */
        .spi_devcfg     = &devcfg,             /* v5.5 字段名为 spi_devcfg（指针） */
    };
    /* IDF v5.5 签名：esp_eth_mac_new_w5500(w5500_config, mac_config) —— w5500_config 在前 */
    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    if (mac == NULL) {
        ESP_LOGE(TAG, "W5500 MAC 创建失败（请在 menuconfig 确认 CONFIG_ETH_USE_SPI_ETHERNET=y）");
        return ESP_FAIL;
    }

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr       = -1;            /* W5500 内部 PHY，无地址概念 */
    /* GPIO15：驱动 install 时先做硬件复位（拉低 100us → 拉高）。
       必须设置，否则 W5500 可能处于复位/未就绪态，SPI 读到版本号 0x00。
       注：GPIO15 与 LCD_RST 共用，此处复位会同时复位 LCD，属正常。 */
    phy_config.reset_gpio_num = BOARD_ETH_PIN_RST;
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);
    if (phy == NULL) {
        ESP_LOGE(TAG, "W5500 PHY 创建失败");
        return ESP_FAIL;
    }

    /* 4) 创建以太网 netif（描述符 "eth"） */
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&netif_cfg);
    if (s_netif == NULL) {
        ESP_LOGE(TAG, "以太网 netif 创建失败");
        return ESP_FAIL;
    }

    /* 5) 注册事件处理器（先于驱动启动，避免漏事件） */
    ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                                   eth_event_handler, NULL),
                        TAG, "ETH_EVENT 处理器注册失败");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                                   ip_event_handler, NULL),
                        TAG, "IP_EVENT(ETH_GOT_IP) 处理器注册失败");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP,
                                                   ip_event_handler, NULL),
                        TAG, "IP_EVENT(ETH_LOST_IP) 处理器注册失败");

    /* 6) DHCP / 静态 IP 预配置（与官方 ethernet/basic 示例一致的时序） */
    if (s_cfg.use_static_ip) {
        esp_netif_dhcpc_stop(s_netif);
        esp_netif_set_ip_info(s_netif, &s_cfg.ip_info);
        esp_netif_dns_info_t dns = {
            .ip = {
                .type     = ESP_IPADDR_TYPE_V4,
                .u_addr.ip4 = s_cfg.dns_server,
            },
        };
        /* IDF v5.5：旧的 esp_netif_dns_setserver 已移除，改用 esp_netif_set_dns_info */
        esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns);
    }
    /* 局域网主机名（随 DHCP 请求通告，方便路由器识别设备） */
    if (esp_netif_set_hostname(s_netif, CONFIG_LWIP_LOCAL_HOSTNAME) != ESP_OK) {
        ESP_LOGW(TAG, "设置主机名失败（可忽略）");
    }

    /* 7) 安装 GPIO ISR 服务：W5500 使用 GPIO14 中断，驱动内部会调用
          gpio_isr_handler_add()，必须先安装 ISR 服务，否则中断模式收不到包。
          重复安装会返回 ESP_ERR_INVALID_STATE，属正常，忽略。 */
    esp_err_t isr_ret = gpio_install_isr_service(0);
    if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "gpio_install_isr_service 返回 %s（可忽略）", esp_err_to_name(isr_ret));
    }

    /* 8) 安装驱动（IDF v5.5 标准 API：esp_eth_config_t + esp_eth_driver_install） */
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    eth_config.check_link_period_ms = ETH_LINK_CHECK_PERIOD_MS;
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&eth_config, &s_eth),
                        TAG, "以太网驱动安装失败");

    /* 设置 MAC 地址：W5500 无出厂 MAC，默认读出 00:00:00:00:00:00 会导致 DHCP 异常，
       这里用 ESP32 芯片的以太网 MAC 派生一个合法地址。 */
    uint8_t eth_mac[6] = {0};
    esp_read_mac(eth_mac, ESP_MAC_ETH);
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(s_eth, ETH_CMD_S_MAC_ADDR, eth_mac),
                        TAG, "设置以太网 MAC 失败");

    /* 9) 挂接 netif（esp_eth 事件经 glue 转发给 esp_netif/LWIP） */
    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(s_eth);
    if (glue == NULL) {
        ESP_LOGE(TAG, "netif glue 创建失败");
        return ESP_FAIL;
    }
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_netif, glue), TAG, "netif 挂接失败");

    /* 10) 启动驱动 */
    ESP_RETURN_ON_ERROR(esp_eth_start(s_eth), TAG, "以太网启动失败");

    ESP_LOGI(TAG, "W5500 有线以太网初始化完成（SPI 时钟 %lu Hz，模式 %s）",
             (unsigned long)CONFIG_NET_ETH_SPI_CLK_HZ,
             s_cfg.use_static_ip ? "静态IP" : "DHCP");
    return ESP_OK;
}

esp_netif_t *net_eth_get_netif(void)
{
    return s_netif;
}

bool net_eth_is_link_up(void)
{
    return s_link_up;
}

bool net_eth_is_ready(void)
{
    return s_link_up && s_ip_ready;
}

esp_err_t net_eth_get_ip_info(esp_netif_ip_info_t *info)
{
    if (s_netif == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_netif_get_ip_info(s_netif, info);
}