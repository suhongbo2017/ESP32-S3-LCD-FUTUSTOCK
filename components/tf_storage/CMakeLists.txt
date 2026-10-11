# ---------------------------------------------------------------------------
# tf_storage：TF(microSD) 卡历史记录（SDMMC 4-bit + FatFS，按天 CSV）
# ---------------------------------------------------------------------------
idf_component_register(
    SRCS "tf_storage.c"
    INCLUDE_DIRS "include"
    REQUIRES board fatfs sdmmc esp_timer log esp_common freertos
)