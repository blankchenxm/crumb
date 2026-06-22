set(APP_ENTRY_SOURCE "main3.c" CACHE STRING "Entry source under main/" FORCE)

idf_component_register(
    SRCS "main3.c" "${APP_ENTRY_SOURCE}"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_driver_gpio
        esp_common
        freertos
        log
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_spi
        esp_event
        esp_netif
        esp_wifi
        esp_http_client
        esp-tls
        mbedtls
        nvs_flash
)
