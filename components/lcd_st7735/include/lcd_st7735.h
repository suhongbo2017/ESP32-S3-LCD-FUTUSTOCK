/**
 * @file lcd_st7735.h
 * @brief ST7735S 1.77" TFT（128x160）自定义 esp_lcd 面板驱动接口
 *
 * 依赖 esp_lcd 组件：面板 IO 走 SPI（esp_lcd_panel_io_spi），
 * 面板本体为自实现 esp_lcd_panel_t 接口（初始化命令序列 / 描点 / 方向控制）。
 *
 * 方向/偏移模型（ST7735S 显存 132x162，面板 128x160）：
 *  - OFFSET_X/Y 是 0° 旋转的基准偏移（默认 x=2, y=1）
 *  - 旋转 ROTATION + MIRROR_X/Y 由驱动换算为 MADCTL（MY/MX/MV/BGR）
 *  - 旋转 90/270 时宽高互换，且 OFFSET_X 与 OFFSET_Y 自动交换（查表）
 *  - OFFSET_MANUAL=y 可关闭自动交换，直接用 OFFSET_X/Y（仅调试）
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_lcd_types.h"
#include "esp_lcd_panel_io.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief ST7735S 面板配置
 */
typedef struct {
    int reset_gpio_num;             /*!< 复位引脚。【必须传 -1】：
                                         GPIO15 与 W5500_RST 共用，上电由 net_eth 负责复位脉冲，
                                         本驱动绝不再驱动该引脚，否则会把 W5500 一起复位 */
    lcd_rgb_element_order_t color_order; /*!< RGB 或 BGR（多数模块为 BGR） */
    bool color_invert;              /*!< 颜色反转（画面负片时开启 INVON） */

    int   rotation;                 /*!< 旋转角：0 / 90 / 180 / 270（度） */
    bool  mirror_x;                 /*!< 水平镜像（MADCTL MX） */
    bool  mirror_y;                 /*!< 垂直镜像（MADCTL MY） */

    int   offset_x;                 /*!< 0°时 CASET 基准列偏移（本板实测 =0；非 0 会导致斜切） */
    int   offset_y;                 /*!< 0°时 RASET 基准行偏移（本板实测 =0；非 0 会导致斜切） */
    bool  offset_manual;            /*!< true=所有旋转直接用 offset_x/y，不自动交换（调试用） */

    uint32_t h_res;                 /*!< 0°面板水平分辨率（默认 128） */
    uint32_t v_res;                 /*!< 0°面板垂直分辨率（默认 160） */

    int  spi_host;                  /*!< SPI 主机（建议 SPI3_HOST，避开 W5500 的 SPI2） */
    unsigned int spi_clk_hz;        /*!< SPI 时钟频率（ST7735S 建议 <= 40MHz） */
    int sclk_gpio_num;              /*!< SCLK（GPIO18） */
    int mosi_gpio_num;              /*!< MOSI（GPIO17） */
    int dc_gpio_num;                /*!< D/C（GPIO16） */
    int cs_gpio_num;                /*!< CS（GPIO21） */
} lcd_st7735_config_t;

/** @brief 默认配置（引脚取 board_pins + Kconfig 值，reset_gpio_num 恒为 -1） */
lcd_st7735_config_t lcd_st7735_default_config(void);

/**
 * @brief 创建 ST7735S 面板（内部完成 SPI 总线 + 面板 IO + 自定义面板的创建）
 *
 * 创建时即按 rotation/mirror 计算出最终 MADCTL 与生效偏移（x_gap/y_gap）
 * 与旋转后的分辨率，可经 lcd_st7735_get_resolution() 查询。
 *
 * @param[in]  cfg      面板配置
 * @param[out] ret_io   返回面板 IO 句柄（仅调试用，可为 NULL）
 * @param[out] ret_panel 返回面板句柄（可传入 esp_lvgl_port / esp_lcd_panel_* 使用）
 * @note 创建后需自行调用 esp_lcd_panel_reset() + esp_lcd_panel_init() 完成初始化
 *       （reset 在无 RST 引脚时退化为软件复位 SWRESET，不触碰 GPIO15）。
 */
esp_err_t lcd_st7735_init(const lcd_st7735_config_t *cfg,
                          esp_lcd_panel_io_handle_t *ret_io,
                          esp_lcd_panel_handle_t *ret_panel);

/**
 * @brief 查询旋转后面板的分辨率（0/180° 为 128x160；90/270° 为 160x128）
 */
esp_err_t lcd_st7735_get_resolution(esp_lcd_panel_handle_t panel,
                                    uint32_t *w, uint32_t *h);

/**
 * @brief 绘制静态诊断测试图（诊断模式用，绕过 LVGL）
 *
 * 内容：1px 白色边框 + 四角 16x16 色块（左上红/右上绿/左下蓝/右下黄）
 *       + 全屏红/绿/蓝三条竖色带（各约 1/3 宽）。
 * 观察项：边框是否被裁切/偏移（验证 OFFSET）、三色竖带是否笔直（验证斜切）、
 *       四角色块方位（验证 ROTATION/MIRROR 方向）。
 *
 * @note 数据按 ST7735S 大端 RGB565 组织；需先完成 esp_lcd_panel_reset/init。
 */
esp_err_t lcd_st7735_draw_test_pattern(esp_lcd_panel_handle_t panel);

/** @brief 启动硬件 VSCRDEF/VSCSAD 连续滚动测试（仅诊断模式使用）。 */
esp_err_t lcd_st7735_start_scroll_test(esp_lcd_panel_handle_t panel);

#ifdef __cplusplus
}
#endif