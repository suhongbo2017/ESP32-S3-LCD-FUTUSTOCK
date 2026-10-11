# ESP32-S3 开发板引脚映射（PINMAP）

> **板型**：ESP32_S3_LCD带屏幕（W5500 网口 + 1.77″ ST7735S TFT + TF 卡 + CH340K + 双 Type-C）
> **来源**：原理图（逐脚核对）+《板子介绍.pdf》+ 零件位置图 + 实机 esptool 识别
> **生成日期**：2026-09-24

---

## 1. 板卡概述

| 项目 | 说明 |
|------|------|
| 主控 | ESP32-S3（Xtensa 32-bit LX7 双核，最高 240 MHz，Wi-Fi + BT 5.0 LE） |
| 模块选型 (U1) | ESP32-S3FN8 / S3R2 / S3R8 / S3R8V / S3R16V / S3FH4R2（可选焊） |
| Flash | 16 MB（实测；器件 U2，SPI-CS0） |
| PSRAM | 8 MB（器件 U3，SPI-CS1） |
| 以太网 | W5500 硬件 TCP/IP 芯片（SPI 接口） |
| 显示 | 1.77″ TFT，驱动 IC **ST7735S**，SPI 接口 |
| 存储 | TF（microSD）卡座，SDMMC 4-bit |
| 串口 | CH340K 自动下载电路（Type-C J3） |
| USB | ESP32-S3 原生 USB（Type-C J4） |
| 已识别实机 | 芯片 ESP32-S3 rev v0.2，MAC `44:1b:f6:ec:c5:8c`，16MB Flash |

---

## 2. 外设引脚映射（核心）

### 2.1 W5500 以太网（SPI2，默认 FSPI 引脚）

| 信号 | GPIO | ESP32-S3 默认 FSPI 功能 |
|------|------|------------------------|
| CS (片选) | **GPIO10** | FSPICS0 |
| MOSI (主出从入) | **GPIO11** | FSPID |
| SCLK (时钟) | **GPIO12** | FSPICLK |
| MISO (从出主入) | **GPIO13** | FSPIQ |
| INT (中断) | **GPIO14** | — |
| RST (复位) | **GPIO15** | 与 LCD_RST **共用同一根线** |

> W5500 外接 25 MHz 晶振；采用硬件 TCP/IP 协议栈，ESP32 侧走 RAW MAC + LWIP。

### 2.2 LCD ST7735S（SPI）

| 信号 | GPIO |
|------|------|
| D/C (数据/命令) | **GPIO16** |
| MOSI (SDA) | **GPIO17** |
| SCLK (SCL) | **GPIO18** |
| CS (片选) | **GPIO21** |
| RST (复位) | **GPIO15**（与 W5500_RST 共用） |

> LCD 与 W5500 不在同一组 SPI 引脚上；W5500 占用 SPI2 默认 IO_MUX 引脚（10–13），
> LCD 使用 GPIO17/18/21/16，需经 **GPIO Matrix** 路由到 SPI2 或 SPI3。

> 📌 **实测备注（2026-09-25，工程实际使用）**：
> - LCD 挂 **SPI3_HOST** @40MHz（W5500 独占 SPI2，避免总线冲突）
> - **CASET/RASET 偏移实测 = (0,0)**（本板行宽不含“显存132→128”余量；按常规 (2,1)
>   设置会导致窗口越界回卷 → 画面 45° 斜切，已修复，排查记录见工程 README §6.7）
> - 正常显示方向 **ROTATION=180、MADCTL=0xC0（MX|MY）、RGB（BGR 位关）**；
>   画面方向/颜色异常可用 ROTATION(0/90/180/270) + MIRROR_X/Y + RGB_BGR 调整
> - 注意：LVGL 看板方向必须与 Kconfig 旋转一致（esp_lvgl_port 会重写 MADCTL），
>   看板 rotation 跟随配置的实现见工程 README §6.8-b

### 2.3 TF 卡（SDMMC 4-bit，即 ESP-IDF SDMMC Slot 0 默认引脚）

| 信号 | GPIO |
|------|------|
| CLK | **GPIO36** |
| CMD | **GPIO35** |
| DATA0 | **GPIO37** |
| DATA1 | **GPIO38** |
| DATA2 | **GPIO33** |
| DATA3 | **GPIO34** |

