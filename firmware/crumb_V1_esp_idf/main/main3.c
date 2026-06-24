/*
 * Recorder + WiFi uploader for ESP32 + 2x ICS-41350 + W25N01GV.
 *
 * Behavior:
 *   - Erase W25N on boot.
 *   - Press and hold KEY_PIN low to record.
 *   - Release the button to stop and save to W25N as raw PCM + metadata.
 *   - Then upload the latest recording to the server over WiFi as a WAV stream.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/i2s_pdm.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#define TAG "RECORDER_WIFI"

#define WIFI_SSID               "blankmeng"
#define WIFI_PASS               "chen106416336"
// #define SERVER_URL              "http://121.41.97.112:8080/upload"
#define SERVER_URL              "https://crumb.xiaomeng-research.xyz/upload"

#define KEY_PIN                 4
#define KEY_ACTIVE_LEVEL        0

#define SPI_MISO_PIN            19
#define SPI_MOSI_PIN            23
#define SPI_CLK_PIN             18
#define SPI_CS_PIN              5
#define SPI_HOST                SPI2_HOST
#define SPI_FREQ_HZ             (10 * 1000 * 1000)

#define PDM_DATA_PIN            25
#define PDM_CLK_PIN             26
#define PDM_SAMPLE_RATE         16000
#define PDM_BITS_PER_SAMPLE     16
#define PDM_CHANNELS            2
#define PDM_READ_BYTES          2048

#define BUTTON_DEBOUNCE_MS      30
#define BUTTON_POLL_MS          10
#define I2S_READ_TIMEOUT_MS     200
#define PROGRESS_LOG_MS         1000
#define RECORDER_TASK_STACK     10240

#define W25N_PAGE_SIZE          2048U
#define HTTP_WRITE_CHUNK_SIZE   8192U
#define W25N_PAGES_PER_BLOCK    64U
#define W25N_BLOCK_COUNT        1024U
#define W25N_METADATA_BLOCK     (W25N_BLOCK_COUNT - 1U)
#define W25N_AUDIO_BLOCK_COUNT  (W25N_BLOCK_COUNT - 1U)
#define W25N_AUDIO_PAGE_LIMIT   (W25N_AUDIO_BLOCK_COUNT * W25N_PAGES_PER_BLOCK)
#define W25N_AUDIO_CAPACITY     ((uint32_t)(W25N_AUDIO_PAGE_LIMIT * W25N_PAGE_SIZE))

#define W25N_CMD_WRITE_ENABLE   0x06
#define W25N_CMD_JEDEC_ID       0x9F
#define W25N_CMD_GET_FEATURE    0x0F
#define W25N_CMD_SET_FEATURE    0x1F
#define W25N_CMD_PAGE_READ      0x13
#define W25N_CMD_READ_CACHE     0x03
#define W25N_CMD_PROGRAM_LOAD   0x02
#define W25N_CMD_PROGRAM_EXEC   0x10
#define W25N_CMD_BLOCK_ERASE    0xD8
#define W25N_CMD_RESET          0xFF

#define W25N_REG_PROTECTION     0xA0
#define W25N_REG_CONFIGURATION  0xB0
#define W25N_REG_STATUS         0xC0

#define W25N_STATUS_BUSY        BIT(0)
#define W25N_STATUS_WEL         BIT(1)
#define W25N_STATUS_ERASE_FAIL  BIT(2)
#define W25N_STATUS_PROGRAM_FAIL BIT(3)

#define W25N_EXPECTED_MFR       0xEF
#define W25N_EXPECTED_DEV       0xAA21

#define RECORDING_META_MAGIC    0x52454331UL
#define RECORDING_META_VERSION  1U
#define RECORDING_FLAG_UPLOADED BIT(0)

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t session_id;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t bits_per_sample;
    uint32_t pcm_bytes;
    uint32_t duration_ms;
    uint32_t pages_used;
    uint32_t flags;
    uint32_t start_unix_time;
    uint32_t reserved[5];
} recording_metadata_t;

typedef struct {
    recording_metadata_t meta;
    uint8_t page_buf[W25N_PAGE_SIZE];
    size_t page_fill;
    uint32_t next_page;
    volatile bool storage_full;
} recording_session_t;

typedef struct __attribute__((packed)) {
    char riff[4];
    uint32_t chunk_size;
    char wave[4];
    char fmt[4];
    uint32_t fmt_size;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data[4];
    uint32_t data_size;
} wav_header_t;

#define PCM_QUEUE_DEPTH     16   // ~512 ms of slack so storage stalls don't drop frames

typedef struct {
    uint8_t data[PDM_READ_BYTES];
    size_t  len;
} pcm_chunk_t;

static spi_device_handle_t s_flash = NULL;
static i2s_chan_handle_t s_rx_chan = NULL;
static volatile uint32_t s_i2s_ovf_count = 0;   // DMA RX queue overflows (silent audio loss)
static uint8_t s_capture_buf[PDM_READ_BYTES];  // fallback drain buffer
static uint32_t s_next_session_id = 1;
static volatile bool s_wifi_connected = false;
static bool s_clock_is_trusted = false;
static recording_session_t s_recording_session;
static uint8_t s_meta_page[W25N_PAGE_SIZE];
static uint8_t s_upload_page[W25N_PAGE_SIZE];

static pcm_chunk_t        s_pcm_pool[PCM_QUEUE_DEPTH];
static QueueHandle_t      s_pcm_data_q       = NULL;
static QueueHandle_t      s_pcm_free_q       = NULL;
static SemaphoreHandle_t  s_storage_done_sem  = NULL;
static volatile esp_err_t s_storage_result    = ESP_OK;

static bool key_is_pressed(void)
{
    return gpio_get_level(KEY_PIN) == KEY_ACTIVE_LEVEL;
}

static void key_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << KEY_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
}

// Called from the I2S ISR when the RX DMA queue overflows: the recorder task
// didn't pull data fast enough, so hardware silently discarded samples. Counting
// these makes that otherwise-invisible audio loss measurable.
static IRAM_ATTR bool i2s_rx_ovf_cb(i2s_chan_handle_t handle,
                                    i2s_event_data_t *event,
                                    void *user_ctx)
{
    s_i2s_ovf_count++;
    return false;
}

static esp_err_t pdm_mic_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 512;   // 8 x 512 frames x 4 B = 16 KB (~256 ms) of DMA buffering

    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &s_rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_event_callbacks_t cbs = {
        .on_recv_q_ovf = i2s_rx_ovf_cb,
    };
    ret = i2s_channel_register_event_callback(s_rx_chan, &cbs, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_register_event_callback failed: %s", esp_err_to_name(ret));
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
        return ret;
    }

    i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(PDM_SAMPLE_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                    I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .clk = PDM_CLK_PIN,
            .din = PDM_DATA_PIN,
            .invert_flags = {
                .clk_inv = false,
            },
        },
    };

    ret = i2s_channel_init_pdm_rx_mode(s_rx_chan, &pdm_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_pdm_rx_mode failed: %s", esp_err_to_name(ret));
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "PDM microphones ready: %d Hz, stereo, %d-bit", PDM_SAMPLE_RATE, PDM_BITS_PER_SAMPLE);
    ESP_LOGI(TAG, "MIC_L = SELECT->GND, MIC_R = SELECT->3.3V");
    return ESP_OK;
}

static esp_err_t w25n_txrx(const uint8_t *tx, uint8_t *rx, size_t len)
{
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    return spi_device_transmit(s_flash, &t);
}

static esp_err_t w25n_tx(const uint8_t *tx, size_t len)
{
    return w25n_txrx(tx, NULL, len);
}

static esp_err_t w25n_get_feature(uint8_t reg, uint8_t *value)
{
    uint8_t tx[3] = {W25N_CMD_GET_FEATURE, reg, 0x00};
    uint8_t rx[3] = {0};
    esp_err_t ret = w25n_txrx(tx, rx, sizeof(tx));
    if (ret == ESP_OK) {
        *value = rx[2];
    }
    return ret;
}

static esp_err_t w25n_write_enable(void)
{
    uint8_t cmd = W25N_CMD_WRITE_ENABLE;
    esp_err_t ret = w25n_tx(&cmd, 1);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t status = 0;
    ret = w25n_get_feature(W25N_REG_STATUS, &status);
    if (ret != ESP_OK) {
        return ret;
    }
    return (status & W25N_STATUS_WEL) ? ESP_OK : ESP_FAIL;
}

static esp_err_t w25n_set_feature(uint8_t reg, uint8_t value)
{
    esp_err_t ret = w25n_write_enable();
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t tx[3] = {W25N_CMD_SET_FEATURE, reg, value};
    return w25n_tx(tx, sizeof(tx));
}

static esp_err_t w25n_wait_ready(uint32_t timeout_ms)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (xTaskGetTickCount() <= deadline) {
        uint8_t status = 0;
        esp_err_t ret = w25n_get_feature(W25N_REG_STATUS, &status);
        if (ret != ESP_OK) {
            return ret;
        }
        if ((status & W25N_STATUS_BUSY) == 0) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    return ESP_ERR_TIMEOUT;
}

static esp_err_t w25n_reset(void)
{
    uint8_t cmd = W25N_CMD_RESET;
    esp_err_t ret = w25n_tx(&cmd, 1);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
    return w25n_wait_ready(50);
}

static esp_err_t w25n_read_jedec(uint8_t *mfr, uint16_t *dev_id)
{
    uint8_t tx[5] = {W25N_CMD_JEDEC_ID, 0x00, 0x00, 0x00, 0x00};
    uint8_t rx[5] = {0};
    esp_err_t ret = w25n_txrx(tx, rx, sizeof(tx));
    if (ret != ESP_OK) {
        return ret;
    }

    *mfr = rx[2];
    *dev_id = ((uint16_t)rx[3] << 8) | rx[4];
    return ESP_OK;
}

static esp_err_t w25n_erase_block(uint16_t block)
{
    if (block >= W25N_BLOCK_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t row = block * W25N_PAGES_PER_BLOCK;
    esp_err_t ret = w25n_write_enable();
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t tx[4] = {
        W25N_CMD_BLOCK_ERASE,
        (uint8_t)(row >> 16),
        (uint8_t)(row >> 8),
        (uint8_t)row,
    };
    ret = w25n_tx(tx, sizeof(tx));
    if (ret != ESP_OK) {
        return ret;
    }

    ret = w25n_wait_ready(100);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t status = 0;
    ret = w25n_get_feature(W25N_REG_STATUS, &status);
    if (ret != ESP_OK) {
        return ret;
    }

    return (status & W25N_STATUS_ERASE_FAIL) ? ESP_FAIL : ESP_OK;
}

static esp_err_t w25n_program_page(uint32_t page, const uint8_t *data, size_t len)
{
    if (page >= (W25N_BLOCK_COUNT * W25N_PAGES_PER_BLOCK) || len > W25N_PAGE_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = w25n_write_enable();
    if (ret != ESP_OK) {
        return ret;
    }

    // Static TX scratch: avoids a malloc/free per page on the storage hot path.
    // Safe because page programming is never concurrent — storage_task programs
    // audio pages, and metadata is written only after storage_task has finished.
    static uint8_t prog_tx[3 + W25N_PAGE_SIZE];
    size_t tx_len = 3 + len;

    prog_tx[0] = W25N_CMD_PROGRAM_LOAD;
    prog_tx[1] = 0x00;
    prog_tx[2] = 0x00;
    memcpy(prog_tx + 3, data, len);

    ret = w25n_tx(prog_tx, tx_len);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t exec[4] = {
        W25N_CMD_PROGRAM_EXEC,
        (uint8_t)(page >> 16),
        (uint8_t)(page >> 8),
        (uint8_t)page,
    };
    ret = w25n_tx(exec, sizeof(exec));
    if (ret != ESP_OK) {
        return ret;
    }

    ret = w25n_wait_ready(50);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t status = 0;
    ret = w25n_get_feature(W25N_REG_STATUS, &status);
    if (ret != ESP_OK) {
        return ret;
    }

    return (status & W25N_STATUS_PROGRAM_FAIL) ? ESP_FAIL : ESP_OK;
}

static esp_err_t w25n_read_page(uint32_t page, uint16_t column, uint8_t *data, size_t len)
{
    if (page >= (W25N_BLOCK_COUNT * W25N_PAGES_PER_BLOCK) || column + len > W25N_PAGE_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t load[4] = {
        W25N_CMD_PAGE_READ,
        (uint8_t)(page >> 16),
        (uint8_t)(page >> 8),
        (uint8_t)page,
    };
    esp_err_t ret = w25n_tx(load, sizeof(load));
    if (ret != ESP_OK) {
        return ret;
    }

    ret = w25n_wait_ready(50);
    if (ret != ESP_OK) {
        return ret;
    }

    size_t tx_len = 4 + len;
    uint8_t *tx = calloc(1, tx_len);
    uint8_t *rx = calloc(1, tx_len);
    if (tx == NULL || rx == NULL) {
        free(tx);
        free(rx);
        return ESP_ERR_NO_MEM;
    }

    tx[0] = W25N_CMD_READ_CACHE;
    tx[1] = (uint8_t)(column >> 8);
    tx[2] = (uint8_t)column;
    tx[3] = 0x00;

    ret = w25n_txrx(tx, rx, tx_len);
    if (ret == ESP_OK) {
        memcpy(data, rx + 4, len);
    }

    free(tx);
    free(rx);
    return ret;
}

static esp_err_t w25n_init(void)
{
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SPI_MOSI_PIN,
        .miso_io_num = SPI_MISO_PIN,
        .sclk_io_num = SPI_CLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };

    esp_err_t ret = spi_bus_initialize(SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return ret;
    }

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = SPI_FREQ_HZ,
        .mode = 0,
        .spics_io_num = SPI_CS_PIN,
        .queue_size = 1,
    };

    ret = spi_bus_add_device(SPI_HOST, &dev_cfg, &s_flash);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(ret));
        spi_bus_free(SPI_HOST);
        return ret;
    }

    ret = w25n_reset();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "W25N reset failed: %s", esp_err_to_name(ret));
        return ret;
    }

    uint8_t mfr = 0;
    uint16_t dev_id = 0;
    ret = w25n_read_jedec(&mfr, &dev_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "W25N JEDEC read failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "W25N JEDEC: manufacturer=0x%02X device=0x%04X", mfr, dev_id);
    if (mfr != W25N_EXPECTED_MFR || dev_id != W25N_EXPECTED_DEV) {
        ESP_LOGE(TAG, "Unexpected W25N JEDEC ID");
        return ESP_FAIL;
    }

    ret = w25n_set_feature(W25N_REG_PROTECTION, 0x00);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to clear W25N protection");
        return ret;
    }

    uint8_t protection = 0;
    uint8_t config = 0;
    ESP_ERROR_CHECK(w25n_get_feature(W25N_REG_PROTECTION, &protection));
    ESP_ERROR_CHECK(w25n_get_feature(W25N_REG_CONFIGURATION, &config));

    ESP_LOGI(TAG, "W25N ready: protection=0x%02X config=0x%02X", protection, config);
    ESP_LOGI(TAG, "Audio capacity: %" PRIu32 " bytes (%.1f s at %d Hz stereo)",
             W25N_AUDIO_CAPACITY,
             (double)W25N_AUDIO_CAPACITY /
                 (PDM_SAMPLE_RATE * PDM_CHANNELS * (PDM_BITS_PER_SAMPLE / 8)),
             PDM_SAMPLE_RATE);
    return ESP_OK;
}

static esp_err_t save_latest_metadata(const recording_metadata_t *meta)
{
    memset(s_meta_page, 0xFF, sizeof(s_meta_page));
    memcpy(s_meta_page, meta, sizeof(*meta));

    esp_err_t ret = w25n_erase_block(W25N_METADATA_BLOCK);
    if (ret != ESP_OK) {
        return ret;
    }

    return w25n_program_page(W25N_METADATA_BLOCK * W25N_PAGES_PER_BLOCK, s_meta_page, sizeof(s_meta_page));
}

static esp_err_t load_latest_metadata(recording_metadata_t *meta)
{
    esp_err_t ret = w25n_read_page(W25N_METADATA_BLOCK * W25N_PAGES_PER_BLOCK,
                                   0,
                                   s_meta_page,
                                   sizeof(s_meta_page));
    if (ret != ESP_OK) {
        return ret;
    }

    memcpy(meta, s_meta_page, sizeof(*meta));
    if (meta->magic != RECORDING_META_MAGIC || meta->version != RECORDING_META_VERSION) {
        return ESP_ERR_NOT_FOUND;
    }

    if (meta->pcm_bytes == 0 || meta->pages_used == 0 || meta->pages_used > W25N_AUDIO_PAGE_LIMIT) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

static void log_saved_recording(const recording_metadata_t *meta, const char *prefix)
{
    double seconds = (double)meta->duration_ms / 1000.0;
    uint32_t wav_bytes = meta->pcm_bytes + 44U;

    ESP_LOGI(TAG,
             "%s session=%" PRIu32 ", duration=%.2f s, pcm=%" PRIu32 " bytes, wav=%" PRIu32
             " bytes, pages=%" PRIu32 ", uploaded=%s",
             prefix,
             meta->session_id,
             seconds,
             meta->pcm_bytes,
             wav_bytes,
             meta->pages_used,
             (meta->flags & RECORDING_FLAG_UPLOADED) ? "yes" : "no");
}

static esp_err_t w25n_clear_recording_area(void)
{
    ESP_LOGI(TAG, "Erasing W25N recording area...");
    for (uint16_t block = 0; block < W25N_BLOCK_COUNT; block++) {
        esp_err_t ret = w25n_erase_block(block);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Erase failed at block %" PRIu16 ": %s", block, esp_err_to_name(ret));
            return ret;
        }

        if ((block % 64U) == 0U || block == (W25N_BLOCK_COUNT - 1U)) {
            ESP_LOGI(TAG, "W25N erase progress: %" PRIu16 "/%" PRIu32 " blocks",
                     block + 1U, W25N_BLOCK_COUNT);
        }
    }

    s_next_session_id = 1;
    ESP_LOGI(TAG, "W25N erase complete");
    return ESP_OK;
}

static esp_err_t recording_flush_page(recording_session_t *session)
{
    if (session->page_fill == 0) {
        return ESP_OK;
    }

    if (session->next_page >= W25N_AUDIO_PAGE_LIMIT) {
        session->storage_full = true;
        return ESP_ERR_NO_MEM;
    }

    if ((session->next_page % W25N_PAGES_PER_BLOCK) == 0) {
        uint16_t block = session->next_page / W25N_PAGES_PER_BLOCK;
        esp_err_t ret = w25n_erase_block(block);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    memset(session->page_buf + session->page_fill, 0, W25N_PAGE_SIZE - session->page_fill);

    esp_err_t ret = w25n_program_page(session->next_page, session->page_buf, W25N_PAGE_SIZE);
    if (ret != ESP_OK) {
        return ret;
    }

    session->next_page++;
    session->meta.pages_used = session->next_page;
    session->page_fill = 0;
    return ESP_OK;
}

static esp_err_t recording_append(recording_session_t *session, const uint8_t *data, size_t len)
{
    while (len > 0) {
        size_t copy_len = W25N_PAGE_SIZE - session->page_fill;
        if (copy_len > len) {
            copy_len = len;
        }

        memcpy(session->page_buf + session->page_fill, data, copy_len);
        session->page_fill += copy_len;
        data += copy_len;
        len -= copy_len;

        if (session->page_fill == W25N_PAGE_SIZE) {
            esp_err_t ret = recording_flush_page(session);
            if (ret != ESP_OK) {
                return ret;
            }
        }
    }

    return ESP_OK;
}

static void storage_task(void *arg)
{
    while (true) {
        pcm_chunk_t *chunk = NULL;
        xQueueReceive(s_pcm_data_q, &chunk, portMAX_DELAY);

        if (chunk == NULL) {
            if (s_storage_result == ESP_OK) {
                s_storage_result = recording_flush_page(&s_recording_session);
            }
            xSemaphoreGive(s_storage_done_sem);
            continue;
        }

        if (s_storage_result == ESP_OK) {
            s_storage_result = recording_append(&s_recording_session, chunk->data, chunk->len);
            if (s_storage_result == ESP_OK) {
                s_recording_session.meta.pcm_bytes += (uint32_t)chunk->len;
            }
        }

        xQueueSend(s_pcm_free_q, &chunk, portMAX_DELAY);
    }
}

static esp_err_t record_one_session(recording_metadata_t *saved_meta)
{
    recording_session_t *session = &s_recording_session;
    memset(session, 0, sizeof(*session));
    session->meta = (recording_metadata_t){
        .magic = RECORDING_META_MAGIC,
        .version = RECORDING_META_VERSION,
        .session_id = s_next_session_id++,
        .sample_rate = PDM_SAMPLE_RATE,
        .channels = PDM_CHANNELS,
        .bits_per_sample = PDM_BITS_PER_SAMPLE,
        .pcm_bytes = 0,
        .duration_ms = 0,
        .pages_used = 0,
        .flags = 0,
        .start_unix_time = 0,
    };

    time_t now = 0;
    time(&now);
    session->meta.start_unix_time = (uint32_t)now;

    s_storage_result = ESP_OK;

    // Drain leftover free-queue entries from a previous session, then repopulate.
    pcm_chunk_t *tmp;
    while (xQueueReceive(s_pcm_free_q, &tmp, 0) == pdTRUE) {}
    for (int i = 0; i < PCM_QUEUE_DEPTH; i++) {
        pcm_chunk_t *p = &s_pcm_pool[i];
        xQueueSend(s_pcm_free_q, &p, 0);
    }

    s_i2s_ovf_count = 0;
    uint32_t dropped_frames = 0;

    esp_err_t ret = i2s_channel_enable(s_rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    TickType_t start_tick = xTaskGetTickCount();
    TickType_t last_progress_tick = start_tick;

    ESP_LOGI(TAG, "Recording start: session=%" PRIu32, session->meta.session_id);
    ESP_LOGI(TAG, "Hold KEY on GPIO%d to keep recording", KEY_PIN);

    while (key_is_pressed() && !session->storage_full) {
        pcm_chunk_t *chunk = NULL;
        if (xQueueReceive(s_pcm_free_q, &chunk, pdMS_TO_TICKS(50)) != pdTRUE) {
            // Pool exhausted: drain I2S into scratch buffer to keep DMA clear.
            size_t dummy = 0;
            i2s_channel_read(s_rx_chan, s_capture_buf, sizeof(s_capture_buf),
                             &dummy, pdMS_TO_TICKS(I2S_READ_TIMEOUT_MS));
            dropped_frames++;
            ESP_LOGW(TAG, "PCM pool exhausted, frame dropped (total=%" PRIu32 ")", dropped_frames);
            continue;
        }

        size_t bytes_read = 0;
        ret = i2s_channel_read(s_rx_chan, chunk->data, PDM_READ_BYTES, &bytes_read,
                               pdMS_TO_TICKS(I2S_READ_TIMEOUT_MS));

        if (ret == ESP_OK && bytes_read > 0) {
            TickType_t now_tick = xTaskGetTickCount();
            if ((now_tick - last_progress_tick) >= pdMS_TO_TICKS(PROGRESS_LOG_MS)) {
                double seconds = (double)session->meta.pcm_bytes /
                                 (PDM_SAMPLE_RATE * PDM_CHANNELS * (PDM_BITS_PER_SAMPLE / 8));
                ESP_LOGI(TAG, "Recording... %.2f s, %" PRIu32 " bytes", seconds,
                         session->meta.pcm_bytes);

                /* DEBUG: 打印左右声道峰值，确认左声道是否有信号 */
                const int16_t *samples = (const int16_t *)chunk->data;
                size_t n_samples = bytes_read / sizeof(int16_t);
                int32_t peak_l = 0, peak_r = 0;
                for (size_t i = 0; i + 1 < n_samples; i += 2) {
                    int32_t l = samples[i] < 0 ? -samples[i] : samples[i];
                    int32_t r = samples[i + 1] < 0 ? -samples[i + 1] : samples[i + 1];
                    if (l > peak_l) peak_l = l;
                    if (r > peak_r) peak_r = r;
                }
                ESP_LOGI(TAG, "DEBUG ch_peak  L=%" PRId32 "  R=%" PRId32, peak_l, peak_r);
                /* END DEBUG */

                last_progress_tick = now_tick;
            }

            chunk->len = bytes_read;
            xQueueSend(s_pcm_data_q, &chunk, portMAX_DELAY);
        } else {
            if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "i2s_channel_read failed: %s", esp_err_to_name(ret));
            }
            xQueueSend(s_pcm_free_q, &chunk, 0);
        }
    }

    i2s_channel_disable(s_rx_chan);

    // Send NULL sentinel so storage_task flushes and signals done.
    pcm_chunk_t *sentinel = NULL;
    xQueueSend(s_pcm_data_q, &sentinel, portMAX_DELAY);
    xSemaphoreTake(s_storage_done_sem, portMAX_DELAY);

    esp_err_t storage_err = s_storage_result;
    if (storage_err != ESP_OK && storage_err != ESP_ERR_NO_MEM) {
        ESP_LOGE(TAG, "Storage task error: %s", esp_err_to_name(storage_err));
        return storage_err;
    }

    if (session->meta.pcm_bytes == 0) {
        ESP_LOGW(TAG, "Recording ignored: no audio captured");
        return ESP_ERR_INVALID_SIZE;
    }

    TickType_t elapsed_ticks = xTaskGetTickCount() - start_tick;
    session->meta.duration_ms = (uint32_t)(elapsed_ticks * portTICK_PERIOD_MS);

    ret = save_latest_metadata(&session->meta);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save recording metadata: %s", esp_err_to_name(ret));
        return ret;
    }

    uint32_t ovf = s_i2s_ovf_count;
    if (dropped_frames != 0 || ovf != 0) {
        ESP_LOGW(TAG,
                 "Audio loss this session: pool-drops=%" PRIu32 " (~%" PRIu32 " B), "
                 "DMA-overflows=%" PRIu32 " -- captured WAV will be shorter than hold time",
                 dropped_frames, dropped_frames * (uint32_t)PDM_READ_BYTES, ovf);
    } else {
        ESP_LOGI(TAG, "Audio loss this session: none (no drops, no DMA overflow)");
    }

    *saved_meta = session->meta;
    log_saved_recording(saved_meta, "Recording saved:");
    if (session->storage_full) {
        ESP_LOGW(TAG, "Recording stopped because W25N storage is full");
    }
    return ESP_OK;
}

