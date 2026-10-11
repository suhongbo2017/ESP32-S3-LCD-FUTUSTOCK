# 模块化 GPIO 设备扩展设计（DEVICES.md）

> 状态：**设计稿（未实现）** — 2026-09-26
> 目的：将来要接新的 GPIO 外设（继电器、按钮、传感器、调光…）时，**加设备 = 加一个文件 + 注册一行**，
> 其余（网页控件、HA 实体、MQTT 读写、历史曲线、引脚保护）全部自动获得。
>
> 阅读顺序：先看 §1 目标 → §3 核心 API → §6 新增设备步骤（最关键）→ §9 待定决策。

---

## 1. 目标与设计原则

| 目标 | 说明 |
|------|------|
| **一处新增** | 新设备只写自己的驱动文件，不碰 web_console / ha_mqtt / ui_dashboard |
| **自动集成** | 网页自动渲染控件；HA 自动生成实体；MQTT 自动可读写；传感器自动进曲线与 TF 记录 |
| **引脚安全** | 统一白名单校验，禁止动板载占用引脚 |
| **零依赖优先** | 先用 ESP-IDF 自带外设（GPIO/ADC/LEDC），需要时再引入外部组件 |
| **可回退** | 框架本身不影响现有功能；设备可单独启用/禁用 |

---

## 2. 目录结构（规划）

```
components/
├── device_registry/            # 框架（只写一次）
│   ├── CMakeLists.txt
│   ├── Kconfig                 # 开关、默认轮询周期
│   ├── include/device_registry.h
│   └── device_registry.c       # 注册表、遍历、读写、引脚校验
└── devices/                    # 具体设备（每加一个设备加一个 .c）
    ├── CMakeLists.txt
    ├── dev_gpio_out.c          # 继电器 / LED（输出）
    ├── dev_gpio_in.c           # 按钮 / 干接点（输入）
    ├── dev_adc.c               # 光敏 / 电位器（模拟量）
    ├── dev_pwm.c               # 调光 / 调速（LEDC）
    └── dev_onewire.c           # DS18B20 等单总线（后续，需额外组件）
```

---

## 3. 核心 API（规划）

### 3.1 设备类型与描述符

```c
typedef enum {
    DEV_OUT    = 0,   /* 开关量输出：继电器/LED      → 网页开关，HA switch        */
    DEV_IN     = 1,   /* 开关量输入：按钮/干接点      → 网页只读，HA binary_sensor */
    DEV_ADC    = 2,   /* 模拟量输入：光敏/电位器      → 网页只读，HA sensor        */
    DEV_PWM    = 3,   /* 占空比输出：调光/调速        → 网页滑块，HA number/light  */
    DEV_SENSOR = 4,   /* 复合传感器（如温湿度）        → 网页数值，HA sensor + 可进曲线 */
} dev_kind_t;

typedef struct device device_t;

struct device {
    const char *id;        /*!< 唯一标识，如 "relay1"（用于 MQTT 主题/URL） */
    const char *name;      /*!< 显示名，如 "客厅灯" */
    dev_kind_t  kind;      /*!< 类型（决定 UI 与 HA 呈现） */
    int         pin;       /*!< 主引脚（多引脚设备见 dev->priv） */
    const char *unit;      /*!< 单位："℃" "%" "lx" "" */
    float       min, max;  /*!< 量程（PWM/ADC 决定滑块与换算） */
    void       *priv;      /*!< 设备私有数据（多引脚、校准系数等） */
    bool        want_history; /*!< SENSOR 类是否进历史曲线 + TF 记录 */

    esp_err_t (*init) (device_t *d);
    esp_err_t (*read) (device_t *d, float *out);   /* 只读类实现；否则可 NULL */
    esp_err_t (*write)(device_t *d, float val);    /* 可写类实现；否则可 NULL */
};
```

### 3.2 注册表 API

