/**
 * @file board_pins.h
 * @brief 板级引脚集中定义 —— ESP32_S3_LCD 带屏幕开发板（W5500 网口版）
 *
 * 引脚来源：《PINMAP.md》（原理图逐脚核对）。
 * 约定：本文件是全工程唯一的引脚定义来源；各外设组件一律包含本文件，
 *       严禁在驱动代码里另写 GPIO 编号。
 */
#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== W5500 有线以太网（SPI2，默认 FSPI 引脚） ==================== */
#define BOARD_ETH_SPI_HOST        SPI2_HOST        /*!< W5500 挂载的 SPI 主机 */
#define BOARD_ETH_PIN_SCLK        GPIO_NUM_12      /*!< FSPICLK  时钟 */
#define BOARD_ETH_PIN_MOSI        GPIO_NUM_11      /*!< FSPID    主出（从入） */
#define BOARD_ETH_PIN_MISO        GPIO_NUM_13      /*!< FSPIQ    主入（从出） */
#define BOARD_ETH_PIN_CS          GPIO_NUM_10      /*!< FSPICS0  片选 */
#define BOARD_ETH_PIN_INT         GPIO_NUM_14      /*!< W5500 中断输出（低有效） */
#define BOARD_ETH_PIN_RST         GPIO_NUM_15      /*!< W5500 复位（与 LCD_RST 共用一根线） */

/* ==================== ST7735S LCD（走 GPIO Matrix 路由到 SPI） ==================== */
#define BOARD_LCD_SPI_HOST        SPI3_HOST        /*!< LCD 独立挂 SPI3，避幵 W5500 独占的 SPI2 */
#define BOARD_LCD_PIN_DC          GPIO_NUM_16      /*!< 数据/命令选择 */
#define BOARD_LCD_PIN_MOSI        GPIO_NUM_17      /*!< SPI 数据（SDA） */
#define BOARD_LCD_PIN_SCLK        GPIO_NUM_18      /*!< SPI 时钟（SCL） */
#define BOARD_LCD_PIN_CS          GPIO_NUM_21      /*!< 片选 */
#define BOARD_LCD_PIN_RST         BOARD_ETH_PIN_RST /*!< 与 W5500_RST 共用 GPIO15 */
#define BOARD_LCD_H_RES           128              /*!< 1.77" TFT 分辨率 */
#define BOARD_LCD_V_RES           160

/* ==================== TF 卡（SDMMC 4-bit，Slot 0 默认 IO_MUX 引脚） ==================== */
#define BOARD_SD_PIN_CLK          GPIO_NUM_36
#define BOARD_SD_PIN_CMD          GPIO_NUM_35
#define BOARD_SD_PIN_D0           GPIO_NUM_37
#define BOARD_SD_PIN_D1           GPIO_NUM_38
#define BOARD_SD_PIN_D2           GPIO_NUM_33
#define BOARD_SD_PIN_D3           GPIO_NUM_34

/* ==================== UART0（CH340K 自动下载 / 日志） ==================== */
#define BOARD_UART0_TX            GPIO_NUM_43
#define BOARD_UART0_RX            GPIO_NUM_44

/* ==================== 其它板载占用（仅供查阅，勿另作他用） ====================
 * BOOT 按键    : GPIO0
 * 原生 USB     : GPIO19 (D-) / GPIO20 (D+)
 * Flash/PSRAM  : GPIO26~32（未引出，禁用）
 * strapping    : GPIO45 (VDD_SPI 选择) / GPIO46 (ROM 日志)
 * 空闲可复用   : GPIO1~9、39~42、47~48（排针引出）
 * ======================================================================== */

#ifdef __cplusplus
}
#endif