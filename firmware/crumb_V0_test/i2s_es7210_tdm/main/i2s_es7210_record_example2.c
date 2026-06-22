/*
 * ESP32 + ES7210 + SD Card 录音 & WiFi 自动上传示例（修复版）
 * 
 * 主要修复：
 * 1. I2S初始化顺序优化
 * 2. 增加I2S预热时间
 * 3. 调整I2S读取超时时间
 * 4. 添加I2S状态检查
 */

 #include <stdio.h>
 #include <string.h>
 #include <sys/unistd.h>
 #include <sys/stat.h>
 #include <dirent.h>
 #include <errno.h>
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
 #include "freertos/queue.h"
 #include "freertos/semphr.h"
 #include "freertos/event_groups.h"
 #include "esp_system.h"
 #include "esp_mac.h"
 #include "esp_wifi.h"
 #include "esp_event.h"
 #include "nvs_flash.h"
 #include "esp_netif.h"
 #include "esp_http_client.h"
 #include "lwip/err.h"
 #include "lwip/sys.h"
 
 /* ================= 用户配置区域 ================= */
 #define WIFI_SSID      "blankmeng"
 #define WIFI_PASS      "chen106416336"
 #define SERVER_URL     "http://47.98.164.80:8000/upload"
 
 /* ================= 硬件引脚配置 ================= */
 #define BUTTON_GPIO     (19)
 #define I2C_NUM         (0)
 #define I2C_SDA_IO      (2)
 #define I2C_SCL_IO      (1)
 #define I2S_MCK_IO      (42)
 #define I2S_BCK_IO      (40)
 #define I2S_WS_IO       (39)
 #define I2S_DI_IO       (15)
 #define SD_CLK_IO       (16)
 #define SD_CMD_IO       (38)
 #define SD_DATA0_IO     (17)
 
 /* Audio config */
 #define SAMPLE_RATE     (48000)
 #define CHANNELS        (1)
 #define SAMPLE_BITS     (I2S_DATA_BIT_WIDTH_16BIT)
 #define MCLK_MULTIPLE   (I2S_MCLK_MULTIPLE_256)
 
 /* 🔧 修复1: 调整I2S读取参数 */
 #define I2S_READ_BUFFER_SIZE    (4096)    // 保持4KB缓冲区
 #define I2S_READ_TIMEOUT_MS     (500)     // 增加到500ms（原100ms）
 
 #define TAG "REC_UPLOAD"
 
 // 静态全局缓冲区
 static int16_t i2s_read_buff[I2S_READ_BUFFER_SIZE / sizeof(int16_t)];
 static bool s_wifi_connected = false;
 
 /* ================= FreeRTOS 多任务全局变量 ================= */
 typedef enum {
     SYS_STATE_IDLE,
     SYS_STATE_RECORDING,
     SYS_STATE_UPLOADING
 } system_state_t;
 
 static QueueHandle_t upload_queue = NULL;
 static SemaphoreHandle_t sd_mutex = NULL;
 static EventGroupHandle_t system_events = NULL;
 static volatile system_state_t g_system_state = SYS_STATE_IDLE;
 static i2s_chan_handle_t g_i2s_rx_chan = NULL;
 static char g_chip_id[5] = {0};
 
 #define EVENT_WIFI_CONNECTED    (1 << 0)
 #define EVENT_BUTTON_PRESSED    (1 << 1)
 #define EVENT_RECORDING_DONE    (1 << 2)
 #define EVENT_UPLOAD_PENDING    (1 << 3)
 
 /* ==================== WiFi 功能模块 ==================== */
 static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                 int32_t event_id, void* event_data)
 {
     if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
         esp_wifi_connect();
     } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
         s_wifi_connected = false;
         ESP_LOGW(TAG, "WiFi disconnected. Retrying...");
         esp_wifi_connect();
     } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
         ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
         ESP_LOGI(TAG, "WiFi Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
         s_wifi_connected = true;
     }
 }
 
 static void wifi_init_sta(void)
 {
     esp_netif_init();
     esp_event_loop_create_default();
     esp_netif_create_default_wifi_sta();
 
     wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
     esp_wifi_init(&cfg);
 
     esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
     esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);
 
     wifi_config_t wifi_config = {
         .sta = {
             .ssid = WIFI_SSID,
             .password = WIFI_PASS,
             .threshold.authmode = WIFI_AUTH_WPA2_PSK,
         },
     };
     esp_wifi_set_mode(WIFI_MODE_STA);
     esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
     esp_wifi_start();
 }
 
 /* ==================== 上传功能模块 ==================== */
 static esp_err_t upload_file(const char *filepath) {
     esp_err_t err = ESP_FAIL;
     FILE *f = fopen(filepath, "rb");
     if (f == NULL) {
         ESP_LOGE(TAG, "Failed to open file: %s", filepath);
         return ESP_FAIL;
     }
 
     fseek(f, 0, SEEK_END);
     long file_size = ftell(f);
     fseek(f, 0, SEEK_SET);
 
     ESP_LOGI(TAG, "Preparing to upload: %s (%ld bytes)", filepath, file_size);
 
     esp_http_client_config_t config = {
         .url = SERVER_URL,
         .method = HTTP_METHOD_POST,
         .timeout_ms = 15000,
         .buffer_size_tx = 4096,
     };
     esp_http_client_handle_t client = esp_http_client_init(&config);
 
     const char *filename = strrchr(filepath, '/');
     filename = (filename == NULL) ? filepath : filename + 1;
 
     char url_with_query[256];
     snprintf(url_with_query, sizeof(url_with_query), "%s?filename=%s", SERVER_URL, filename);
     esp_http_client_set_url(client, url_with_query);
     esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
 
     if ((err = esp_http_client_open(client, file_size)) != ESP_OK) {
         ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
         fclose(f);
         esp_http_client_cleanup(client);
         return err;
     }
 
     char *buffer = malloc(8192);
     if (buffer == NULL) {
         ESP_LOGE(TAG, "No memory for buffer");
         fclose(f);
         esp_http_client_cleanup(client);
         return ESP_ERR_NO_MEM;
     }
 
     long total_sent = 0;
     while (true) {
         size_t read_bytes = fread(buffer, 1, 8192, f);
         if (read_bytes == 0) break;
 
         int write_bytes = esp_http_client_write(client, buffer, read_bytes);
         if (write_bytes < 0) {
             ESP_LOGE(TAG, "HTTP write failed");
             err = ESP_FAIL;
             break;
         }
         total_sent += write_bytes;
     }
     free(buffer);
 
     esp_http_client_fetch_headers(client);
     int status_code = esp_http_client_get_status_code(client);
 
     ESP_LOGI(TAG, "HTTP Status: %d, Sent: %ld bytes", status_code, total_sent);
 
     esp_http_client_close(client);
     esp_http_client_cleanup(client);
     fclose(f);
 
     if (status_code == 200) {
         return ESP_OK;
     } else {
         return ESP_FAIL;
     }
 }
 
 /* ==================== 文件命名功能模块 ==================== */
 static void init_chip_id(void) {
     uint8_t mac[6];
     esp_err_t ret = esp_read_mac(mac, ESP_MAC_WIFI_STA);
     if (ret != ESP_OK) {
         ESP_LOGE(TAG, "Failed to read MAC address");
         snprintf(g_chip_id, sizeof(g_chip_id), "0000");
     } else {
         snprintf(g_chip_id, sizeof(g_chip_id), "%02X%02X", mac[4], mac[5]);
     }
     ESP_LOGI(TAG, "Chip ID: %s", g_chip_id);
 }
 
 static void generate_filename(char* buf, size_t len) {
     static uint16_t session_counter = 1;
     uint32_t boot_sec = xTaskGetTickCount() / configTICK_RATE_HZ;
 
     snprintf(buf, len, "/sdcard/REC_%s_%08lu_%03d.WAV",
              g_chip_id, boot_sec, session_counter);
 
     session_counter++;
     if (session_counter > 999) {
         session_counter = 1;
     }
 }
 
 static void scan_existing_recordings(void) {
     DIR* dir = opendir("/sdcard");
     if (!dir) {
         ESP_LOGW(TAG, "Failed to open /sdcard for scanning");
         return;
     }
 
     int count = 0;
     struct dirent* entry;
     while ((entry = readdir(dir)) != NULL) {
         if (strstr(entry->d_name, ".WAV") || strstr(entry->d_name, ".wav")) {
             char filepath[264];
             snprintf(filepath, sizeof(filepath), "/sdcard/%s", entry->d_name);
 
             if (xQueueSend(upload_queue, filepath, 0) == pdTRUE) {
                 count++;
                 ESP_LOGI(TAG, "Found existing recording: %s", entry->d_name);
             }
         }
     }
     closedir(dir);
 
     if (count > 0) {
         xEventGroupSetBits(system_events, EVENT_UPLOAD_PENDING);
         ESP_LOGI(TAG, "Found %d existing recordings for upload", count);
     }
 }
 
 /* ==================== 🔧 修复2: 改进的I2S初始化 ==================== */
 static i2s_chan_handle_t i2s_init(void)
 {
     ESP_LOGI(TAG, "=== Starting I2S Initialization ===");
 
     i2s_chan_handle_t i2s_rx_chan;
     
     // Step 1: 创建I2S通道
     i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
     esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &i2s_rx_chan);
     if (ret != ESP_OK) {
         ESP_LOGE(TAG, "❌ i2s_new_channel failed: %s", esp_err_to_name(ret));
         return NULL;
     }
     ESP_LOGI(TAG, "✅ I2S channel created");
 
     // Step 2: 配置TDM模式（与第一个代码完全一致）
     i2s_tdm_config_t tdm_cfg = {
         .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
             SAMPLE_BITS, 
             I2S_SLOT_MODE_STEREO,  // 虽然是STEREO，但只用SLOT0
             I2S_TDM_SLOT0          // 只使用SLOT0（单声道）
         ),
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
 
     ret = i2s_channel_init_tdm_mode(i2s_rx_chan, &tdm_cfg);
     if (ret != ESP_OK) {
         ESP_LOGE(TAG, "❌ i2s_channel_init_tdm_mode failed: %s", esp_err_to_name(ret));
         i2s_del_channel(i2s_rx_chan);
         return NULL;
     }
     ESP_LOGI(TAG, "✅ I2S TDM mode initialized");
 
     ESP_LOGI(TAG, "I2S Config: %dHz, %d-bit, MONO (SLOT0), MCLK=%d, BCLK=%d, WS=%d, DIN=%d",
              SAMPLE_RATE, 16, I2S_MCK_IO, I2S_BCK_IO, I2S_WS_IO, I2S_DI_IO);
 
     ESP_LOGI(TAG, "=== I2S Initialization Complete ===");
     return i2s_rx_chan;
 }
 
 /* ==================== 🔧 修复3: ES7210初始化保持不变 ==================== */
 static void codec_init(void)
 {
     ESP_LOGI(TAG, "=== Starting ES7210 Codec Initialization ===");
 
     // I2C配置
     i2c_config_t i2c_cfg = {
         .sda_io_num = I2C_SDA_IO,
         .scl_io_num = I2C_SCL_IO,
         .mode = I2C_MODE_MASTER,
         .sda_pullup_en = GPIO_PULLUP_ENABLE,
         .scl_pullup_en = GPIO_PULLUP_ENABLE,
         .master.clk_speed = 100000,
     };
     ESP_ERROR_CHECK(i2c_param_config(I2C_NUM, &i2c_cfg));
     ESP_ERROR_CHECK(i2c_driver_install(I2C_NUM, i2c_cfg.mode, 0, 0, 0));
     ESP_LOGI(TAG, "✅ I2C initialized");
 
     // ES7210配置
     es7210_dev_handle_t es7210;
     es7210_i2c_config_t codec_i2c_cfg = {
         .i2c_port = I2C_NUM, 
         .i2c_addr = 0x40
     };
     ESP_ERROR_CHECK(es7210_new_codec(&codec_i2c_cfg, &es7210));
     ESP_LOGI(TAG, "✅ ES7210 codec created");
 
     es7210_codec_config_t codec_cfg = {
         .i2s_format = ES7210_I2S_FMT_I2S,
         .mclk_ratio = MCLK_MULTIPLE,
         .sample_rate_hz = SAMPLE_RATE,
         .bit_width = (es7210_i2s_bits_t)SAMPLE_BITS,
         .mic_bias = ES7210_MIC_BIAS_2V87,
         .mic_gain = ES7210_MIC_GAIN_30DB,
         .flags.tdm_enable = true
     };
     ESP_ERROR_CHECK(es7210_config_codec(es7210, &codec_cfg));
     ESP_LOGI(TAG, "✅ ES7210 codec configured");
 
     ESP_LOGI(TAG, "=== ES7210 Codec Initialization Complete ===");
 }
 
 static sdmmc_card_t *mount_sd(void)
 {
     ESP_LOGI(TAG, "=== Mounting SD Card ===");
     
     esp_vfs_fat_sdmmc_mount_config_t mnt = {
         .format_if_mount_failed = false,
         .max_files = 10,
         .allocation_unit_size = 16 * 1024,
         .disk_status_check_enable = false,
     };
     sdmmc_card_t *card;
     sdmmc_host_t host = SDMMC_HOST_DEFAULT();
     sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
     slot.width = 1;
     slot.clk = SD_CLK_IO;
     slot.cmd = SD_CMD_IO;
     slot.d0  = SD_DATA0_IO;
     slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
     ESP_ERROR_CHECK(esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mnt, &card));
     
     ESP_LOGI(TAG, "✅ SD Card mounted successfully");
     return card;
 }
 
 static void button_init(void)
 {
     gpio_config_t io_conf = {
         .intr_type = GPIO_INTR_DISABLE,
         .mode = GPIO_MODE_INPUT,
         .pin_bit_mask = (1ULL << BUTTON_GPIO),
         .pull_down_en = GPIO_PULLDOWN_DISABLE,
         .pull_up_en = GPIO_PULLUP_ENABLE,
     };
     gpio_config(&io_conf);
     ESP_LOGI(TAG, "✅ Button initialized on GPIO%d", BUTTON_GPIO);
 }
 
 /* ==================== FreeRTOS 任务函数 ==================== */
 
 void button_task(void* pvParameters) {
     int last_state = 1;
     int debounce_counter = 0;
 
     ESP_LOGI(TAG, "Button task started");
 
     while (1) {
         int current_state = gpio_get_level(BUTTON_GPIO);
 
         if (current_state == last_state) {
             debounce_counter = 0;
         } else {
             debounce_counter++;
             if (debounce_counter >= 3) {
                 if (current_state == 0) {
                     xEventGroupSetBits(system_events, EVENT_BUTTON_PRESSED);
                     ESP_LOGI(TAG, "Button pressed!");
                 }
                 last_state = current_state;
                 debounce_counter = 0;
             }
         }
 
         vTaskDelay(pdMS_TO_TICKS(10));
     }
 }
 
 /* ==================== 🔧 修复4: 改进的录音任务 ==================== */
 void record_task(void* pvParameters) {
     char filename[264];
 
     ESP_LOGI(TAG, "Record task started");
 
     while (1) {
         EventBits_t bits = xEventGroupWaitBits(
             system_events,
             EVENT_BUTTON_PRESSED,
             pdTRUE,
             pdFALSE,
             portMAX_DELAY
         );
 
         if (bits & EVENT_BUTTON_PRESSED) {
             g_system_state = SYS_STATE_RECORDING;
 
             generate_filename(filename, sizeof(filename));
             ESP_LOGI(TAG, "🎙️ Recording to: %s", filename);
 
             xSemaphoreTake(sd_mutex, portMAX_DELAY);
 
             FILE* f = fopen(filename, "wb");
             if (f) {
                 ESP_LOGI(TAG, "✅ File opened successfully");
                 
                 // 写WAV头占位
                 wav_header_t hdr = WAV_HEADER_PCM_DEFAULT(0, SAMPLE_BITS, SAMPLE_RATE, CHANNELS);
                 fwrite(&hdr, sizeof(hdr), 1, f);
 
                 size_t total_written = 0, br;
                 int read_count = 0;
                 int success_count = 0;
                 int zero_read_count = 0;
                 int error_count = 0;
 
                 ESP_LOGI(TAG, "🎙️ Recording started (press and hold button)...");
 
                 // 🔧 修复: 增加I2S读取超时时间
                 while (gpio_get_level(BUTTON_GPIO) == 0) {
                     read_count++;
                     
                     // 使用更长的超时时间（500ms而不是100ms）
                     esp_err_t ret = i2s_channel_read(
                         g_i2s_rx_chan, 
                         i2s_read_buff,
                         I2S_READ_BUFFER_SIZE, 
                         &br,
                         pdMS_TO_TICKS(I2S_READ_TIMEOUT_MS)  // 500ms
                     );
 
                     if (ret == ESP_OK) {
                         if (br > 0) {
                             success_count++;
                             fwrite(i2s_read_buff, br, 1, f);
                             total_written += br;
 
                             // 每秒打印一次进度
                             if (success_count % 50 == 0) {
                                 float seconds = (float)total_written / (SAMPLE_RATE * CHANNELS * 2);
                                 ESP_LOGI(TAG, "🎙️ Recording... %.1fs (%d bytes)", seconds, (int)total_written);
                             }
                         } else {
                             zero_read_count++;
                             if (zero_read_count % 10 == 0) {
                                 ESP_LOGW(TAG, "⚠️  I2S read OK but 0 bytes (count: %d)", zero_read_count);
                             }
                         }
                     } else {
                         error_count++;
                         ESP_LOGE(TAG, "❌ I2S read failed: %s (count: %d/%d)", 
                                 esp_err_to_name(ret), error_count, read_count);
                         
                         // 如果连续错误太多，可能需要重新初始化I2S
                         if (error_count > 10) {
                             ESP_LOGE(TAG, "❌ Too many I2S errors, stopping recording");
                             break;
                         }
                     }
                 }
 
                 ESP_LOGI(TAG, "🛑 Recording stopped");
                 ESP_LOGI(TAG, "📊 Stats: reads=%d, success=%d, zeros=%d, errors=%d", 
                         read_count, success_count, zero_read_count, error_count);
 
                 // 回写正确的WAV头
                 wav_header_t hdr_final = WAV_HEADER_PCM_DEFAULT(
                     total_written, SAMPLE_BITS, SAMPLE_RATE, CHANNELS);
                 fseek(f, 0, SEEK_SET);
                 fwrite(&hdr_final, sizeof(hdr_final), 1, f);
                 fclose(f);
 
                 float duration = (float)total_written / (SAMPLE_RATE * CHANNELS * 2);
                 ESP_LOGI(TAG, "✅ Recording saved: %s (%.2f seconds, %d bytes)", 
                         filename, duration, (int)total_written);
 
                 // 加入上传队列
                 if (xQueueSend(upload_queue, filename, 0) == pdTRUE) {
                     xEventGroupSetBits(system_events, EVENT_UPLOAD_PENDING);
                 }
             } else {
                 ESP_LOGE(TAG, "❌ Failed to open file: %s, errno: %d (%s)",
                          filename, errno, strerror(errno));
             }
 
             xSemaphoreGive(sd_mutex);
             xEventGroupSetBits(system_events, EVENT_RECORDING_DONE);
             g_system_state = SYS_STATE_IDLE;
         }
     }
 }
 
 void wifi_manager_task(void* pvParameters) {
     ESP_LOGI(TAG, "WiFi manager task started");
     wifi_init_sta();
 
     while (1) {
         if (s_wifi_connected) {
             xEventGroupSetBits(system_events, EVENT_WIFI_CONNECTED);
         } else {
             xEventGroupClearBits(system_events, EVENT_WIFI_CONNECTED);
         }
 
         vTaskDelay(pdMS_TO_TICKS(5000));
     }
 }
 
 void upload_task(void* pvParameters) {
     const char* task_name = pcTaskGetName(NULL);
     char filepath[264];
 
     ESP_LOGI(TAG, "%s started", task_name);
 
     while (1) {
         xEventGroupWaitBits(
             system_events,
             EVENT_WIFI_CONNECTED | EVENT_UPLOAD_PENDING,
             pdFALSE,
             pdTRUE,
             portMAX_DELAY
         );
 
         if (g_system_state == SYS_STATE_RECORDING) {
             vTaskDelay(pdMS_TO_TICKS(100));
             continue;
         }
 
         if (xQueueReceive(upload_queue, filepath, pdMS_TO_TICKS(1000)) == pdTRUE) {
             ESP_LOGI(TAG, "[%s] Uploading: %s", task_name, filepath);
 
             xSemaphoreTake(sd_mutex, portMAX_DELAY);
             esp_err_t ret = upload_file(filepath);
             xSemaphoreGive(sd_mutex);
 
             if (ret == ESP_OK) {
                 ESP_LOGI(TAG, "[%s] Upload success, deleting: %s", task_name, filepath);
                 unlink(filepath);
             } else {
                 ESP_LOGE(TAG, "[%s] Upload failed, re-queuing: %s", task_name, filepath);
                 xQueueSend(upload_queue, filepath, 0);
             }
 
             if (uxQueueMessagesWaiting(upload_queue) == 0) {
                 xEventGroupClearBits(system_events, EVENT_UPLOAD_PENDING);
             }
         }
     }
 }
 
 /* ==================== 🔧 修复5: 改进的主函数初始化顺序 ==================== */
 void app_main(void)
 {
     ESP_LOGI(TAG, "");
     ESP_LOGI(TAG, "========================================");
     ESP_LOGI(TAG, "  ESP32 Voice Recorder & Uploader");
     ESP_LOGI(TAG, "========================================");
     ESP_LOGI(TAG, "");
 
     // ========== 1. NVS初始化 ==========
     esp_err_t ret = nvs_flash_init();
     if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
         ESP_ERROR_CHECK(nvs_flash_erase());
         ret = nvs_flash_init();
     }
     ESP_ERROR_CHECK(ret);
 
     // ========== 2. 初始化芯片ID ==========
     init_chip_id();
 
     // ========== 3. 按键初始化（最先） ==========
     button_init();
 
     // ========== 4. SD卡初始化（在I2S之前） ==========
     mount_sd();
 
     // 测试SD卡
     FILE* test_file = fopen("/sdcard/test.txt", "w");
     if (test_file) {
         fprintf(test_file, "SD card test");
         fclose(test_file);
         unlink("/sdcard/test.txt");
         ESP_LOGI(TAG, "✅ SD card write test: OK");
     } else {
         ESP_LOGE(TAG, "❌ SD card write test FAILED");
     }
 
     // ========== 5. 🔧 关键：I2C和ES7210初始化（在I2S之前） ==========
     codec_init();
     vTaskDelay(pdMS_TO_TICKS(100));  // 给ES7210时间稳定
     ESP_LOGI(TAG, "⏱️  ES7210 warmup complete");
 
     // ========== 6. 🔧 关键：I2S初始化（在ES7210之后） ==========
     g_i2s_rx_chan = i2s_init();
     if (g_i2s_rx_chan == NULL) {
         ESP_LOGE(TAG, "❌ I2S initialization failed, halting");
         while(1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
     }
 
     // ========== 7. 🔧 关键：启用I2S通道 ==========
     ESP_LOGI(TAG, "Enabling I2S channel...");
     ESP_ERROR_CHECK(i2s_channel_enable(g_i2s_rx_chan));
     ESP_LOGI(TAG, "✅ I2S channel enabled");
 
     // ========== 8. 🔧 关键：延长预热时间 ==========
     ESP_LOGI(TAG, "⏱️  I2S warmup (500ms)...");
     vTaskDelay(pdMS_TO_TICKS(500));  // 从100ms增加到500ms
     ESP_LOGI(TAG, "✅ I2S warmup complete");
 
     // 🔧 测试I2S读取
     ESP_LOGI(TAG, "🧪 Testing I2S read...");
     size_t test_br;
     esp_err_t test_ret = i2s_channel_read(g_i2s_rx_chan, i2s_read_buff, 
                                           I2S_READ_BUFFER_SIZE, &test_br, 
                                           pdMS_TO_TICKS(1000));
     if (test_ret == ESP_OK && test_br > 0) {
         ESP_LOGI(TAG, "✅ I2S test read: OK (%d bytes)", test_br);
     } else {
         ESP_LOGW(TAG, "⚠️  I2S test read: %s, bytes=%d", esp_err_to_name(test_ret), test_br);
     }
 
     // ========== 9. 创建同步原语 ==========
     upload_queue = xQueueCreate(10, sizeof(char) * 264);
     sd_mutex = xSemaphoreCreateMutex();
     system_events = xEventGroupCreate();
 
     if (!upload_queue || !sd_mutex || !system_events) {
         ESP_LOGE(TAG, "❌ Failed to create sync primitives");
         return;
     }
 
     // ========== 10. 扫描已有录音文件 ==========
     scan_existing_recordings();
 
     // ========== 11. 创建任务 ==========
     ESP_LOGI(TAG, "Creating tasks...");
 
     xTaskCreatePinnedToCore(upload_task, "upload_1", 8192, NULL, 3, NULL, 1);
     xTaskCreatePinnedToCore(upload_task, "upload_2", 8192, NULL, 3, NULL, 1);
     xTaskCreatePinnedToCore(wifi_manager_task, "wifi_mgr", 4096, NULL, 4, NULL, 1);
     xTaskCreatePinnedToCore(record_task, "recorder", 8192, NULL, 9, NULL, 0);
     xTaskCreatePinnedToCore(button_task, "button", 2048, NULL, 10, NULL, 0);
 
     ESP_LOGI(TAG, "");
     ESP_LOGI(TAG, "========================================");
     ESP_LOGI(TAG, "  ✅ System Ready!");
     ESP_LOGI(TAG, "  Press GPIO%d to record", BUTTON_GPIO);
     ESP_LOGI(TAG, "========================================");
     ESP_LOGI(TAG, "");
 
     vTaskDelete(NULL);
 }