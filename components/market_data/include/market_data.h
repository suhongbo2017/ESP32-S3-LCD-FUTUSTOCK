#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MARKET_DATA_SYMBOL_MAX 8
#define MARKET_DATA_NAME_MAX   63
#define MARKET_DATA_TIME_MAX   20

typedef struct {
    char symbol[MARKET_DATA_SYMBOL_MAX + 1];
    char name[MARKET_DATA_NAME_MAX + 1];
    float cost;
    float quantity;
} market_holding_t;

typedef struct {
    float last;
    float change;
    float percent;
    float previous;
    char quote_time[MARKET_DATA_TIME_MAX + 1];
    bool valid;
} market_quote_t;

/** 初始化快照存储。 */
esp_err_t market_data_init(void);

/** 拉取持仓并立即拉取一次行情；调用应在网络已就绪后执行。 */
esp_err_t market_data_refresh(void);

/** 当前持仓/行情快照。返回只读指针，调用者不应跨刷新长期保存。 */
size_t market_data_holding_count(void);
const market_holding_t *market_data_holding(size_t index);
const market_quote_t *market_data_quote(size_t index);

/** 将持仓和行情复制到调用者缓冲区，适用于 LVGL 等异步读取方。 */
esp_err_t market_data_copy_snapshot(market_holding_t *holdings,
                                    market_quote_t *quotes,
                                    size_t capacity,
                                    size_t *count);

const char *market_data_last_error(void);
uint32_t market_data_last_success_ms(void);
uint16_t market_data_quote_ok_count(void);

#ifdef __cplusplus
}
#endif
