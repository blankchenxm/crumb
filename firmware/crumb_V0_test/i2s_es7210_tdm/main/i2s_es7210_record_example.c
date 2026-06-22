/*
 * ESP32 + ES7210 + SD Card 按键录音示例
 * 功能：按下GPIO19按键开始录音，松开停止录音，自动保存为RECORD1.WAV、RECORD2.WAV等
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_vfs_fat.h"
#include "driver/i2s_tdm.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "es7210.h"
#include "format_wav.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 按键配置 */
#define BUTTON_GPIO     (19)
 
 /* I2C */
 #define I2C_NUM         (0)
 #define I2C_SDA_IO      (2)
 #define I2C_SCL_IO      (1)
 
 /* I2S */
 #define I2S_MCK_IO      (42)
 #define I2S_BCK_IO      (40)
 #define I2S_WS_IO       (39)
 #define I2S_DI_IO       (15)
 
 /* SD */
 #define SD_CLK_IO       (16)
 #define SD_CMD_IO       (38)
 #define SD_DATA0_IO     (17)
 
/* Audio config */
#define SAMPLE_RATE     (48000)
#define CHANNELS        (2)
#define SAMPLE_BITS     (I2S_DATA_BIT_WIDTH_16BIT)
#define MCLK_MULTIPLE   (I2S_MCLK_MULTIPLE_256)

#define TAG "example"

// 静态全局缓冲区（增大缓冲区以减少写入频率）
static int16_t i2s_read_buff[8192];  // 16KB buffer

// 文件计数器
static int file_counter = 1;
 
 /* ==================== Audio Init ==================== */
 static i2s_chan_handle_t i2s_init(void)
 {
     i2s_chan_handle_t i2s_rx_chan;
     i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
     ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &i2s_rx_chan));
 
     i2s_tdm_config_t conf = {
         .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
             SAMPLE_BITS, I2S_SLOT_MODE_STEREO, I2S_TDM_SLOT0 | I2S_TDM_SLOT1),
         .clk_cfg = {
             .clk_src = I2S_CLK_SRC_DEFAULT,
             .sample_rate_hz = SAMPLE_RATE,
             .mclk_multiple = MCLK_MULTIPLE,
         },
         .gpio_cfg = {
             .mclk = I2S_MCK_IO,
             .bclk = I2S_BCK_IO,
             .ws   = I2S_WS_IO,
             .dout = -1,
             .din  = I2S_DI_IO,
         },
     };
     ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(i2s_rx_chan, &conf));
     return i2s_rx_chan;
 }
 
 static void codec_init(void)
 {
     i2c_config_t conf = {
         .sda_io_num = I2C_SDA_IO,
         .scl_io_num = I2C_SCL_IO,
         .mode = I2C_MODE_MASTER,
         .sda_pullup_en = GPIO_PULLUP_ENABLE,
         .scl_pullup_en = GPIO_PULLUP_ENABLE,
         .master.clk_speed = 100000,
     };
     ESP_ERROR_CHECK(i2c_param_config(I2C_NUM, &conf));
     ESP_ERROR_CHECK(i2c_driver_install(I2C_NUM, conf.mode, 0, 0, 0));
 
     es7210_dev_handle_t es7210;
     es7210_i2c_config_t i2c_conf = {.i2c_port = I2C_NUM, .i2c_addr = 0x40};
     ESP_ERROR_CHECK(es7210_new_codec(&i2c_conf, &es7210));
 
     es7210_codec_config_t cfg = {
         .i2s_format = ES7210_I2S_FMT_I2S,
         .mclk_ratio = MCLK_MULTIPLE,
         .sample_rate_hz = SAMPLE_RATE,
         .bit_width = (es7210_i2s_bits_t)SAMPLE_BITS,
         .mic_bias = ES7210_MIC_BIAS_2V87,
         .mic_gain = ES7210_MIC_GAIN_30DB,
         .flags.tdm_enable = true
     };
     ESP_ERROR_CHECK(es7210_config_codec(es7210, &cfg));
 }
 
 static sdmmc_card_t *mount_sd(void)
 {
     esp_vfs_fat_sdmmc_mount_config_t mnt = {
         .format_if_mount_failed = false,
         .max_files = 5,
         .allocation_unit_size = 16 * 1024,  // 增大到16KB以提升性能
     };
     sdmmc_card_t *card;
     sdmmc_host_t host = SDMMC_HOST_DEFAULT();
     host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;  // 使用高速模式(40MHz)

     sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
     slot.width = 1;
     slot.clk = SD_CLK_IO;
     slot.cmd = SD_CMD_IO;
     slot.d0  = SD_DATA0_IO;
     slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

     esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mnt, &card);
     if (ret != ESP_OK) {
         ESP_LOGE(TAG, "Failed to mount SD card: %s (0x%x)", esp_err_to_name(ret), ret);
         ESP_LOGE(TAG, "Please check:");
         ESP_LOGE(TAG, "  1. SD card is inserted");
         ESP_LOGE(TAG, "  2. SD card pins: CLK=%d, CMD=%d, DATA0=%d", SD_CLK_IO, SD_CMD_IO, SD_DATA0_IO);
         ESP_LOGE(TAG, "  3. SD card is formatted as FAT32");
         ESP_LOGE(TAG, "  4. Try a different SD card");
         return NULL;
     }

     ESP_LOGI(TAG, "SD card mounted successfully");
     sdmmc_card_print_info(stdout, card);
     return card;
 }
 
