/**
 * @file lcd_st7735.c
 * @brief ST7735S 1.77" TFT（128x160）驱动：esp_lcd_panel_io_spi + 自写初始化命令序列
 *
 * 关键约束（务必遵守）：
 *  - RST(GPIO15) 与 W5500_RST 共用一条复位线，net_eth 上电时已做过硬件复位脉冲，
 *    会同时复位 LCD。因此本驱动 reset_gpio_num 必须为 -1，绝不由本驱动操作 GPIO15，
 *    否则会把正在工作的 W5500 一起复位。
 *  - reset() 在无 RST 引脚时退化为“软件复位”（经 SPI 发 SWRESET 命令，不触碰 GPIO），
 *    与 IDF st7789 官方驱动行为一致。
 *  - LCD 初始化必须在 net_eth_init() 之后调用（app_main 顺序已保证）。
 *  - LCD 独立挂 SPI3_HOST，避开 W5500 独占的 SPI2(FSPI 默认引脚)。
 *
 * 旋转 / 偏移模型：
 *  - ST7735S 显存 132(列) x 162(行)，面板 128x160。
 *  - 0°：列偏移 2、行偏移 1（RAM 128 有效列居中于 132，160 有效行居中于 162）。
 *  - 旋转由 MADCTL 位（MY/MX/MV）实现，MADCTL 只改变“扫描方向/交换”，
 *    窗口(CASET/RASET)仍在绝对 RAM 坐标下；因此 0°/180° 偏移不变，
 *    90°/270° 需把列/行偏移互换（宽高也随之互换）。
 *  - MIRROR_X/Y 在旋转基础上再翻转对应轴（不改变偏移）。
 *
 * 备查 MADCTL（ST7735S）：MY=0x80 MX=0x40 MV=0x20 ML=0x10 BGR=0x08 MH=0x04
 */
#include "lcd_st7735.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_compiler.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "board_pins.h"

static const char *TAG = "lcd_st7735";

/* ST7735S 私有命令（esp_lcd_panel_commands.h 未覆盖的部分） */
#define ST7735_CMD_FRMCTR1  0xB1   /*!< 正常模式帧率 */
#define ST7735_CMD_FRMCTR2  0xB2   /*!< 空闲模式帧率 */
#define ST7735_CMD_FRMCTR3  0xB3   /*!< 部分模式帧率 */
#define ST7735_CMD_INVCTR   0xB4   /*!< 显示反转控制 */
#define ST7735_CMD_PWCTR1   0xC0   /*!< 电源控制 1：AVDD/VRHP/VRHL */
#define ST7735_CMD_PWCTR2   0xC1   /*!< 电源控制 2：VGH/VGL */
#define ST7735_CMD_PWCTR3   0xC2   /*!< 电源控制 3：VGH/VGL 提升 */
#define ST7735_CMD_PWCTR4   0xC3   /*!< 电源控制 4：VMH/VML */
#define ST7735_CMD_PWCTR5   0xC4   /*!< 电源控制 5：VMH/VML 提升 */
#define ST7735_CMD_VMCTR1   0xC5   /*!< VCOM 控制 */
#define ST7735_CMD_GMCTRP1  0xE0   /*!< 正极性 Gamma */
#define ST7735_CMD_GMCTRN1  0xE1   /*!< 负极性 Gamma */

/* 诊断测试图用 RGB565（小端，绘制时按 ST7735S 大端交换后发送） */
#define TP_RED     0xF800
#define TP_GREEN   0x07E0
#define TP_BLUE    0x001F
#define TP_YELLOW  0xFFE0
#define TP_WHITE   0xFFFF
#define TP_BLACK   0x0000

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;            /*!< 恒为 -1（共享复位线约束） */
    int x_gap;                     /*!< 生效列偏移（含旋转查表） */
    int y_gap;                     /*!< 生效行偏移（含旋转查表） */
    int rotation;                  /*!< 旋转角（日志用） */
    uint32_t width;                /*!< 旋转后面板有效宽 */
    uint32_t height;               /*!< 旋转后面板有效高 */
    uint8_t fb_bits_per_pixel;     /*!< 固定 16（RGB565） */
    uint8_t madctl_val;            /*!< MADCTL 最终值（旋转+镜像+BGR） */
    bool color_invert;             /*!< 颜色反转开关 */
} st7735_panel_t;