```c
esp_err_t device_registry_init(void);                 /* 启动时遍历已注册设备并 init() */
esp_err_t device_register(const device_t *dev);       /* 驱动文件里调用，一行 */
int       device_count(void);
const device_t *device_at(int idx);                   /* 遍历用 */
const device_t *device_find(const char *id);
int       device_snapshot_all(void);                  /* 刷新所有可读设备的当前值 */
esp_err_t device_set(const char *id, float val);      /* 写（带 kind 校验 + 白名单） */
esp_err_t device_get(const char *id, float *out);     /* 读 */
```

> **可选进阶**：用 `__attribute__((constructor))` 让设备**自注册**（连"注册一行"都省掉）。
> 先用显式注册（更可控、易调试），确认稳定后再考虑自注册。

---

## 4. 类型 → 呈现映射（自动集成规则）

| kind | 网页控件 | HA 实体 | MQTT | 备注 |
|------|----------|---------|------|------|
| `DEV_OUT` | 开关（开/关） | `switch` | `<prefix>/dev/<id>/set` ← / `.../state` → | 状态本地保持 |
| `DEV_IN` | 只读电平 | `binary_sensor` | `.../state` → | 轮询/中断 |
| `DEV_ADC` | 只读数值 + 单位 | `sensor` | `.../state` → | 需校准（原始值→物理量） |
| `DEV_PWM` | 滑块（min–max） | `number`（或 `light`） | `.../set` ← / `.../state` → | LEDC |
| `DEV_SENSOR` | 数值（可进曲线） | `sensor` | `.../state` → | `want_history` 时进曲线与 TF |
| ~~（旧）~~ | ~~截图~~ | — | — | 已移除（见 README §11.5） |

MQTT 主题前缀沿用现有约定：`ha/esp32gw/dev/<id>/{set,state}`。

---

## 5. 自动集成点（框架负责，设备无需关心）

1. **web_console**
   - `GET /api/devices` → 数组：`{id,name,kind,unit,min,max,value,readable,writable}`
   - `GET /api/device?id=x` / `POST /api/device?id=x&value=v`
   - 首页新增「**设备**」卡片：按 `kind` 自动渲染控件（开关/滑块/数值），周期刷新
2. **ha_mqtt**：连接成功后按设备列表批量发 Discovery；收到 `.../set` 调 `device_set()`；周期/变化时发 `.../state`
3. **sensor_history**：`DEV_SENSOR && want_history` 的设备值进环形缓冲 → 复用现有曲线
4. **tf_storage**：同上设备按周期落盘（`type=<id>`）
5. **引脚白名单**：`device_registry` 内部统一校验（见 §8），设备**不需要**自己判断

---

## 6. 新增一个设备的完整步骤（★ 最关键）

### 6.1 写驱动文件 `components/devices/dev_relay.c`

```c
#include "device_registry.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "dev_relay";

static esp_err_t relay_init(device_t *d)
{
    gpio_reset_pin(d->pin);
    gpio_set_direction(d->pin, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level(d->pin, 0);                 /* 上电默认关 */
    return ESP_OK;
}

static esp_err_t relay_read(device_t *d, float *out)
{
    *out = (float)gpio_get_level(d->pin);
    return ESP_OK;
}

static esp_err_t relay_write(device_t *d, float val)
{
    gpio_set_level(d->pin, val > 0.5f ? 1 : 0);
    return ESP_OK;
}

/* 设备描述符 + 自注册 */
static const device_t s_relay1 = {
    .id = "relay1", .name = "继电器1", .kind = DEV_OUT,
    .pin = 5, .min = 0, .max = 1,
    .init = relay_init, .read = relay_read, .write = relay_write,
};

esp_err_t dev_relay_register(void) { return device_register(&s_relay1); }
```

### 6.2 在 `components/devices/devices.c` 里加一行

```c
esp_err_t dev_relay_register(void);
/* ... */
void devices_register_all(void)
{
    dev_relay_register();      /* ← 新增设备只加这一行 */
    dev_button_register();
    dev_ldr_register();
}
```

