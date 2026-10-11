# ---------------------------------------------------------------------------
# lcd_st7735：ST7735S 1.77" TFT 驱动（esp_lcd_panel_io_spi + 自写初始化序列）
#  - LCD 独立挂 SPI3_HOST（W5500 独占 SPI2，避免总线冲突）
#  - RST 与 W5500_RST 共用 GPIO15，驱动内 reset_gpio_num 恒为 -1（见代码注释）
# ---------------------------------------------------------------------------
idf_component_register(
    SRCS "lcd_st7735.c"
    INCLUDE_DIRS "include"
    REQUIRES board esp_lcd driver log esp_common freertos
)