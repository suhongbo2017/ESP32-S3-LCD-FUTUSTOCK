#include "market_data.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "market_data";

#define RESPONSE_MAX        24576
#define QUOTE_PATH_MAX      1024
#define REQUEST_TIMEOUT_MS  12000

static market_holding_t s_holdings[CONFIG_MARKET_DATA_MAX_HOLDINGS];
static market_quote_t s_quotes[CONFIG_MARKET_DATA_MAX_HOLDINGS];
static size_t s_holding_count = 0;
static uint16_t s_quote_ok_count = 0;
static uint32_t s_last_success_ms = 0;
static char s_last_error[128] = "尚未同步";
static char s_quote_path[QUOTE_PATH_MAX];

/* esp_http_client event callback: only the response body is retained. */
typedef struct {
    char *buf;
    size_t used;
    bool overflow;
} response_buf_t;

static esp_err_t response_event(esp_http_client_event_t *evt)
{
    response_buf_t *response = (response_buf_t *)evt->user_data;
    if (response == NULL || evt->event_id != HTTP_EVENT_ON_DATA || evt->data_len <= 0) {
        return ESP_OK;
    }
    if (response->used + (size_t)evt->data_len >= RESPONSE_MAX) {
        response->overflow = true;
        return ESP_ERR_NO_MEM;
    }
    memcpy(response->buf + response->used, evt->data, (size_t)evt->data_len);
    response->used += (size_t)evt->data_len;
    response->buf[response->used] = '\0';
    return ESP_OK;
}

static void set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_last_error, sizeof(s_last_error), fmt, ap);
    va_end(ap);
}

static esp_err_t http_get(const char *url, const char *user, const char *password,
                          char *body, size_t body_size, int *status)
{
    response_buf_t response = {
        .buf = body,
        .used = 0,
        .overflow = false,
    };
    body[0] = '\0';

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = response_event,
        .user_data = &response,
        .timeout_ms = REQUEST_TIMEOUT_MS,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    if (user != NULL && user[0] != '\0') {
        config.username = user;
        config.password = password != NULL ? password : "";
        config.auth_type = HTTP_AUTH_TYPE_BASIC;
    }

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK && status != NULL) {
        *status = esp_http_client_get_status_code(client);
    }
    if (response.overflow && err == ESP_OK) {
        err = ESP_ERR_NO_MEM;
    }
    esp_http_client_cleanup(client);
    return err;
}

static bool copy_json_string(cJSON *object, const char *key, char *dst, size_t cap)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }
    snprintf(dst, cap, "%s", item->valuestring);
    return true;
}

static void normalize_symbol(const char *code, char *dst, size_t cap)
{
    const char *p = strrchr(code != NULL ? code : "", '.');
    p = p != NULL ? p + 1 : (code != NULL ? code : "");
    size_t n = 0;
    while (p[n] != '\0' && n + 1 < cap) {
        dst[n] = (char)toupper((unsigned char)p[n]);
        n++;
    }
    dst[n] = '\0';
}

