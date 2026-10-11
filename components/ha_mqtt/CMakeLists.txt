# ---------------------------------------------------------------------------
# ha_mqtt：Home Assistant MQTT 接入（esp-mqtt 封装）
#  - 连接本地 broker（Kconfig 可配），订阅 ha/esp32gw/notify 通知，发布状态/Discovery
#  - 通知通过消息队列 + 独立任务投递给 UI（不在 esp-mqtt 回调里直接操作 LVGL）
# ---------------------------------------------------------------------------
idf_component_register(
    SRCS "ha_mqtt.c"
    INCLUDE_DIRS "include"
    REQUIRES mqtt json net_manager tf_storage app_cfg sensor_history log esp_timer esp_common freertos
)