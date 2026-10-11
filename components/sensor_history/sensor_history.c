/**
 * @file sensor_history.c
 * @brief 温湿度历史环形缓冲实现
 */
#include "sensor_history.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define HIST_CAP 240       /*!< 约 1 小时（传感器 15s/条） */

static sensor_sample_t s_buf[HIST_CAP];
static int             s_head  = 0;    /*!< 下一个写入位置 */
static int             s_count = 0;
static SemaphoreHandle_t s_lock = NULL;

void sensor_history_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
}

void sensor_history_add(float temp, float hum)
{
    sensor_history_add_ts(temp, hum, 0);
}

void sensor_history_add_ts(float temp, float hum, int64_t age_us)
{
    if (s_lock == NULL) {
        sensor_history_init();
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    s_buf[s_head].t_us = esp_timer_get_time() - age_us;
    s_buf[s_head].temp = temp;
    s_buf[s_head].hum  = hum;
    s_head = (s_head + 1) % HIST_CAP;
    if (s_count < HIST_CAP) {
        s_count++;
    }
    xSemaphoreGive(s_lock);
}

int sensor_history_get(sensor_sample_t *out, int max)
{
    if (out == NULL || max <= 0) {
        return 0;
    }
    if (s_lock == NULL) {
        return 0;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return 0;
    }
    int n = (s_count < max) ? s_count : max;
    /* 取最近 n 条，按时间升序输出 */
    int start = (s_head - n + HIST_CAP * 2) % HIST_CAP;
    for (int i = 0; i < n; i++) {
        out[i] = s_buf[(start + i) % HIST_CAP];
    }
    xSemaphoreGive(s_lock);
    return n;
}

int sensor_history_count(void)
{
    return s_count;
}