static void sync_time_sntp(void)
{
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    setenv("TZ", "CST-8", 1);
    tzset();

    if (esp_netif_sntp_init(&config) != ESP_OK) {
        ESP_LOGE(TAG, "SNTP init failed");
        return;
    }

    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) == ESP_OK) {
        time_t now = 0;
        struct tm t = {0};
        char buf[32];
        time(&now);
        localtime_r(&now, &t);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
        ESP_LOGI(TAG, "SNTP sync OK: %s CST", buf);
        s_clock_is_trusted = true;
    } else {
        ESP_LOGW(TAG, "SNTP sync timed out; filenames will use session ID");
    }

    esp_netif_sntp_deinit();
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        ESP_LOGW(TAG, "WiFi disconnected, retrying...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_wifi_connected = true;
        ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static esp_err_t wifi_init_sta(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA3_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
            .pmf_cfg = {
                .capable = true,
                .required = true,
            },
        },
    };
    strncpy((char *)wifi_cfg.sta.ssid, WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    strncpy((char *)wifi_cfg.sta.password, WIFI_PASS, sizeof(wifi_cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi init done, connecting to %s", WIFI_SSID);
    return ESP_OK;
}

static wav_header_t make_wav_header(const recording_metadata_t *meta)
{
    wav_header_t header = {
        .riff = {'R', 'I', 'F', 'F'},
        .chunk_size = 36U + meta->pcm_bytes,
        .wave = {'W', 'A', 'V', 'E'},
        .fmt = {'f', 'm', 't', ' '},
        .fmt_size = 16,
        .audio_format = 1,
        .num_channels = (uint16_t)meta->channels,
        .sample_rate = meta->sample_rate,
        .byte_rate = meta->sample_rate * meta->channels * (meta->bits_per_sample / 8U),
        .block_align = (uint16_t)(meta->channels * (meta->bits_per_sample / 8U)),
        .bits_per_sample = (uint16_t)meta->bits_per_sample,
        .data = {'d', 'a', 't', 'a'},
        .data_size = meta->pcm_bytes,
    };
    return header;
}

static void build_recording_name(const recording_metadata_t *meta, char *buf, size_t len)
{
    if (meta->start_unix_time != 0 && s_clock_is_trusted) {
        time_t t = (time_t)meta->start_unix_time;
        struct tm tm_info = {0};
        localtime_r(&t, &tm_info);
        snprintf(buf, len, "REC_%04d%02d%02d_%02d%02d%02d_%lus.wav",
                 tm_info.tm_year + 1900,
                 tm_info.tm_mon + 1,
                 tm_info.tm_mday,
                 tm_info.tm_hour,
                 tm_info.tm_min,
                 tm_info.tm_sec,
                 (unsigned long)(meta->duration_ms / 1000U));
    } else {
        snprintf(buf, len, "REC_%06" PRIu32 "_%lus.wav",
                 meta->session_id,
                 (unsigned long)(meta->duration_ms / 1000U));
    }
}

static esp_err_t upload_latest_recording(recording_metadata_t *meta)
{
    if (!s_wifi_connected) {
        ESP_LOGW(TAG, "Upload skipped: WiFi not connected");
        return ESP_ERR_INVALID_STATE;
    }

    if ((meta->flags & RECORDING_FLAG_UPLOADED) != 0) {
        ESP_LOGI(TAG, "Latest recording already uploaded, skipping");
        return ESP_OK;
    }

    wav_header_t header = make_wav_header(meta);
    char filename[64];
    build_recording_name(meta, filename, sizeof(filename));

    char url[320];
    snprintf(url, sizeof(url), "%s?filename=%s", SERVER_URL, filename);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .buffer_size_tx = 16384,
        .keep_alive_enable = true,
        .keep_alive_interval = 5000,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        // NOTE: server cert verification is disabled (no CA bundle attached).
        // The cert bundle lacks Cloudflare's current root CA, so verification
        // fails with -0x3000. Traffic is still TLS-encrypted, just unverified.
        // For proper security later, embed the root CA and use .cert_pem instead.
        .skip_cert_common_name_check = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "audio/wav");
    esp_http_client_set_header(client, "X-Session-Id", filename);

    uint32_t total_bytes = sizeof(header) + meta->pcm_bytes;
    esp_err_t ret = esp_http_client_open(client, total_bytes);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(ret));
        esp_http_client_cleanup(client);
        return ret;
    }

    int written = esp_http_client_write(client, (const char *)&header, sizeof(header));
    if (written != sizeof(header)) {
        ESP_LOGE(TAG, "Failed to send WAV header");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    uint32_t remaining = meta->pcm_bytes;
    uint32_t sent_pcm = 0;
    TickType_t last_progress = xTaskGetTickCount();

    for (uint32_t page = 0; page < meta->pages_used && remaining > 0; page++) {
        ret = w25n_read_page(page, 0, s_upload_page, W25N_PAGE_SIZE);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read page %" PRIu32 " from W25N: %s", page, esp_err_to_name(ret));
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ret;
        }

        size_t to_send = remaining > W25N_PAGE_SIZE ? W25N_PAGE_SIZE : remaining;
        size_t offset = 0;
        while (offset < to_send) {
            size_t chunk = (to_send - offset) > HTTP_WRITE_CHUNK_SIZE ? HTTP_WRITE_CHUNK_SIZE : (to_send - offset);
            written = esp_http_client_write(client, (const char *)(s_upload_page + offset), chunk);
            if (written != (int)chunk) {
                ESP_LOGE(TAG, "HTTP write failed at page %" PRIu32 " offset %zu", page, offset);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                return ESP_FAIL;
            }
            offset += chunk;
            sent_pcm += chunk;
            remaining -= chunk;
        }

        if ((xTaskGetTickCount() - last_progress) >= pdMS_TO_TICKS(PROGRESS_LOG_MS)) {
            ESP_LOGI(TAG, "Upload progress: %" PRIu32 "/%" PRIu32 " bytes", sent_pcm, meta->pcm_bytes);
            last_progress = xTaskGetTickCount();
        }
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Upload response: HTTP %d", status);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status < 200 || status >= 300) {
        return ESP_FAIL;
    }

    meta->flags |= RECORDING_FLAG_UPLOADED;
    ret = save_latest_metadata(meta);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Upload succeeded but metadata update failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Upload complete: %s", filename);
    return ESP_OK;
}

