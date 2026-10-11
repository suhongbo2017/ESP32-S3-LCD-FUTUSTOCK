/**
 * @file sensor_history.h
 * @brief 温湿度历史环形缓冲（供 Web 曲线图使用）
 *
 * 由 ha_mqtt 在收到传感器数据时写入；web_console 读取用于绘图。
 * 只在 RAM 中保留最近 N 条（掉电即清空）。
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int64_t t_us;    /*!< 接收时刻（esp_timer，微秒） */
    float   temp;    /*!< 温度 ℃ */
    float   hum;     /*!< 湿度 % */
} sensor_sample_t;

/** 初始化环形缓冲（幂等） */
void sensor_history_init(void);

/** 追加一条样本（线程安全，非阻塞） */
void sensor_history_add(float temp, float hum);

/** 追加一条“过去 age_us 微秒”的样本（开机从 TF 日志回灌历史用） */
void sensor_history_add_ts(float temp, float hum, int64_t age_us);

/**
 * @brief 取历史样本（时间升序）
 * @param out 输出缓冲
 * @param max 最多条数
 * @return 实际写入条数
 */
int sensor_history_get(sensor_sample_t *out, int max);

/** 当前样本总数 */
int sensor_history_count(void);

#ifdef __cplusplus
}
#endif