/* ---------------------------------------------------------------------------
 * 几何计算：旋转 + 镜像 → MADCTL / 生效偏移 / 分辨率
 * ------------------------------------------------------------------------- */

/**
 * @brief 按旋转/镜像配置计算最终显示几何
 *
 * @param[in]  cfg    面板配置（含 rotation/mirror/基准偏移/分辨率）
 * @param[out] madctl MADCTL 值（不含 BGR）
 * @param[out] x_gap  生效列偏移
 * @param[out] y_gap  生效行偏移
 * @param[out] w, h   旋转后的分辨率
 */
static void lcd_st7735_compute_geometry(const lcd_st7735_config_t *cfg,
                                        uint8_t *madctl, int *x_gap, int *y_gap,
                                        uint32_t *w, uint32_t *h)
{
    int rot = cfg->rotation;
    bool horiz = (rot == 90 || rot == 270);     /* 横屏：宽高互换 */
    int ox = cfg->offset_x;
    int oy = cfg->offset_y;

    if (cfg->offset_manual) {
        /* 手工覆盖：所有旋转用同一组偏移（调试用） */
        *x_gap = ox;
        *y_gap = oy;
    } else {
        /* 自动查表：0°/180° 沿用基准；90°/270° 交换列/行偏移 */
        *x_gap = horiz ? oy : ox;
        *y_gap = horiz ? ox : oy;
    }

    /* 旋转 → MADCTL 基础位（ST7735S）：
       0°  : 0x00
       90° : MV|MX  (0x60)
       180°: MX|MY  (0xC0)
       270°: MV|MY  (0xA0) */
    switch (rot) {
    case 90:  *madctl = LCD_CMD_MV_BIT | LCD_CMD_MX_BIT; break;
    case 180: *madctl = LCD_CMD_MX_BIT | LCD_CMD_MY_BIT; break;
    case 270: *madctl = LCD_CMD_MV_BIT | LCD_CMD_MY_BIT; break;
    default:  *madctl = 0x00; break;
    }
    /* 镜像叠加 */
    if (cfg->mirror_x) {
        *madctl ^= LCD_CMD_MX_BIT;
    }
    if (cfg->mirror_y) {
        *madctl ^= LCD_CMD_MY_BIT;
    }

    *w = horiz ? cfg->v_res : cfg->h_res;
    *h = horiz ? cfg->h_res : cfg->v_res;
}

/* ---------------------------------------------------------------------------
 * 内部工具
 * ------------------------------------------------------------------------- */

static esp_err_t lcd_tx_cmd(esp_lcd_panel_io_handle_t io, uint8_t cmd,
                            const uint8_t *param, size_t param_len)
{
    return esp_lcd_panel_io_tx_param(io, cmd, param, param_len);
}

/* ---------------------------------------------------------------------------
 * esp_lcd_panel_t 接口实现
 * ------------------------------------------------------------------------- */

static esp_err_t panel_st7735_del(esp_lcd_panel_t *panel)
{
    st7735_panel_t *st7735 = __containerof(panel, st7735_panel_t, base);
    ESP_LOGD(TAG, "del st7735 panel @%p", st7735);
    free(st7735);
    return ESP_OK;
}

