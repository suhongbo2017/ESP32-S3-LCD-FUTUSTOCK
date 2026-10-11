# ESP32-S3 Gateway 项目构建流水

本文件是 `ESP32-S3-Gateway` 的构建、烧录、验证和文档同步操作手册。新成员先读本文件，再修改代码。

## 1. 项目基线

| 项目 | 当前值 |
|------|--------|
| 工程目录 | `H:\VSCODE\ESP32-S3-Gateway` |
| ESP-IDF | v5.5.5，`H:\esp\esp-idf` |
| 工具链 | `IDF_TOOLS_PATH=H:\espressif` |
| Python 环境 | `H:\espressif\python_env\idf5.5_py3.13_env` |
| 芯片目标 | `esp32s3` |
| 串口 | 通常为 `COM3`，以 Windows 实际枚举结果为准 |
| Flash | 16MB |
| PSRAM | 8MB，Quad |
| 应用分区 | `ota_0` / `ota_1`，各 3MB |
| 有线 IP | DHCP，已实测 `192.168.99.26` |
| WiFi IP | DHCP，已实测 `192.168.99.32` |
| Web 控制台 | `http://esp32gw.local/` 或当前活动 IP |

## 2. 修改前检查

1. 进入工程目录：

   ```bat
   cd /d H:\VSCODE\ESP32-S3-Gateway
   ```

2. 查看工作区状态，确认没有覆盖用户已有修改：

   ```bat
   git status --short
   git diff -- README.md BUILD_FLOW.md
   ```

3. 涉及硬件引脚时先读 `PINMAP.md`；涉及扩展设备时先读 `DEVICES.md`；涉及历史踩坑时先读 `SOLUTIONS.md` 和 README 的 §8、§11。

4. 修改 Kconfig、分区表、组件依赖或 `sdkconfig.defaults` 前，记录修改前的配置。不要随意删除 `sdkconfig`，否则会丢失本机已经确认的配置。

## 3. 标准构建流水

### 3.1 首次配置

只在新机器或新工程副本执行：

```bat
cd /d H:\VSCODE\ESP32-S3-Gateway
H:\VSCODE\ESP32-S3-Gateway\build\project_flow.bat set-target
```

如果已经存在正确的 `sdkconfig`，不要为了普通改动重复执行 `set-target`。

### 3.2 重新配置

Kconfig、组件依赖、分区表或目标配置发生变化时执行：

```bat
H:\VSCODE\ESP32-S3-Gateway\build\project_flow.bat reconfigure
```

确认新配置已经进入 `sdkconfig`：

```bat
grep -n "CONFIG_" sdkconfig
```

Windows 原生 CMD 没有 `grep` 时，用：

```bat
findstr /N "CONFIG_" sdkconfig
```

### 3.3 编译

```bat
H:\VSCODE\ESP32-S3-Gateway\build\project_flow.bat build
```

验收条件：

- `BUILD_EXIT=0`
- 没有 `error:`、`undefined reference` 或分区空间不足
- app binary 小于最小 app 分区（当前 3MB）
- 如出现新的 warning，先判断是否与本次改动相关

### 3.4 串口烧录

先确认板子连接的是丝印 **CH340K 串口** 的 USB 口，并查实际端口：

```bat
mode
```

默认 COM3：

```bat
H:\VSCODE\ESP32-S3-Gateway\build\project_flow.bat flash COM3
```

其他端口：

```bat
H:\VSCODE\ESP32-S3-Gateway\build\project_flow.bat flash COM5
```

烧录完成后应看到 `Hard resetting via RTS pin...` 和 `FLASH_EXIT=0`。

### 3.5 串口监视

```bat
H:\VSCODE\ESP32-S3-Gateway\build\project_flow.bat monitor COM3
```

退出 monitor：按 `Ctrl+]`。重点观察：

- `net_manager` 的 ETH/WiFi 切换
- `ha_mqtt` 的连接、订阅和重连
- `ui_dashboard` 的 LVGL 初始化
- `[heap]`、`内存:`、`heap_free` 和最低水位
- `Guru Meditation`、`abort()`、`stack overflow`、watchdog

### 3.6 编译后 OTA

适用于串口不可用但 Web 控制台仍可访问的情况：

```bat
curl --noproxy "*" -X POST "http://<网关IP>/api/ota" ^
  --data-binary "@build\esp32s3_gateway.bin" ^
  -w "HTTP %{http_code}\n"
```

必须确认返回：

```json
{"ok":true,"rebooting":true}
```

OTA 后等待 30 秒，再访问：

```text
http://<网关IP>/api/status
```

注意：如果 HTTP 服务已经停止，OTA 也不可用，此时只能通过 CH340K 串口或断电重启恢复。

## 4. 修改后的验证顺序

每次较大修改至少完成以下步骤：

1. 编译：确认 `BUILD_EXIT=0`。
2. 烧录或 OTA：确认设备确实运行新固件。
3. 启动观察：至少观察完整启动流程，确认无 panic、abort、看门狗重启。
4. 功能验证：只验证本次修改涉及的功能，同时检查 ETH/WiFi、MQTT、LCD、TF 和 Web 是否发生回归。
5. 长稳观察：涉及内存、网络、HTTP、MQTT、LVGL 或任务调度的修改，至少观察 15 分钟；重要版本继续观察数小时。
6. 记录证据：保存命令、退出码、固件大小、设备 uptime、heap、MQTT/TF/Web 状态和关键日志。
7. 更新文档：用户确认 `OK` 后，立即更新 README §11；任何大修改即使还未最终确认，也要更新本文件的变更记录或 README 的待验证项。

## 5. 重要配置和注意事项

