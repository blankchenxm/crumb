idf_component_register(SRCS "i2s_es7210_record_example.c"
                       INCLUDE_DIRS "."
                       REQUIRES es7210 driver fatfs esp_wifi nvs_flash esp_http_client
)