static esp_err_t panel_st7735_reset(esp_lcd_panel_t *panel)
{
    st7735_panel_t *st7735 = __containerof(panel, st7735_panel_t, base);

    /* 本板 RST 与 W5500 共用（reset_gpio_num 恒为 -1），只能做软件复位：
       SWRESET 命令经 SPI 送达 LCD 控制器，不触碰 GPIO15，
       不会影响正在工作的 W5500。 */
    if (st7735->reset_gpio_num >= 0) {
        /* 理论上不会走到这里（创建时已校验 -1）；保留硬件复位分支权限 */
        ESP_LOGE(TAG, "本驱动禁止操作复位引脚（与 W5500 共用 GPIO15）！");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(lcd_tx_cmd(st7735->io, LCD_CMD_SWRESET, NULL, 0),
                        TAG, "io tx param failed");
    vTaskDelay(pdMS_TO_TICKS(120));   /* ST7735S 规格：软件复位后等待 >= 120ms */

    return ESP_OK;
}

static esp_err_t panel_st7735_init(esp_lcd_panel_t *panel)
{
    st7735_panel_t *st7735 = __containerof(panel, st7735_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7735->io;

    /* 退出睡眠（上电后 LCD 默认睡眠、显示关闭） */
    ESP_RETURN_ON_ERROR(lcd_tx_cmd(io, LCD_CMD_SLPOUT, NULL, 0), TAG, "io tx param failed");
    vTaskDelay(pdMS_TO_TICKS(130));

    /* 帧率 */
    ESP_RETURN_ON_ERROR(lcd_tx_cmd(io, ST7735_CMD_FRMCTR1, (uint8_t[]){0x01, 0x2C, 0x2D}, 3), TAG, "frmctr1");
    ESP_RETURN_ON_ERROR(lcd_tx_cmd(io, ST7735_CMD_FRMCTR2, (uint8_t[]){0x01, 0x2C, 0x2D}, 3), TAG, "frmctr2");
    ESP_RETURN_ON_ERROR(lcd_tx_cmd(io, ST7735_CMD_FRMCTR3, (uint8_t[]){0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D}, 6), TAG, "frmctr3");
    ESP_RETURN_ON_ERROR(lcd_tx_cmd(io, ST7735_CMD_INVCTR, (uint8_t[]){0x07}, 1), TAG, "invctr");

    /* 电源 */
    ESP_RETURN_ON_ERROR(lcd_tx_cmd(io, ST7735_CMD_PWCTR1, (uint8_t[]){0xA2, 0x02, 0x84}, 3), TAG, "pwctr1");
    ESP_RETURN_ON_ERROR(lcd_tx_cmd(io, ST7735_CMD_PWCTR2, (uint8_t[]){0xC5}, 1), TAG, "pwctr2");
    ESP_RETURN_ON_ERROR(lcd_tx_cmd(io, ST7735_CMD_PWCTR3, (uint8_t[]){0x0A, 0x00}, 2), TAG, "pwctr3");
    ESP_RETURN_ON_ERROR(lcd_tx_cmd(io, ST7735_CMD_PWCTR4, (uint8_t[]){0x8A, 0x2A}, 2), TAG, "pwctr4");
    ESP_RETURN_ON_ERROR(lcd_tx_cmd(io, ST7735_CMD_PWCTR5, (uint8_t[]){0x8A, 0xEE}, 2), TAG, "pwctr5");
    ESP_RETURN_ON_ERROR(lcd_tx_cmd(io, ST7735_CMD_VMCTR1, (uint8_t[]){0x0E}, 1), TAG, "vmctr1");

    /* 显示方向 + 颜色顺序 */
    ESP_RETURN_ON_ERROR(lcd_tx_cmd(io, LCD_CMD_MADCTL, (uint8_t[]){st7735->madctl_val}, 1), TAG, "madctl");
    /* 像素格式：0x05 = 16bpp RGB565（ST7735S 固定大端字节序，由 LVGL esp_lvgl_port swap_bytes 配合） */
    ESP_RETURN_ON_ERROR(lcd_tx_cmd(io, LCD_CMD_COLMOD, (uint8_t[]){0x05}, 1), TAG, "colmod");

    /* Gamma（“R”变体，ST7735S 兼容 3A 命令集） */
    ESP_RETURN_ON_ERROR(lcd_tx_cmd(io, ST7735_CMD_GMCTRP1,
                                   (uint8_t[]){0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D,
                                               0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10}, 16), TAG, "gmctrp1");
    ESP_RETURN_ON_ERROR(lcd_tx_cmd(io, ST7735_CMD_GMCTRN1,
                                   (uint8_t[]){0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D,
                                               0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10}, 16), TAG, "gmctrn1");

    /* 颜色反转（部分 ST7735S 面板需要） */
    ESP_RETURN_ON_ERROR(lcd_tx_cmd(io, st7735->color_invert ? LCD_CMD_INVON : LCD_CMD_INVOFF,
                                   NULL, 0), TAG, "invert");

    /* 正常显示模式 + 开屏 */
    ESP_RETURN_ON_ERROR(lcd_tx_cmd(io, LCD_CMD_NORON, NULL, 0), TAG, "noron");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(lcd_tx_cmd(io, LCD_CMD_DISPON, NULL, 0), TAG, "dispon");
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "ST7735S 初始化完成：ROTATION=%d MADCTL=0x%02X OFFSET=(%d,%d) RES=%lux%lu",
             st7735->rotation, st7735->madctl_val, st7735->x_gap, st7735->y_gap,
             (unsigned long)st7735->width, (unsigned long)st7735->height);
    return ESP_OK;
}

static esp_err_t panel_st7735_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start,
                                          int x_end, int y_end, const void *color_data)
{
    st7735_panel_t *st7735 = __containerof(panel, st7735_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7735->io;

    /* 显存起始偏移（旋转后已由几何计算给出生效偏移） */
    x_start += st7735->x_gap;
    x_end   += st7735->x_gap;
    y_start += st7735->y_gap;
    y_end   += st7735->y_gap;

    /* 定义 MCU 可访问的显存窗口 */
    ESP_RETURN_ON_ERROR(lcd_tx_cmd(io, LCD_CMD_CASET, (uint8_t[]){
        (x_start >> 8) & 0xFF, x_start & 0xFF,
        ((x_end - 1) >> 8) & 0xFF, (x_end - 1) & 0xFF,
    }, 4), TAG, "caset");
    ESP_RETURN_ON_ERROR(lcd_tx_cmd(io, LCD_CMD_RASET, (uint8_t[]){
        (y_start >> 8) & 0xFF, y_start & 0xFF,
        ((y_end - 1) >> 8) & 0xFF, (y_end - 1) & 0xFF,
    }, 4), TAG, "raset");

    /* 写像素：RGB565 每像素 2 字节 */
    size_t len = (size_t)(x_end - x_start) * (y_end - y_start) * st7735->fb_bits_per_pixel / 8;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(io, LCD_CMD_RAMWR, color_data, len),
                        TAG, "io tx color failed");
    return ESP_OK;
}