/* ==================== 按键初始化 ==================== */
static void button_init(void)
{
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE,  // 使用内部上拉
    };
    gpio_config(&io_conf);
    ESP_LOGI(TAG, "Button initialized on GPIO%d", BUTTON_GPIO);
}

/* ==================== 按键触发录音 ==================== */
static void record_wav_on_button_press(i2s_chan_handle_t ch, bool sd_available)
{
    if (!sd_available) {
        ESP_LOGW(TAG, "SD card not available, cannot record");
        return;
    }

    char filename[32];
    snprintf(filename, sizeof(filename), "/sdcard/RECORD%d.WAV", file_counter);

    FILE *f = fopen(filename, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file: %s", filename);
        return;
    }

    // 先写入WAV头（数据大小先写0，录音完成后再更新）
    wav_header_t hdr = WAV_HEADER_PCM_DEFAULT(0, SAMPLE_BITS, SAMPLE_RATE, CHANNELS);
    fwrite(&hdr, sizeof(hdr), 1, f);

    ESP_LOGI(TAG, "Recording started... (%s)", filename);

    size_t total_written = 0, br;
    int flush_counter = 0;

    // 持续录音直到按键松开
    while (gpio_get_level(BUTTON_GPIO) == 0) {  // 按键按下时为低电平
        esp_err_t ret = i2s_channel_read(ch, i2s_read_buff, sizeof(i2s_read_buff), &br, pdMS_TO_TICKS(1000));
        if (ret == ESP_OK && br > 0) {
            size_t written = fwrite(i2s_read_buff, 1, br, f);
            if (written != br) {
                ESP_LOGE(TAG, "fwrite failed: expected %d, wrote %d", br, written);
            }
            total_written += written;

            // 每写入约64KB就flush一次，确保数据及时写入SD卡
            flush_counter++;
            if (flush_counter >= 4) {  // 4 * 16KB = 64KB
                fflush(f);
                flush_counter = 0;
                vTaskDelay(pdMS_TO_TICKS(5));  // 给SD卡5ms处理时间
            }
        } else {
            ESP_LOGW(TAG, "i2s_channel_read failed or no data: %s, bytes: %d", esp_err_to_name(ret), br);
        }
    }

    // 最后flush确保所有数据写入
    fflush(f);
    
    // 更新WAV头中的数据大小
    wav_header_t hdr_final = WAV_HEADER_PCM_DEFAULT(total_written, SAMPLE_BITS, SAMPLE_RATE, CHANNELS);
    fseek(f, 0, SEEK_SET);
    fwrite(&hdr_final, sizeof(hdr_final), 1, f);
    
    fclose(f);
    
    float duration = (float)total_written / (SAMPLE_RATE * CHANNELS * SAMPLE_BITS / 8);
    ESP_LOGI(TAG, "Recording stopped. Saved to %s (%.2f seconds, %d bytes)", filename, duration, (int)total_written);
    
    file_counter++;
}
 
/* ==================== main ==================== */
void app_main(void)
{
    // 初始化I2S和编解码器
    i2s_chan_handle_t ch = i2s_init();
    codec_init();

    // 挂载SD卡
    sdmmc_card_t *card = mount_sd();
    bool sd_available = (card != NULL);

    if (!sd_available) {
        ESP_LOGW(TAG, "Continuing without SD card - recording will not work");
    }

    // 初始化按键
    button_init();

    // 启用I2S通道（保持启用状态以便随时录音）
    ESP_ERROR_CHECK(i2s_channel_enable(ch));
    ESP_LOGI(TAG, "I2S channel enabled");

    // 给I2S和编解码器一些预热时间
    vTaskDelay(pdMS_TO_TICKS(100));

    if (sd_available) {
        ESP_LOGI(TAG, "System ready. Press button on GPIO%d to start recording.", BUTTON_GPIO);
    } else {
        ESP_LOGW(TAG, "System running without SD card. Fix SD card issue to enable recording.");
    }

    // 主循环：等待按键按下
    int last_button_state = 1;  // 1=松开，0=按下

    while (1) {
        int current_button_state = gpio_get_level(BUTTON_GPIO);

        // 检测按键按下（从高到低的跳变）
        if (last_button_state == 1 && current_button_state == 0) {
            ESP_LOGI(TAG, "Button pressed!");
            record_wav_on_button_press(ch, sd_available);
        }

        last_button_state = current_button_state;
        vTaskDelay(pdMS_TO_TICKS(50));  // 50ms防抖延迟
    }
    
    // 注意：下面的代码实际上不会执行到，因为上面是无限循环
    // 如果需要优雅退出，可以添加退出条件
    // i2s_channel_disable(ch);
    // esp_vfs_fat_sdcard_unmount("/sdcard", card);
}
 