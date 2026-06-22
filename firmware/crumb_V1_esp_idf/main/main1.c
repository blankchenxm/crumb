/*
 * Simple I2C scanner for ESP-IDF v5.x
 *
 * Purpose:
 *   Verify whether the I2C bus is electrically alive before debugging
 *   a specific device such as BQ25180.
 *
 * Current pin mapping:
 *   SCL = GPIO27
 *   SDA = GPIO14
 */

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"

#define I2C_SCL_PIN         27
#define I2C_SDA_PIN         14
#define I2C_PORT            I2C_NUM_0
#define I2C_SCAN_START      0x08
#define I2C_SCAN_END        0x77
#define I2C_PROBE_TIMEOUT   20
#define I2C_SCAN_INTERVAL   5000

static const char *TAG = "I2C_SCANNER";

static i2c_master_bus_handle_t init_i2c_bus(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    i2c_master_bus_handle_t bus = NULL;
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(ret));
        return NULL;
    }

    return bus;
}

static void scan_i2c_bus(i2c_master_bus_handle_t bus)
{
    int found = 0;

    ESP_LOGI(TAG, "Scanning I2C bus on SCL=GPIO%d SDA=GPIO%d", I2C_SCL_PIN, I2C_SDA_PIN);
    for (uint8_t addr = I2C_SCAN_START; addr <= I2C_SCAN_END; addr++) {
        esp_err_t ret = i2c_master_probe(bus, addr, pdMS_TO_TICKS(I2C_PROBE_TIMEOUT));
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "  Found device at 0x%02X", addr);
            found++;
        }
    }

    if (found == 0) {
        ESP_LOGW(TAG, "No I2C devices found");
        ESP_LOGW(TAG, "Check power, GND, SDA/SCL routing, and external pull-ups to 3.3V");
    } else {
        ESP_LOGI(TAG, "Scan complete: %d device(s) found", found);
    }
}

void app_main(void)
{
    esp_log_level_set("gpio", ESP_LOG_VERBOSE);
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "=== ESP-IDF I2C Scanner ===");
    ESP_LOGI(TAG, "Internal pull-ups: enabled");
    ESP_LOGI(TAG, "Recommended hardware: add external 4.7k pull-ups on SDA and SCL");

    /* Suppress timeout spam for addresses that do not ACK during scanning. */
    esp_log_level_set("i2c.master", ESP_LOG_NONE);

    i2c_master_bus_handle_t bus = init_i2c_bus();
    if (bus == NULL) {
        return;
    }

    while (true) {
        scan_i2c_bus(bus);
        vTaskDelay(pdMS_TO_TICKS(I2C_SCAN_INTERVAL));
    }
}