static esp_err_t panel_st7735_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    st7735_panel_t *st7735 = __containerof(panel, st7735_panel_t, base);
    return lcd_tx_cmd(st7735->io, invert_color_data ? LCD_CMD_INVON : LCD_CMD_INVOFF, NULL, 0);
}

static esp_err_t panel_st7735_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    st7735_panel_t *st7735 = __containerof(panel, st7735_panel_t, base);
    if (mirror_x) {
        st7735->madctl_val |= LCD_CMD_MX_BIT;
    } else {
        st7735->madctl_val &= ~LCD_CMD_MX_BIT;
    }
    if (mirror_y) {
        st7735->madctl_val |= LCD_CMD_MY_BIT;
    } else {
        st7735->madctl_val &= ~LCD_CMD_MY_BIT;
    }
    return lcd_tx_cmd(st7735->io, LCD_CMD_MADCTL, (uint8_t[]){st7735->madctl_val}, 1);
}

static esp_err_t panel_st7735_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    st7735_panel_t *st7735 = __containerof(panel, st7735_panel_t, base);
    if (swap_axes) {
        st7735->madctl_val |= LCD_CMD_MV_BIT;
        /* 交换偏移（纵向/横向偏移定义随方向变化） */
        int tmp = st7735->x_gap;
        st7735->x_gap = st7735->y_gap;
        st7735->y_gap = tmp;
    } else {
        st7735->madctl_val &= ~LCD_CMD_MV_BIT;
        int tmp = st7735->x_gap;
        st7735->x_gap = st7735->y_gap;
        st7735->y_gap = tmp;
    }
    return lcd_tx_cmd(st7735->io, LCD_CMD_MADCTL, (uint8_t[]){st7735->madctl_val}, 1);
}

