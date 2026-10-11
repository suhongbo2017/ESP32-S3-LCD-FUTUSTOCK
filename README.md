# ESP32-S3-LCD-FUTUSTOCK

ESP32-S3 + 1.77" ST7735S 行情看板，目标是迁移 futu-tracker 动态持仓、腾讯行情、连续滚动显示和 OTA Web 控制端。

## 开发原则

- 以 `ESP32-S3-Gateway` 的 ESP-IDF 5.5.5、`esp_lcd`、LVGL 和双分区 OTA 为基础。
- 本仓库独立开发，不修改相邻的原始工程。
- WiFi、futu-tracker 和 OTA 密码只放在未跟踪的本地配置中。
- 每次功能修改都先编译和验证，再提交并推送到 `origin/main`。

## 当前状态

仓库已建立，后续按以下阶段实现：

1. 迁移动态持仓和行情数据组件。
2. 适配 128x160 ST7735S 看板布局。
3. 接入现有 Web 控制台和双分区 OTA。
4. 单独验证 ST7735S 硬件连续滚动。

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