static esp_err_t sync_holdings(void)
{
    char *body = (char *)malloc(RESPONSE_MAX);
    if (body == NULL) {
        set_error("持仓响应缓冲分配失败");
        return ESP_ERR_NO_MEM;
    }

    int status = 0;
    esp_err_t err = http_get(CONFIG_MARKET_DATA_API_URL,
                             CONFIG_MARKET_DATA_API_USER,
                             CONFIG_MARKET_DATA_API_PASSWORD,
                             body, RESPONSE_MAX, &status);
    if (err != ESP_OK) {
        set_error("持仓 HTTP 失败: %s", esp_err_to_name(err));
        free(body);
        return err;
    }
    if (status != 200) {
        set_error("持仓 HTTP 状态 %d", status);
        free(body);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) {
        set_error("持仓 JSON 解析失败");
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *positions = cJSON_GetObjectItemCaseSensitive(root, "positions");
    if (!cJSON_IsArray(positions)) {
        cJSON_Delete(root);
        set_error("响应缺少 positions");
        return ESP_ERR_INVALID_RESPONSE;
    }

    market_holding_t staged[CONFIG_MARKET_DATA_MAX_HOLDINGS];
    size_t count = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, positions) {
        cJSON *qty_item = cJSON_GetObjectItemCaseSensitive(item, "qty");
        double qty = cJSON_IsNumber(qty_item) ? qty_item->valuedouble : 0.0;
        if (qty <= 0.0 || count >= CONFIG_MARKET_DATA_MAX_HOLDINGS) {
            continue;
        }
        char code[32] = {0};
        if (!copy_json_string(item, "code", code, sizeof(code))) {
            continue;
        }
        memset(&staged[count], 0, sizeof(staged[count]));
        normalize_symbol(code, staged[count].symbol, sizeof(staged[count].symbol));
        if (staged[count].symbol[0] == '\0') {
            continue;
        }
        if (!copy_json_string(item, "name", staged[count].name, sizeof(staged[count].name))) {
            snprintf(staged[count].name, sizeof(staged[count].name), "%s", staged[count].symbol);
        }
        cJSON *cost_item = cJSON_GetObjectItemCaseSensitive(item, "avg_cost");
        staged[count].cost = cJSON_IsNumber(cost_item) ? (float)cost_item->valuedouble : -1.0f;
        staged[count].quantity = (float)qty;
        count++;
    }
    cJSON_Delete(root);
    if (count == 0) {
        set_error("没有有效持仓");
        return ESP_ERR_NOT_FOUND;
    }

    memcpy(s_holdings, staged, count * sizeof(staged[0]));
    s_holding_count = count;
    ESP_LOGI(TAG, "持仓同步成功：%u 只", (unsigned)count);
    return ESP_OK;
}

static bool is_timestamp(const char *value)
{
    return value != NULL && strlen(value) >= 16 &&
           isdigit((unsigned char)value[0]) && isdigit((unsigned char)value[1]) &&
           isdigit((unsigned char)value[2]) && isdigit((unsigned char)value[3]) &&
           value[4] == '-' && value[7] == '-' && value[10] == ' ' && value[13] == ':';
}

static bool parse_float(const char *value, float *out)
{
    if (value == NULL || value[0] == '\0') return false;
    char *end = NULL;
    float parsed = strtof(value, &end);
    if (end == value || !isfinite(parsed)) return false;
    *out = parsed;
    return true;
}

static bool parse_quote_line(char *line)
{
    char *p = line;
    while (*p == ' ' || *p == '\r' || *p == '\n') p++;
    if (strncmp(p, "v_us", 4) != 0) return false;
    char *quote = strchr(p, '"');
    if (quote == NULL) return false;

    char symbol[MARKET_DATA_SYMBOL_MAX + 1] = {0};
    size_t symbol_len = 0;
    for (char *s = p + 4; s < quote && *s != '=' && symbol_len < MARKET_DATA_SYMBOL_MAX; s++) {
        symbol[symbol_len++] = (char)toupper((unsigned char)*s);
    }
    symbol[symbol_len] = '\0';

    size_t index = 0;
    while (index < s_holding_count && strcmp(s_holdings[index].symbol, symbol) != 0) index++;
    if (index == s_holding_count) return false;

    char *body = quote + 1;
    char *end_quote = strrchr(body, '"');
    if (end_quote != NULL) *end_quote = '\0';
    char *fields[80];
    int field_count = 0;
    fields[field_count++] = body;
    for (char *s = body; *s != '\0' && field_count < 80; s++) {
        if (*s == '~') {
            *s = '\0';
            fields[field_count++] = s + 1;
        }
    }
    if (field_count < 6) return false;

    float last = 0.0f;
    float previous = 0.0f;
    if (!parse_float(fields[3], &last) || last <= 0.0f || !parse_float(fields[4], &previous)) return false;

    int anchor = -1;
    for (int i = 0; i < field_count; i++) {
        if (is_timestamp(fields[i])) {
            anchor = i;
            break;
        }
    }
    market_quote_t *quote_out = &s_quotes[index];
    quote_out->last = last;
    quote_out->previous = previous;
    quote_out->change = previous > 0.0f ? last - previous : NAN;
    quote_out->percent = previous > 0.0f ? quote_out->change / previous * 100.0f : NAN;
    quote_out->quote_time[0] = '\0';
    if (anchor >= 0 && anchor + 2 < field_count) {
        parse_float(fields[anchor + 1], &quote_out->change);
        parse_float(fields[anchor + 2], &quote_out->percent);
        snprintf(quote_out->quote_time, sizeof(quote_out->quote_time), "%s", fields[anchor]);
    }
    quote_out->valid = true;
    return true;
}