static esp_err_t panel_st7735_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    st7735_panel_t *st7735 = __containerof(panel, st7735_panel_t, base);
    st7735->x_gap = x_gap;
    st7735->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_st7735_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    st7735_panel_t *st7735 = __containerof(panel, st7735_panel_t, base);
    return lcd_tx_cmd(st7735->io, on_off ? LCD_CMD_DISPON : LCD_CMD_DISPOFF, NULL, 0);
}

static esp_err_t panel_st7735_disp_sleep(esp_lcd_panel_t *panel, bool sleep)
{
    st7735_panel_t *st7735 = __containerof(panel, st7735_panel_t, base);
    return lcd_tx_cmd(st7735->io, sleep ? LCD_CMD_SLPIN : LCD_CMD_SLPOUT, NULL, 0);
}

/* ST7735S 无背光/亮度寄存器的简单映射，不支持亮度调节 */
static esp_err_t panel_st7735_set_brightness(esp_lcd_panel_t *panel, int brightness)
{
    (void)panel; (void)brightness;
    return ESP_ERR_NOT_SUPPORTED;
}

/* ---------------------------------------------------------------------------
 * 对外接口
 * ------------------------------------------------------------------------- */

lcd_st7735_config_t lcd_st7735_default_config(void)
{
    lcd_st7735_config_t cfg = {
        .reset_gpio_num = -1,               /* 共享复位线：恒为 -1，禁止改动 */
        .offset_x       = CONFIG_LCD_ST7735_OFFSET_X,
        .offset_y       = CONFIG_LCD_ST7735_OFFSET_Y,
        .offset_manual  = false,
        .h_res          = CONFIG_LCD_ST7735_H_RES,
        .v_res          = CONFIG_LCD_ST7735_V_RES,
        .spi_host       = BOARD_LCD_SPI_HOST,  /* SPI3_HOST */
        .spi_clk_hz     = CONFIG_LCD_ST7735_SPI_CLK_HZ,
        .sclk_gpio_num  = BOARD_LCD_PIN_SCLK,
        .mosi_gpio_num  = BOARD_LCD_PIN_MOSI,
        .dc_gpio_num    = BOARD_LCD_PIN_DC,
        .cs_gpio_num    = BOARD_LCD_PIN_CS,
    };
#if defined(CONFIG_LCD_ST7735_ROTATION_90)
    cfg.rotation = 90;
#elif defined(CONFIG_LCD_ST7735_ROTATION_180)
    cfg.rotation = 180;
#elif defined(CONFIG_LCD_ST7735_ROTATION_270)
    cfg.rotation = 270;
#else
    cfg.rotation = 0;
#endif
#if defined(CONFIG_LCD_ST7735_MIRROR_X)
    cfg.mirror_x = true;
#else
    cfg.mirror_x = false;
#endif
#if defined(CONFIG_LCD_ST7735_MIRROR_Y)
    cfg.mirror_y = true;
#else
    cfg.mirror_y = false;
#endif
#if defined(CONFIG_LCD_ST7735_OFFSET_MANUAL)
    cfg.offset_manual = true;
#endif
#if defined(CONFIG_LCD_ST7735_RGB_BGR)
    cfg.color_order = LCD_RGB_ELEMENT_ORDER_BGR;  /* 多数模块 BGR */
#else
    cfg.color_order = LCD_RGB_ELEMENT_ORDER_RGB;
#endif
#if defined(CONFIG_LCD_ST7735_INVERT_COLOR)
    cfg.color_invert = true;                /* 面板颜色异常（负片）时开启 INVON */
#else
    cfg.color_invert = false;
#endif
    return cfg;
}

