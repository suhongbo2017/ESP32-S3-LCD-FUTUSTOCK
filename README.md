# ESP32-S3-LCD-FUTUSTOCK

ESP32-S3 + 1.77" ST7735S 行情看板，目标是迁移 futu-tracker 动态持仓、腾讯行情、连续滚动显示和 OTA Web 控制端。

## 开发原则

- 以 `ESP32-S3-Gateway` 的 ESP-IDF 5.5.5、`esp_lcd`、LVGL 和双分区 OTA 为基础。
- 本仓库独立开发，不修改相邻的原始工程。
- WiFi、futu-tracker 和 OTA 密码只放在未跟踪的本地配置中。
- 每次功能修改都先编译和验证，再提交并推送到 `origin/main`。

## 当前状态

第三阶段已完成：

- 已复制并独立维护 ESP32-S3 Gateway 的 ESP-IDF、ST7735S、LVGL、WiFi、双网和双分区 OTA 基础工程。
- 已新增 `components/market_data`，使用 `esp_http_client + cJSON` 迁移 futu-tracker 持仓同步和腾讯行情请求。
- `app_main` 已创建行情刷新任务：网络可用时按 Kconfig 周期同步持仓并拉取行情。
- LVGL 已改为 `FutuStock` 128x160 行情看板：4 行紧凑行情、5 秒分页，底部显示链路和 IP。
- 行情颜色已按实机屏幕颜色通道设置为：上涨绿色、下跌红色、无效数据灰色。
- Web `/api/status` 已增加 `market` 快照：持仓数量、有效行情数量、错误信息和逐条行情。
- 行情网络任务与 LVGL 显示任务通过快照复制隔离，网络请求不会在 LVGL 锁内执行。

下一阶段：

1. 在目标 S3 硬件上完成编译、烧录和屏幕显示验证。
2. 增加控制端刷新/同步操作。
3. 单独验证 1.77 寸 ST7735S 的硬件连续滚动，再决定是否接入分页看板。

### 构建配置

首次使用前，在 `sdkconfig` 或 `menuconfig` 中配置：

```text
Component config -> market_data
  MARKET_DATA_API_URL
  MARKET_DATA_API_USER
  MARKET_DATA_API_PASSWORD

Component config -> net_wifi
  NET_WIFI_SSID
  NET_WIFI_PASSWORD
```

API 密码只保存在本机 `sdkconfig`，不要提交到仓库。

`GET /api/status` 的行情字段示例：

```json
{
  "market": {
    "ready": true,
    "holdings": 16,
    "quote_ok": 16,
    "error": "OK",
    "items": [{"symbol":"NVDA","last":169.68,"percent":1.2}]
  }
}
```

只读接口沿用 Web 控制台现有鉴权策略；当启用 `CONFIG_WEB_CONSOLE_PROTECT_READ` 时需要 Token。

第三阶段已在本机 ESP-IDF 5.5.5 环境完成编译验证，并在 ESP32-S3 `COM7` 实机烧录验证：

```text
Project build complete.
esp32s3_gateway.bin binary size 0x22b8d0
Smallest app partition is 0x300000
0xd4730 bytes (28%) free

WiFi: 192.168.99.36
持仓同步成功：16 只
行情成功：16/16 只
MQTT 已连接，传感器数据正常
```

实机已确认屏幕初始化不再触发 LVGL 重启，连续 30 秒串口观察无 `Guru Meditation`。

根据实机显示方向，ST7735S 默认旋转已设为 `ROTATION=180`，用于上下翻转。

## 版本管理

```bash
git remote -v
git status
git log --oneline --decorate -5
```

默认远程仓库：

```text
https://github.com/suhongbo2017/ESP32-S3-LCD-FUTUSTOCK.git
```

提交前至少执行：

```bash
git diff --check
# 使用工程对应的 ESP-IDF 构建命令
# 通过后再 git add / git commit / git push origin main
```