### 6.3 完成

编译烧录后自动得到：**网页出现控件、HA 出现对应实体、MQTT 可读写**。无需改 web/mqtt/ui 任何代码。

---

## 7. 示例设备要点

| 设备 | 关键点 |
|------|--------|
| `dev_gpio_out.c` | 上电默认电平可配（避免上电误动作）；`write` 后立即回报状态 |
| `dev_gpio_in.c` | 可选**中断**（`gpio_install_isr_service` 已在 net_eth 装过）或周期轮询；去抖 |
| `dev_adc.c` | `adc_oneshot`（IDF v5 新 API）；需 `adc_cali` 校准；量程换算到物理量 |
| `dev_pwm.c` | `ledc_timer_config` + `ledc_channel_config`；占空比 0–100% 映射 min–max |
| `dev_onewire.c` | DS18B20：需 RMT 或现成组件；**单总线时序敏感**，建议最后做 |

---

## 8. 引脚白名单与安全（复用现有规则）

| 可用（引出且空闲） | 禁止（板载占用） |
|--------------------|------------------|
| **GPIO1–9、39–42、47–48** | 0(BOOT)、10–18,21(W5500/LCD)、19–20(USB)、26–32(Flash/PSRAM)、33–38(TF)、43–44(UART0)、45–46(strapping) |

- 注册时校验：`pin` 不在白名单 → `device_register()` 返回错误并打印告警（**不注册**）
- 冲突检查（建议）：同一 `pin` 被两个设备占用 → 拒绝后者
- 引脚分配表建议维护在 `stocks` 之外的 `DEVICE_PINS.md`（避免与 `PINMAP.md` 混淆）

---

## 9. 待定决策（实现前需拍板）

| # | 问题 | 选项 | 建议 |
|---|------|------|------|
| 1 | **设备清单放哪** | A. 代码里（每设备一个 .c，加设备=加文件）<br>B. 运行时配置（网页增删，存 NVS） | **A**（简单可靠）；B 留作以后 |
| 2 | **注册方式** | A. 显式 `device_register()`<br>B. `__attribute__((constructor))` 自注册 | **A** 起步 |
| 3 | **首次实现哪些类型** | OUT / IN / ADC / PWM / SENSOR(单总线) | **OUT + IN + ADC + PWM**（纯片上外设） |
| 4 | **HA 自动暴露** | 是/否 | **是**（Discovery，价值最高） |
| 5 | **PWM 实现** | LEDC / MCPWM / RMT | **LEDC**（简单够用） |
| 6 | **轮询周期** | 统一周期 vs 每设备独立 | 统一 Kconfig（默认 2s），设备可覆盖 |

---

## 10. 分期实施计划（将来按需启动）

| 阶段 | 内容 | 预估工作量 |
|------|------|-----------|
| **P0** | `device_registry` 框架 + `dev_gpio_out` 一个示例 + `/api/devices` + 网页通用控件 | 小 |
| **P1** | HA Discovery 自动实体 + MQTT `dev/<id>/{set,state}` | 中 |
| **P2** | `dev_gpio_in` / `dev_adc` / `dev_pwm` | 中 |
| **P3** | `DEV_SENSOR` 接入曲线与 TF 记录；单总线设备（按需） | 中 |

> **触发条件**：等你确定要接的具体外设（型号 + 引脚）时再启动，避免空转。

---

## 11. 依赖与注意

- 组件依赖：`driver`（GPIO/ADC/LEDC）、`esp_timer`、`freertos`；P0–P2 **无需外部组件**
- 与现有组件的耦合方向：**设备 → 框架 → (web/mqtt/ui)**，反向不依赖，避免循环
- 初始化顺序：`net_manager_init()` 之后、`web_console_init()` 之前调 `device_registry_init()`
- 复位线共用提醒：**GPIO15 为 W5500/LCD 共用复位**，任何新设备都不得占用（已在白名单外）