> 该组引脚正是 ESP32-S3 SDMMC 主机（Slot 0）的默认 IO_MUX 配置，可直接用 `sdmmc_host` 驱动。

### 2.4 USB / 串口 / 按键 / 启动

| 功能 | 引脚 | 说明 |
|------|------|------|
| 原生 USB D- | **GPIO19** | Type-C J4（ESP-USB） |
| 原生 USB D+ | **GPIO20** | Type-C J4（ESP-USB） |
| UART0 TX | **GPIO43** | 接 CH340K，下载/日志 |
| UART0 RX | **GPIO44** | 接 CH340K，下载/日志 |
| BOOT 按键 (S1) | **GPIO0** | 下载模式 |
| RESET 按键 (S2) | **EN (CHIP_PU)** | 复位 |
| 电源指示 LED | LED1 | — |

---

## 3. GPIO 使用汇总

### 3.1 已被板载器件占用（不要他用）

| GPIO | 用途 |
|------|------|
| 0 | BOOT 按键（可引出，谨慎复用） |
| 10–15 | W5500（SPI2 + INT + RST） |
| 15 | LCD_RST（与 W5500_RST 共用） |
| 16–18, 21 | ST7735S LCD |
| 19, 20 | 原生 USB D-/D+ |
| 26–32 | 内部 SPI Flash / PSRAM（**未引出，禁用**） |
| 33–38 | TF 卡（SDMMC） |
| 43, 44 | UART0（CH340K） |
| 45 | VDD_SPI 电压选择（strapping，板上已用） |
| 46 | ROM 日志打印控制（strapping） |

### 3.2 空闲可用（已引出到排针）

**GPIO1–9、GPIO39–42、GPIO47–48**

### 3.3 启动 / 特殊引脚（strapping）注意事项

| GPIO | 上电默认作用 | 复用建议 |
|------|--------------|----------|
| GPIO0 | 启动模式（低=下载） | 可用作输出，但上电勿拉低 |
| GPIO3 | JTAG 信号源选择 | 可用，注意上电电平 |
| GPIO45 | VDD_SPI 电压选择 | 板上已固定使用 |
| GPIO46 | ROM 代码日志打印 | 板上已使用 |
| GPIO19/20 | USB D-/D+ | 用作 USB 时勿复用 |

---

## 4. 排针引出（J1 / J2）

> 以《零件位置图.jpg》为准，建议实物核对。

**J1（左侧，STR1 / STR2）**

| STR1 | STR2 |
|------|------|
| 5V | 5V |
| GND | GND |
| 3.3V | 3.3V |
| EN | GND |
| GPIO0 | GPIO1 |
| GPIO2 | GPIO3 |
| GPIO4 | GPIO5 |
| GPIO6 | GPIO7 |
| GPIO8 | GPIO9 |
| GPIO10 | GPIO11 |
| GPIO12 | GPIO13 |
| GPIO14 | GPIO15 |
| GPIO16 | GPIO17 |
| GPIO18 | GPIO19 |
| GPIO20 | GPIO21 |

**J2（右侧，STR3 / STR4）**

| STR3 | STR4 |
|------|------|
| 5V | 5V |
| 5V | 5V |
| GND | GND |
| GND | GND |
| 3.3V | 3.3V |
| 3.3V | 3.3V |
| GPIO47 | GPIO48 |
| GPIO45 | GPIO46 |
| GPIO43 | GPIO44 |
| GPIO41 | GPIO42 |
| GPIO39 | GPIO40 |
| GPIO37 | GPIO38 |
| GPIO35 | GPIO36 |
| GPIO33 | GPIO34 |

---

## 5. ESP-IDF 配置速查

```c
/* W5500 (SPI2 / FSPI, 默认 IO_MUX) */
#define ETH_SPI_HOST      SPI2_HOST
#define ETH_PIN_SCLK      12
#define ETH_PIN_MOSI      11
#define ETH_PIN_MISO      13
#define ETH_PIN_CS        10
#define ETH_PIN_INT       14
#define ETH_PIN_RST       15   /* 与 LCD_RST 共用 */

/* LCD ST7735S (经 GPIO Matrix 到 SPI2 或 SPI3) */
#define LCD_PIN_DC        16
#define LCD_PIN_MOSI      17
#define LCD_PIN_SCLK      18
#define LCD_PIN_CS        21
#define LCD_PIN_RST       15   /* 与 W5500_RST 共用 */
                               /* 实测：CASET/RASET 偏移 (0,0)；挂 SPI3_HOST，见 §2.2 备注 */
#define LCD_H_RES         128  /* 1.77" 常见 128x160 */
#define LCD_V_RES         160

/* TF 卡 (SDMMC Slot 0 默认引脚) */
#define SD_PIN_CLK        36
#define SD_PIN_CMD        35
#define SD_PIN_D0         37
#define SD_PIN_D1         38
#define SD_PIN_D2         33
#define SD_PIN_D3         34
```

