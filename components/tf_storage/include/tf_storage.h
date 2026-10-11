/**
 * @file tf_storage.h
 * @brief TF(microSD) 卡历史记录：按天落盘 CSV + 日报
 *
 * 特性：
 *  - SDMMC 4-bit（Slot 0 默认引脚：CLK=36 CMD=35 D0=37 D1=38 D2=33 D3=34，见 PINMAP）
 *  - 挂载失败（无卡/格式不支持）不阻塞系统：tf_storage_is_ready() 返回 false，业务照常运行
 *  - 生产者在任意任务/回调中调用 tf_storage_log()，内部「队列 + 落盘任务」解耦，不阻塞调用者
 *  - 文件名按天：/sdcard/log_YYYYMMDD.csv（时间无效时退化为 log_uptime.csv）
 *  - 每条记录格式：时间,类型,字段...（字段中的逗号/换行会被替换为空格，保证 CSV 合法）
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 挂载 TF 卡（幂等；失败只记录日志并返回错误，不影响系统其它功能） */
esp_err_t tf_storage_init(void);

/** TF 卡是否已挂载可用 */
bool tf_storage_is_ready(void);

/**
 * @brief 追加一条记录（线程安全，非阻塞；队列满则丢弃并告警）
 *
 * @param type 记录类型（如 "sensor" / "net" / "notify" / "boot"）
 * @param fmt  字段格式（不要含逗号，逗号会被替换为空格）
 */
esp_err_t tf_storage_log(const char *type, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/** 挂载点（未挂载时返回 Kconfig 配置值） */
const char *tf_storage_mount_point(void);

/** 已成功落盘的记录条数（用于验证/诊断） */
uint32_t tf_storage_written_count(void);

/** 当前日志目录（未挂载时返回配置路径） */
const char *tf_storage_log_dir(void);

#ifdef __cplusplus
}
#endif