esp_err_t lcd_st7735_init(const lcd_st7735_config_t *cfg,
                          esp_lcd_panel_io_handle_t *ret_io,
                          esp_lcd_panel_handle_t *ret_panel)
{
    esp_err_t ret = ESP_OK;
    esp_lcd_panel_io_handle_t io = NULL;
    st7735_panel_t *st7735 = NULL;

    ESP_GOTO_ON_FALSE(cfg && ret_panel, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");
    /* 共享复位线约束：绝不操作 GPIO15 */
    ESP_GOTO_ON_FALSE(cfg->reset_gpio_num < 0, ESP_ERR_INVALID_ARG, err, TAG,
                      "reset_gpio_num 必须为 -1（GPIO15 与 W5500_RST 共用！）");
    ESP_GOTO_ON_FALSE(cfg->rotation == 0 || cfg->rotation == 90 ||
                      cfg->rotation == 180 || cfg->rotation == 270,
                      ESP_ERR_INVALID_ARG, err, TAG, "rotation 仅支持 0/90/180/270");

    /* 1) SPI 总线：LCD 独立挂 SPI3，避开 W5500 独占的 SPI2 */
    spi_bus_config_t buscfg = {
        .sclk_io_num     = cfg->sclk_gpio_num,
        .mosi_io_num     = cfg->mosi_gpio_num,
        .miso_io_num     = -1,               /* ST7735S 只写，无 MISO */
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = cfg->h_res * cfg->v_res * 2,  /* 整帧（128*160*2=40KB），整帧一次传输 */
    };
    ESP_GOTO_ON_ERROR(spi_bus_initialize(cfg->spi_host, &buscfg, SPI_DMA_CH_AUTO),
                      err, TAG, "SPI 总线初始化失败");

    /* 2) 面板 IO：8bit 命令/参数，DC/CS 由 IO 驱动自动控制 */
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num        = cfg->dc_gpio_num,
        .cs_gpio_num        = cfg->cs_gpio_num,
        .pclk_hz            = cfg->spi_clk_hz,
        .lcd_cmd_bits       = 8,
        .lcd_param_bits     = 8,
        .spi_mode           = 0,
        .trans_queue_depth  = 10,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_spi(cfg->spi_host, &io_cfg, &io),
                      err, TAG, "面板 IO 创建失败");

    /* 3) 自定义面板 */
    st7735 = calloc(1, sizeof(*st7735));
    ESP_GOTO_ON_FALSE(st7735, ESP_ERR_NO_MEM, err, TAG, "面板内存不足");

    /* 3.1) 几何：旋转/镜像/偏移/分辨率 一次算好 */
    uint8_t madctl = 0;
    int x_gap = 0, y_gap = 0;
    uint32_t width = 0, height = 0;
    lcd_st7735_compute_geometry(cfg, &madctl, &x_gap, &y_gap, &width, &height);
    if (cfg->color_order == LCD_RGB_ELEMENT_ORDER_BGR) {
        madctl |= LCD_CMD_BGR_BIT;
    }

    st7735->io                = io;
    st7735->reset_gpio_num    = cfg->reset_gpio_num;
    st7735->x_gap             = x_gap;
    st7735->y_gap             = y_gap;
    st7735->rotation          = cfg->rotation;
    st7735->width             = width;
    st7735->height            = height;
    st7735->fb_bits_per_pixel = 16;
    st7735->madctl_val        = madctl;
    st7735->color_invert      = cfg->color_invert;

    st7735->base.del          = panel_st7735_del;
    st7735->base.reset        = panel_st7735_reset;
    st7735->base.init         = panel_st7735_init;
    st7735->base.draw_bitmap  = panel_st7735_draw_bitmap;
    st7735->base.invert_color = panel_st7735_invert_color;
    st7735->base.mirror       = panel_st7735_mirror;
    st7735->base.swap_xy      = panel_st7735_swap_xy;
    st7735->base.set_gap      = panel_st7735_set_gap;
    st7735->base.disp_on_off  = panel_st7735_disp_on_off;
    st7735->base.disp_sleep   = panel_st7735_disp_sleep;
    st7735->base.set_brightness = panel_st7735_set_brightness;

    *ret_panel = &(st7735->base);
    if (ret_io != NULL) {
        *ret_io = io;
    }

    ESP_LOGI(TAG, "ST7735S 面板创建成功：SPI%lu, %u Hz | ROTATION=%d MIRROR_X=%d MIRROR_Y=%d "
             "MADCTL=0x%02X OFFSET=(%d,%d) RES=%lux%lu",
             (unsigned long)(cfg->spi_host + 1), cfg->spi_clk_hz,
             cfg->rotation, (int)cfg->mirror_x, (int)cfg->mirror_y,
             madctl, x_gap, y_gap, (unsigned long)width, (unsigned long)height);
    return ESP_OK;

err:
    if (io) {
        esp_lcd_panel_io_del(io);
    }
    if (st7735) {
        free(st7735);
    }
    return ret;
}