- **W5500**：`esp_eth` + W5500 驱动，`SPI2_HOST`，与 ESP32-S3 的 FSPI 默认引脚一致，无需矩阵配置。
- **LCD**：`esp_lcd` + ST7735S 面板驱动；因引脚非 SPI2 默认 IO_MUX，需经 GPIO Matrix。
- **TF 卡**：`esp_vfs_fat` + `sdmmc`，直接使用 Slot 0 默认引脚。
- **UART0**：GPIO43/44，CH340K 自动下载（DTR/RTS 控制 EN/GPIO0）。

---

## 6. 联网架构（来自《网络框架图》）

- 两条上行链路：**WiFi** 与 **W5500 以太网（SPI）**，应用层统一走 **LWIP**，代码可复用。
- W5500 内部自带硬件 TCP/IP 栈；ESP32-S3 侧使用 RAW MAC + LWIP。
- APP 层常用协议：tcp / udp / mqtt / http / DNS / DHCP。

## 7. MQTT（来自《mqtt框架图》与《MQTT公网服务器.txt》）

| 项目 | 框架图 | MQTT公网服务器.txt |
|------|--------|--------------------|
| Broker IP | 137.135.83.217 | 54.244.173.190 / broker.emqx.io |
| 端口 | 1883 | 1883（WS 8083 / TLS 8883 / WSS 8084 / QUIC 14567） |
| 密码 | 公网随意 | —（EMQX 公共 broker 匿名） |
| 主题 | 自定订阅/发布主题 | — |
| QoS | 0 / 1 / 2 | — |

> ✅ **2026-09-24 已实测确认：上云统一使用 `broker.emqx.io:1883`（EMQX 公共 broker，匿名）。**
> 54.244.173.190 与 137.135.83.217 均连接超时，弃用。
> 该 broker 另提供 TLS 8883 / WS 8083 等端口，如需加密接入按官方文档启用。

> 📌 **工程实际接入（2026-09-25，里程碑 C）：本地 Home Assistant broker
> `192.168.99.253:1883`（匿名可用）**，订阅 `ha/esp32gw/notify`、发布 `ha/esp32gw/state`，
> 并启用 MQTT Discovery（`homeassistant/sensor/esp32gw_*`）。
> 公网 broker.emqx.io 仅作连通性验证，日常上云走本地 HA。

---

## 8. 参考文件

| 文件 | 内容 |
|------|------|
| `原理图/ESP32_S3_LCD带屏幕.pdf` (+ .step) | 本板原理图 / 3D |
| `原理图/ESP32S3带RS485带POE.pdf` | 工业版（RS485 + CAN + POE） |
| `原理图/ESP32_S3_蓝色POE.pdf` | 摄像头版（OV2640 + POE） |
| `文档资料/板子介绍.pdf` | 板卡功能说明 |
| `文档资料/零件位置图.jpg` | 器件布局与排针 |
| `文档资料/网络框架图.jpg` / `mqtt框架图.jpg` | 联网架构 |
| `文档资料/W5500高清晰中文数据手册.pdf` | 以太网芯片手册 |
| `文档资料/ST7735S_Sitronix.pdf` | LCD 驱动 IC 手册 |
| `文档资料/CH340E_规格书_规格书WJ377529.PDF` | 串口芯片 |
| `文档资料/摄像头ov2640_ds_1.8_.pdf` | 摄像头（蓝色 POE 版） |
| `文档资料/esp32-s3_*.pdf` | 数据手册 / 技术参考 / 硬件设计指南 |

---

## 9. 待办 / 已知问题

- [x] 确认 MQTT Broker 使用哪个地址（见 §7）：**broker.emqx.io:1883**
- [ ] 实机当前处于 **task watchdog 重启循环**，需抓日志定位
- [ ] 排针定义建议对照实物核对（见 §4）