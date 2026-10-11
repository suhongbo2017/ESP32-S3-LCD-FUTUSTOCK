# ESP32-S3-LCD-FUTUSTOCK

ESP32-S3 + 1.77" ST7735S 行情看板，目标是迁移 futu-tracker 动态持仓、腾讯行情、连续滚动显示和 OTA Web 控制端。

## 开发原则

- 以 `ESP32-S3-Gateway` 的 ESP-IDF 5.5.5、`esp_lcd`、LVGL 和双分区 OTA 为基础。
- 本仓库独立开发，不修改相邻的原始工程。
- WiFi、futu-tracker 和 OTA 密码只放在未跟踪的本地配置中。
- 每次功能修改都先编译和验证，再提交并推送到 `origin/main`。

## 当前状态

第一阶段骨架已完成：

- 已复制并独立维护 ESP32-S3 Gateway 的 ESP-IDF、ST7735S、LVGL、WiFi、双网和双分区 OTA 基础工程。
- 已新增 `components/market_data`，使用 `esp_http_client + cJSON` 迁移 futu-tracker 持仓同步和腾讯行情请求。
- `app_main` 已创建行情刷新任务：网络可用时按 Kconfig 周期同步持仓并拉取行情，结果输出到串口。
- 当前尚未把行情数据接入 LVGL；屏幕仍显示原 Gateway 的网络看板。

下一阶段：

1. 在 Web `/api/status` 返回持仓和行情快照。
2. 将行情数据显示到 128x160 ST7735S 的 LVGL 看板。
3. 增加 1.77 寸屏幕分页和连续滚动实验。
4. 在目标 S3 硬件上完成编译、烧录和长稳验证。

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
