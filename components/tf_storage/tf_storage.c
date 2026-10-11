/**
 * @file tf_storage.c
 * @brief TF(microSD) 卡历史记录实现（SDMMC 4-bit + FatFS，按天 CSV）
 *
 * 设计要点：
 *  - 挂载失败不阻塞：s_ready=false，tf_storage_log() 直接丢弃并（限频）告警
 *  - 调用者零阻塞：格式化成整行 → 入队 → 独立任务落盘
 *  - 每次写入即时 open/append/close：写入频率低（约 1 条/15s），换来断电安全与自动按天分文件
 *  - 时间来源：SNTP（由 app_main 启动）；未同步时退化为 uptime 秒，文件名用 log_uptime.csv
 */
#include "tf_storage.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "board_pins.h"

static const char *TAG = "tf_storage";

#define TF_LINE_MAX   192
#define TF_WRITE_STACK 4096
#define TF_WRITE_PRIO  3

typedef struct {
    char line[TF_LINE_MAX];
} tf_rec_t;

static sdmmc_card_t  *s_card  = NULL;
static bool           s_ready = false;
static QueueHandle_t  s_q     = NULL;
static char           s_dir[64];
static uint32_t       s_written = 0;   /*!< 已成功落盘条数 */
static char           s_last_path[128] = { 0 };  /*!< 上一次写入的文件（用于按天/回退时提示切换） */

uint32_t tf_storage_written_count(void)
{
    return s_written;
}

const char *tf_storage_log_dir(void)
{
    return s_dir;
}

const char *tf_storage_mount_point(void)
{
    return CONFIG_TF_STORAGE_MOUNT_POINT;
}

bool tf_storage_is_ready(void)
{
    return s_ready;
}

/* ------------------------------------------------------------------ */
/* 时间与路径                                                          */
/* ------------------------------------------------------------------ */

/** 系统时间是否已同步（> 2023-11-14，足以区分 1970/未同步） */
static bool time_valid(void)
{
    return time(NULL) > 1700000000;
}

/** 生成当天日志文件路径；时间无效时用 log_uptime.csv */
static void build_log_path(char *out, size_t n)
{
    if (time_valid()) {
        time_t now = time(NULL);
        struct tm tmv;
        localtime_r(&now, &tmv);
        snprintf(out, n, "%s/log_%04d%02d%02d.csv",
                 s_dir, tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    } else {
        snprintf(out, n, "%s/log_uptime.csv", s_dir);
    }
}

/** 生成时间戳字段 */
static void build_time_field(char *out, size_t n)
{
    if (time_valid()) {
        time_t now = time(NULL);
        struct tm tmv;
        localtime_r(&now, &tmv);
        strftime(out, n, "%Y-%m-%d %H:%M:%S", &tmv);
    } else {
        snprintf(out, n, "uptime=%llds", (long long)(esp_timer_get_time() / 1000000));
    }
}

/* ------------------------------------------------------------------ */
/* 落盘                                                                */
/* ------------------------------------------------------------------ */

static void write_record(const char *line)
{
    char path[128];
    build_log_path(path, sizeof(path));

    bool is_new = false;
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        is_new = true;
    } else {
        fclose(fp);
    }

    fp = fopen(path, "a");
    if (fp == NULL) {
        ESP_LOGW(TAG, "打开日志失败：%s（%s）", path, strerror(errno));
        return;
    }
    if (is_new) {
        fprintf(fp, "time,type,fields\n");   /* 新文件写表头 */
    }
    fputs(line, fp);
    fputc('\n', fp);
    fclose(fp);

    /* 写入计数/文件切换日志（便于确认按天分文件与落盘） */
    s_written++;
    if (s_last_path[0] == '\0' || strcmp(s_last_path, path) != 0) {
        strncpy(s_last_path, path, sizeof(s_last_path) - 1);
        ESP_LOGI(TAG, "日志文件 -> %s", path);
    } else if ((s_written % 10) == 0) {
        ESP_LOGI(TAG, "TF 记录：已写入 %lu 条", (unsigned long)s_written);
    }
}

