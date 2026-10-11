/**
 * @file net_manager.h
 * @brief 双网冗余管理器对外接口（优先/切换框架）
 *
 * 职责：周期评估有线(W5500)与 WiFi 两条上行链路的就绪状态，
 *      按优先级把 lwIP 默认接口（含默认路由）切到最佳链路，
 *      并在切换时通知上层业务（MQTT / LCD 看板等）。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 上行链路类型 */
typedef enum {
    NET_BACKEND_NONE = 0,   /*!< 暂无可用链路（离线） */
    NET_BACKEND_ETH,        /*!< W5500 有线以太网（主链路，默认优先） */
    NET_BACKEND_WIFI,       /*!< Wi-Fi（备用链路） */
} net_backend_t;

/** 网络状态快照 */
typedef struct {
    net_backend_t active;         /*!< 当前启用的上行链路 */
    bool          eth_ready;      /*!< 有线可用：链路 UP 且已获(配置) IP */
    bool          wifi_ready;     /*!< Wi-Fi 可用：已关联且已获 IP */
    char          active_ip[16];  /*!< 当前上行链路的 IP（点分十进制，"0.0.0.0" 表示无可显示） */
} net_manager_status_t;

/** 链路切换通知回调（在管理器任务上下文中执行，勿做耗时操作） */
typedef void (*net_manager_cb_t)(net_manager_status_t *status, void *user_ctx);

/** @brief 启动双网管理器（创建轮询任务）。应在 net_eth_init/net_wifi_init 之后调用。 */
esp_err_t net_manager_init(void);

/** @brief 获取当前网络状态快照 */
net_manager_status_t net_manager_get_status(void);

/** @brief 获取当前活跃上行链路 */
net_backend_t net_manager_get_active(void);

/** @brief 注册链路切换监听器（最多 NET_MANAGER_MAX_LISTENERS 个） */
esp_err_t net_manager_register_listener(net_manager_cb_t cb, void *user_ctx);

/** @brief 链路类型的可读名称（"ETH" / "WiFi" / "None"） */
const char *net_manager_backend_name(net_backend_t backend);

#ifdef __cplusplus
}
#endif