# ---------------------------------------------------------------------------
# sensor_history：温湿度历史环形缓冲（Web 曲线图数据源）
# ---------------------------------------------------------------------------
idf_component_register(
    SRCS "sensor_history.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer freertos log esp_common
)