static void recorder_task(void *arg)
{
    recording_metadata_t meta = {0};
    esp_err_t ret = w25n_clear_recording_area();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase W25N before recording: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        if (!key_is_pressed()) {
            vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
        if (!key_is_pressed()) {
            continue;
        }

        ret = record_one_session(&meta);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Recording session failed: %s", esp_err_to_name(ret));
            continue;
        }

        while (!s_wifi_connected) {
            ESP_LOGI(TAG, "Waiting for WiFi before upload...");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        if (!s_clock_is_trusted) {
            sync_time_sntp();
            if (s_clock_is_trusted) {
                time_t now;
                time(&now);
                meta.start_unix_time = (uint32_t)(now - (time_t)(meta.duration_ms / 1000U));
            }
        }

        ret = upload_latest_recording(&meta);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Upload not completed: %s", esp_err_to_name(ret));
        }

        while (key_is_pressed()) {
            vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
        ESP_LOGI(TAG, "Ready for next recording");
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, "  ESP32 W25N Recorder + WiFi Upload");
    ESP_LOGI(TAG, "====================================");

    s_pcm_data_q      = xQueueCreate(PCM_QUEUE_DEPTH, sizeof(pcm_chunk_t *));
    s_pcm_free_q      = xQueueCreate(PCM_QUEUE_DEPTH, sizeof(pcm_chunk_t *));
    s_storage_done_sem = xSemaphoreCreateBinary();
    configASSERT(s_pcm_data_q && s_pcm_free_q && s_storage_done_sem);

    key_init();
    ESP_ERROR_CHECK(pdm_mic_init());
    ESP_ERROR_CHECK(w25n_init());
    ESP_ERROR_CHECK(wifi_init_sta());
    // storage_task runs at higher priority so it drains the queue promptly.
    xTaskCreate(storage_task, "storage_task", 4096, NULL, 6, NULL);
    xTaskCreate(recorder_task, "recorder_wifi_task", RECORDER_TASK_STACK, NULL, 5, NULL);
}