### 5.1 `sdkconfig` 管理

- `sdkconfig.defaults` 是可复现默认值；`sdkconfig` 是本机当前生效值。
- 普通改动不要删除 `sdkconfig`。
- Kconfig 新增后如出现 `CONFIG_xxx undeclared`，先执行 `reconfigure`，再重新编译。
- 当前稳定配置包括：LVGL 使用 libc malloc、LVGL 绘制缓冲放 PSRAM、CJK 压缩字体开启、LCD 旋转 180°、偏移 0/0、RGB 模式、双 OTA 分区。

### 5.2 硬件资源

- GPIO15 同时为 W5500 RST 和 LCD RST，LCD 驱动不得再次操作该复位 GPIO。
- GPIO14 为 W5500 中断。
- GPIO1–9、39–42、47–48 是 Web GPIO 控制白名单；不要绕过白名单控制板载占用脚。
- W5500 使用 SPI2，LCD 使用 SPI3；不要把两者改到同一 SPI 总线而不重新核对 CS、INT、RST。
- 8MB PSRAM 用于 LVGL 大缓冲；不要把整帧缓冲或大内存池移回内部 RAM。

### 5.3 网络和 MQTT

- ETH 优先，WiFi 兜底；切换由 `net_manager` 负责。
- MQTT 当前实测 broker 为 `192.168.99.253:1883`，匿名接入。
- 传感器主题为 `esp32-sensor/data`，正常约 15 秒一条。
- 传感器掉线阈值默认 120000ms，告警主题为 `ha/esp32gw/alert`。
- 不要把局域网设备地址通过代理访问；本机存在代理时，curl 使用 `--noproxy "*"`。
- DHCP 地址可能变化，验证时以 MQTT state 或 `/api/status` 返回的活动 IP 为准。

### 5.4 Web 和 OTA

- Web 默认无 Token，仅适用于可信局域网；跨网段部署前必须开启 Token。
- HTTP 服务长时间运行异常时，先用 MQTT state 判断设备是否仍在运行，再用串口抓日志。
- HTTP 不通时不要反复 OTA；先确认 TCP 80、活动 IP、ARP 和串口连接。
- Windows 出现“能 ping 但 HTTP 不通”时，先清理 ARP 缓存并确认访问的是设备当前 IP。

### 5.5 时间和 TF

- 优先 SNTP；局域网外网不可用时使用 HA 的 HTTP `Date` 响应头校时。
- 未校时时日志可能写入 `log_uptime.csv`；校时后写入 `log_YYYYMMDD.csv`。
- 修改 TF 写入、CSV 格式、历史回灌前，必须验证掉电/重启后的文件可读性和历史曲线恢复。

## 6. 大修改文档同步规则

以下任一项都属于“大修改”：

- 新增或删除组件、任务、网络协议、MQTT 主题、HTTP API、GPIO 控制能力。
- 修改 Kconfig 默认值、分区表、Flash/PSRAM 使用、LCD 参数、网络切换策略。
- 修复崩溃、内存耗尽、HTTP/MQTT 长稳、OTA 或数据丢失问题。
- 任何需要重新烧录并进行真机验证的改动。

完成大修改后必须同步：

1. `README.md`：架构/配置/API/验证结果和踩坑记录。
2. README §11：用户确认 OK 后记录最终确认参数、日期、实测证据。
3. `PINMAP.md`：引脚、复用和电气约束发生变化时更新。
4. `DEVICES.md`：新增模块化设备或 GPIO 设备时更新。
5. `SOLUTIONS.md`：方案、依赖、选型发生变化时更新。
6. 本文件：若构建、烧录、验证流程或注意事项发生变化，更新对应章节。

## 7. 故障排查顺序

| 现象 | 首先检查 | 处理 |
|------|----------|------|
| `CONFIG_xxx undeclared` | Kconfig 是否已保存、sdkconfig 是否更新 | `reconfigure` 后 build |
| 编译通过但烧录失败 | COM 端口是否存在、串口是否被 logger 占用 | 停止串口程序，重新枚举端口 |
| COM3 消失 | CH340K USB 线和正确 USB 口 | 重新插拔；必要时换线/换 USB 口 |
| 设备反复重启 | 串口 panic、heap 最低水位、LVGL 缓冲位置 | 先看启动堆画像，再检查 PSRAM 配置 |
| ETH/WiFi 不切换 | `net_manager` 日志、网线、DHCP | 确认 ETH 优先和 WiFi 认证阈值 |
| MQTT 不连接 | broker、默认路由、topic、匿名配置 | 先确认设备获 IP，再看自动重连 |
| HTTP 不通但 MQTT 正常 | 活动 IP、TCP 80、httpd 日志 | 串口抓日志；HTTP 挂掉时用串口恢复 |
| TF 没有日期文件 | SNTP/HTTP Date 校时日志 | 确认 HA 地址和 Date 响应头 |
| LCD 斜切/颜色错 | OFFSET、MADCTL、RGB_BGR | 使用 TEST_PATTERN，最终保持实测配置 |

## 8. 变更记录

| 日期 | 变更 | 验证 |
|------|------|------|
| 2026-09-27 | 增加传感器掉线告警，默认 120 秒；补充 MQTT、TF、LCD 三路告警验证 | 8 秒临时阈值端到端验证，恢复 120 秒后 40 秒无误报 |
| 2026-09-27 | LVGL 内存改为 libc malloc，绘制缓冲移 PSRAM | 长时间运行超过原崩溃点，堆水位稳定 |
| 2026-09-27 | 发现 HTTP 长时间运行后 TCP 80 不响应，MQTT 仍正常 | 待串口恢复后定位 httpd 原因 |
