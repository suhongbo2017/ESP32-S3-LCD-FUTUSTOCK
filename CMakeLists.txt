# ============================================================================
# ESP32-S3 双网冗余 IoT 网关 + 本地看板（方案 A）—— 工程顶层 CMakeLists
# 参照《SOLUTIONS.md》方案 A：网口为主链路、WiFi 兜底、TF 记录、屏显示。
# 要求：ESP-IDF >= 5.2（W5500 驱动已内置于 esp_eth 组件，IDF >= 5.1）
# ============================================================================
cmake_minimum_required(VERSION 3.16)

# components/ 下每个子目录即一个自定义组件
set(EXTRA_COMPONENT_DIRS components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32s3_gateway)