static void tf_writer_task(void *arg)
{
    (void)arg;
    tf_rec_t rec;
    for (;;) {
        if (xQueueReceive(s_q, &rec, portMAX_DELAY) == pdTRUE) {
            write_record(rec.line);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

esp_err_t tf_storage_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
#if !CONFIG_TF_STORAGE_ENABLE
    ESP_LOGW(TAG, "TF 记录已在 Kconfig 中关闭");
    return ESP_ERR_NOT_SUPPORTED;
#else
    /* 目录：<mount>/<dir> */
    if (CONFIG_TF_STORAGE_LOG_DIR[0] != '\0') {
        snprintf(s_dir, sizeof(s_dir), "%s/%s",
                 CONFIG_TF_STORAGE_MOUNT_POINT, CONFIG_TF_STORAGE_LOG_DIR);
    } else {
        snprintf(s_dir, sizeof(s_dir), "%s", CONFIG_TF_STORAGE_MOUNT_POINT);
    }

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
#if CONFIG_TF_STORAGE_FORMAT_IF_MOUNT_FAILED
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif
        .max_files              = 4,
        .allocation_unit_size   = 16 * 1024,
    };
#if CONFIG_TF_STORAGE_FORMAT_IF_MOUNT_FAILED
    ESP_LOGW(TAG, "已启用“挂载失败自动格式化”：无法承载有效文件系统的卡将被格式化（数据会丢失！）");
#endif

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;   /* 40MHz；不稳可改 SDMMC_FREQ_DEFAULT */

    /* SDMMC Slot 0：本板引脚与 ESP32-S3 默认 IO_MUX 一致（见 PINMAP §2.3） */
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk   = BOARD_SD_PIN_CLK;
    slot.cmd   = BOARD_SD_PIN_CMD;
    slot.d0    = BOARD_SD_PIN_D0;
    slot.d1    = BOARD_SD_PIN_D1;
    slot.d2    = BOARD_SD_PIN_D2;
    slot.d3    = BOARD_SD_PIN_D3;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(CONFIG_TF_STORAGE_MOUNT_POINT,
                                            &host, &slot, &mcfg, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "TF 卡挂载失败（%s）：不影响其它功能，历史记录功能关闭",
                 esp_err_to_name(ret));
        s_card = NULL;
        return ret;
    }

    /* 建日志目录；不可用时回退到挂载根目录（带探测，避免后续 fopen 失败） */
    bool dir_ok = false;
    if (strcmp(s_dir, CONFIG_TF_STORAGE_MOUNT_POINT) == 0) {
        dir_ok = true;
    } else {
        int mk_ret = mkdir(s_dir, 0775);
        if (mk_ret == 0 || errno == EEXIST) {
            dir_ok = true;
        } else {
            ESP_LOGW(TAG, "创建目录 %s 失败（errno=%d）", s_dir, errno);
        }
    }
    if (dir_ok) {
        char probe[160];
        snprintf(probe, sizeof(probe), "%s/probe.tmp", s_dir);   /* 8.3 安全名 */
        FILE *pf = fopen(probe, "w");
        if (pf == NULL) {
            ESP_LOGW(TAG, "目录 %s 不可写（errno=%d）", s_dir, errno);
            dir_ok = false;
        } else {
            fclose(pf);
            remove(probe);
        }
    }
    if (!dir_ok) {
        ESP_LOGW(TAG, "回退到挂载根目录 %s", CONFIG_TF_STORAGE_MOUNT_POINT);
        snprintf(s_dir, sizeof(s_dir), "%s", CONFIG_TF_STORAGE_MOUNT_POINT);
    }

    s_q = xQueueCreate(CONFIG_TF_STORAGE_QUEUE_LEN, sizeof(tf_rec_t));
    if (s_q == NULL) {
        ESP_LOGE(TAG, "写队列创建失败");
        return ESP_ERR_NO_MEM;
    }
    xTaskCreate(tf_writer_task, "tf_write", TF_WRITE_STACK, NULL, TF_WRITE_PRIO, NULL);

    s_ready = true;
    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(TAG, "TF 卡已挂载：%s（日志目录 %s）",
             CONFIG_TF_STORAGE_MOUNT_POINT, s_dir);
    return ESP_OK;
#endif
}

esp_err_t tf_storage_log(const char *type, const char *fmt, ...)
{
    if (!s_ready || s_q == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    tf_rec_t rec;
    char   tfield[32];
    char   fields[TF_LINE_MAX - 64];

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(fields, sizeof(fields), fmt, ap);
    va_end(ap);

    /* CSV 安全：字段中的逗号/换行替换为空格 */
    for (char *p = fields; *p != '\0'; p++) {
        if (*p == ',' || *p == '\n' || *p == '\r') {
            *p = ' ';
        }
    }
    build_time_field(tfield, sizeof(tfield));
    snprintf(rec.line, sizeof(rec.line), "%s,%s,%s", tfield, type ? type : "-", fields);

    if (xQueueSend(s_q, &rec, 0) != pdTRUE) {
        static int64_t last_warn_us;
        int64_t now = esp_timer_get_time();
        if (now - last_warn_us > 5 * 1000000) {   /* 告警限频 5s */
            last_warn_us = now;
            ESP_LOGW(TAG, "写队列已满，丢弃记录（SD 卡慢或已拔出？）");
        }
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}