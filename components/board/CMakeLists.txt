# ---------------------------------------------------------------------------
# board：板级引脚/硬件参数组件（纯头文件，无源文件）
# 全工程唯一的引脚定义来源，见 include/board_pins.h（引脚依据 PINMAP.md）
# ---------------------------------------------------------------------------
idf_component_register(
    INCLUDE_DIRS "include"
    REQUIRES driver     # 提供 gpio_num_t / SPI2_HOST 等类型与常量
)