static esp_err_t fetch_quotes(void)
{
    s_quote_ok_count = 0;
    memset(s_quotes, 0, sizeof(s_quotes));
    s_quote_path[0] = '\0';
    snprintf(s_quote_path, sizeof(s_quote_path), "http://qt.gtimg.cn/q=");
    for (size_t i = 0; i < s_holding_count; i++) {
        size_t used = strlen(s_quote_path);
        snprintf(s_quote_path + used, sizeof(s_quote_path) - used, "%s%s",
                 i == 0 ? "us" : ",us", s_holdings[i].symbol);
    }

    char *body = (char *)malloc(RESPONSE_MAX);
    if (body == NULL) {
        set_error("行情响应缓冲分配失败");
        return ESP_ERR_NO_MEM;
    }
    int status = 0;
    esp_err_t err = http_get(s_quote_path, NULL, NULL, body, RESPONSE_MAX, &status);
    if (err != ESP_OK || status != 200) {
        set_error("行情 HTTP 失败: %s/%d", esp_err_to_name(err), status);
        free(body);
        return err != ESP_OK ? err : ESP_FAIL;
    }

    char *cursor = body;
    while (*cursor != '\0') {
        char *line_end = strchr(cursor, '\n');
        if (line_end != NULL) *line_end = '\0';
        if (parse_quote_line(cursor)) s_quote_ok_count++;
        if (line_end == NULL) break;
        cursor = line_end + 1;
    }
    free(body);
    if (s_quote_ok_count == 0) {
        set_error("行情无有效数据");
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "行情成功：%u/%u 只", (unsigned)s_quote_ok_count, (unsigned)s_holding_count);
    return ESP_OK;
}

esp_err_t market_data_init(void)
{
    s_holding_count = 0;
    s_quote_ok_count = 0;
    s_last_success_ms = 0;
    s_last_error[0] = '\0';
    return ESP_OK;
}

esp_err_t market_data_refresh(void)
{
#if !CONFIG_MARKET_DATA_ENABLED
    set_error("market_data 未启用");
    return ESP_ERR_NOT_SUPPORTED;
#else
    int64_t started = esp_timer_get_time();
    esp_err_t err = sync_holdings();
    if (err == ESP_OK) err = fetch_quotes();
    if (err == ESP_OK) {
        s_last_success_ms = (uint32_t)(esp_timer_get_time() / 1000);
        snprintf(s_last_error, sizeof(s_last_error), "OK");
    }
    ESP_LOGI(TAG, "刷新完成：%s，耗时 %lldms", err == ESP_OK ? "成功" : s_last_error,
             (long long)((esp_timer_get_time() - started) / 1000));
    return err;
#endif
}

size_t market_data_holding_count(void) { return s_holding_count; }
const market_holding_t *market_data_holding(size_t index)
{
    return index < s_holding_count ? &s_holdings[index] : NULL;
}
const market_quote_t *market_data_quote(size_t index)
{
    return index < s_holding_count ? &s_quotes[index] : NULL;
}
const char *market_data_last_error(void) { return s_last_error; }
uint32_t market_data_last_success_ms(void) { return s_last_success_ms; }
uint16_t market_data_quote_ok_count(void) { return s_quote_ok_count; }
