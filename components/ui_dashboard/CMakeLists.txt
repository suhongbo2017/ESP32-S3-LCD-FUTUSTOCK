# ---------------------------------------------------------------------------
# ui_dashboard：LVGL 本地看板（ST7735S 128x160 显示双网状态）
#  - 依赖 lvgl / esp_lvgl_port（Component Manager 下载）与 lcd_st7735 / net_manager
# ---------------------------------------------------------------------------
idf_component_register(
    SRCS "ui_dashboard.c" "fonts/lv_font_cjk_14.c"
    INCLUDE_DIRS "include" "fonts"
    REQUIRES lvgl esp_lvgl_port lcd_st7735 net_manager market_data ha_mqtt board
             log esp_timer freertos esp_common
)