esp_err_t lcd_st7735_get_resolution(esp_lcd_panel_handle_t panel,
                                    uint32_t *w, uint32_t *h)
{
    if (panel == NULL || w == NULL || h == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    st7735_panel_t *st7735 = __containerof(panel, st7735_panel_t, base);
    *w = st7735->width;
    *h = st7735->height;
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 * 诊断测试图（诊断模式，绕过 LVGL）
 * ------------------------------------------------------------------------- */

/** RGB565 交换字节序（ST7735S 16bpp 为大端，绘制数据按面板格式组织） */
static inline uint16_t tp_bswap16(uint16_t v)
{
    return (uint16_t)((v << 8) | (v >> 8));
}

/** 计算 (x,y) 处测试图颜色（RGB565 小端） */
static uint16_t tp_color_at(uint32_t w, uint32_t h, uint32_t x, uint32_t y)
{
    /* 1) 1px 白色边框（判断偏移/裁切/斜切） */
    if (x == 0 || y == 0 || x == w - 1 || y == h - 1) {
        return TP_WHITE;
    }

    /* 2) 四角 16x16 标记方块（判断旋转/镜像方向） */
    const uint32_t SZ = 16;
    if (x >= 2 && x < 2 + SZ && y >= 2 && y < 2 + SZ) {
        return TP_RED;                                  /* 左上：红 */
    }
    if (x >= w - 2 - SZ && x < w - 2 && y >= 2 && y < 2 + SZ) {
        return TP_GREEN;                                /* 右上：绿 */
    }
    if (x >= 2 && x < 2 + SZ && y >= h - 2 - SZ && y < h - 2) {
        return TP_BLUE;                                 /* 左下：蓝 */
    }
    if (x >= w - 2 - SZ && x < w - 2 && y >= h - 2 - SZ && y < h - 2) {
        return TP_YELLOW;                               /* 右下：黄 */
    }

    /* 3) 全屏红/绿/蓝三条竖色带（各约 1/3 宽，判断扫描是否笔直/斜切） */
    uint32_t third = w / 3;
    if (x < third) {
        return TP_RED;
    }
    if (x < 2 * third) {
        return TP_GREEN;
    }
    return TP_BLUE;
}

esp_err_t lcd_st7735_draw_test_pattern(esp_lcd_panel_handle_t panel)
{
    if (panel == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t w = 0, h = 0;
    ESP_RETURN_ON_ERROR(lcd_st7735_get_resolution(panel, &w, &h), TAG, "获取分辨率失败");

    ESP_LOGI(TAG, "测试图：整帧 %lux%lu = %lu B（RGB565 大端）",
             (unsigned long)w, (unsigned long)h, (unsigned long)(w * h * 2));

    /* 整帧缓冲（DMA 内存，直接把数据交给 SPI 传输） */
    uint16_t *buf = heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, TAG, "测试图缓冲申请失败");

    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            buf[y * w + x] = tp_bswap16(tp_color_at(w, h, x, y));
        }
    }

    esp_err_t ret = esp_lcd_panel_draw_bitmap(panel, 0, 0, (int)w, (int)h, buf);
    heap_caps_free(buf);
    ESP_RETURN_ON_ERROR(ret, TAG, "测试图绘制失败");

    ESP_LOGI(TAG, "测试图已绘制完成（观察：白边框是否裁切/偏移、三色竖带是否笔直、四角色块方位）");
    return ESP_OK;
}