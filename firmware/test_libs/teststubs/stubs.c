/* Host-side stub implementations of ESP-IDF services used by the components
 * under test (dcc, settings). All functions are no-ops or minimal in-memory
 * stand-ins. */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#include "esp_err.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "driver/adc.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/mcpwm.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/spi_master.h"
#include "esp_flash.h"
#include "esp_flash_spi_init.h"
#include "esp_partition.h"
#include "esp_littlefs.h"


int64_t mock_timer_now_us = 0;
int mock_gpio_set_level_count = 0;
int mock_gpio_get_level = 0;
int mock_vtask_delay_count = 0;

int64_t esp_timer_get_time(void)
{
    return mock_timer_now_us;
}

const char *esp_err_to_name(esp_err_t code)
{
    (void)code;
    return "err";
}

/* ---- GPIO ---- */
esp_err_t gpio_config(const gpio_config_t *cfg)
{
    (void)cfg;
    return ESP_OK;
}

int mock_gpio_isr_install_err = 0;

esp_err_t gpio_install_isr_service(int flags)
{
    (void)flags;
    return (esp_err_t)mock_gpio_isr_install_err;
}

esp_err_t gpio_isr_handler_add(gpio_num_t gpio, void (*isr)(void *), void *arg)
{
    (void)gpio;
    (void)isr;
    (void)arg;
    return ESP_OK;
}

esp_err_t gpio_isr_handler_remove(gpio_num_t gpio)
{
    (void)gpio;
    return ESP_OK;
}

esp_err_t gpio_set_level(gpio_num_t gpio, uint32_t level)
{
    (void)gpio;
    (void)level;
    mock_gpio_set_level_count++;
    return ESP_OK;
}

int gpio_get_level(gpio_num_t gpio)
{
    (void)gpio;
    return mock_gpio_get_level;
}

/* ---- FreeRTOS (minimal in-memory queue) ---- */
typedef struct {
    void *items[16];
    size_t head;
    size_t count;
    size_t item_size;
} mock_queue_t;

int mock_queue_create_fail = 0;
int mock_queue_send_fail = 0;

QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item_size)
{
    (void)len;
    if (mock_queue_create_fail) {
        return NULL;
    }
    mock_queue_t *q = (mock_queue_t *)calloc(1, sizeof(*q));
    if (q == NULL) {
        return NULL;
    }
    q->item_size = item_size;
    return q;
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks)
{
    (void)ticks;
    mock_queue_t *mq = (mock_queue_t *)q;
    if (mock_queue_send_fail || mq == NULL || mq->count >= 16) {
        return pdFALSE;
    }
    size_t idx = (mq->head + mq->count) % 16;
    mq->items[idx] = malloc(mq->item_size);
    if (mq->items[idx] == NULL) {
        return pdFALSE;
    }
    memcpy(mq->items[idx], item, mq->item_size);
    mq->count++;
    return pdTRUE;
}

BaseType_t xQueueSendFromISR(QueueHandle_t q, const void *item, BaseType_t *woken)
{
    if (woken != NULL) {
        *woken = pdTRUE;
    }
    if (mock_queue_send_fail) {
        return pdFALSE;
    }
    return xQueueSend(q, item, 0);
}

BaseType_t xQueueReceive(QueueHandle_t q, void *buf, TickType_t ticks)
{
    (void)ticks;
    mock_queue_t *mq = (mock_queue_t *)q;
    if (mq == NULL || mq->count == 0) {
        return pdFALSE;
    }
    memcpy(buf, mq->items[mq->head], mq->item_size);
    free(mq->items[mq->head]);
    mq->head = (mq->head + 1) % 16;
    mq->count--;
    return pdTRUE;
}

void vQueueDelete(QueueHandle_t q)
{
    mock_queue_t *mq = (mock_queue_t *)q;
    if (mq == NULL) {
        return;
    }
    while (mq->count > 0) {
        free(mq->items[mq->head]);
        mq->head = (mq->head + 1) % 16;
        mq->count--;
    }
    free(mq);
}

int mock_task_create_ok = 1;

BaseType_t xTaskCreate(void (*task)(void *), const char *name, uint32_t stack,
                       void *param, UBaseType_t prio, TaskHandle_t *handle)
{
    (void)task;
    (void)name;
    (void)stack;
    (void)param;
    (void)prio;
    if (handle != NULL) {
        *handle = NULL;
    }
    return mock_task_create_ok ? pdPASS : pdFAIL;
}

BaseType_t xTaskCreatePinnedToCore(void (*task)(void *), const char *name,
                                   uint32_t stack, void *param,
                                   UBaseType_t prio, TaskHandle_t *handle,
                                   BaseType_t core)
{
    (void)task;
    (void)name;
    (void)stack;
    (void)param;
    (void)prio;
    (void)core;
    if (handle != NULL) {
        *handle = NULL;
    }
    return mock_task_create_ok ? pdPASS : pdFAIL;
}

void vTaskDelay(const TickType_t ticks)
{
    (void)ticks;
    mock_vtask_delay_count++;
}

void vTaskDelayUntil(TickType_t *pxPreviousWakeTime, TickType_t xTimeIncrement)
{
    (void)pxPreviousWakeTime;
    (void)xTimeIncrement;
    mock_vtask_delay_count++;
}

void vTaskDelete(TaskHandle_t task)
{
    (void)task;
}

TickType_t xTaskGetTickCount(void)
{
    return 0;
}

uint32_t uxTaskGetStackHighWaterMark(TaskHandle_t task)
{
    (void)task;
    return 4096;
}

int mock_mutex_create_fail = 0;
int mock_sem_take_fail = 0;

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return mock_mutex_create_fail ? NULL : (SemaphoreHandle_t)1;
}

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    return (SemaphoreHandle_t)1;
}

void vSemaphoreDelete(SemaphoreHandle_t s)
{
    (void)s;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t)
{
    (void)s;
    (void)t;
    return mock_sem_take_fail ? pdFALSE : pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    (void)s;
    return pdTRUE;
}

/* ---- NVS (generic in-memory key/value store) ---- */
#define MOCK_NVS_MAX_KEYS 48
#define MOCK_NVS_MAX_BLOB 4096
#define MOCK_NVS_KEY_MAX  16

typedef enum {
    MOCK_NVS_U8 = 1,
    MOCK_NVS_U16,
    MOCK_NVS_U32,
    MOCK_NVS_STR,
    MOCK_NVS_BLOB,
} mock_nvs_type_t;

typedef struct {
    char key[MOCK_NVS_KEY_MAX];
    mock_nvs_type_t type;
    size_t len;
    uint8_t data[MOCK_NVS_MAX_BLOB];
} mock_nvs_entry_t;

static mock_nvs_entry_t s_nvs[MOCK_NVS_MAX_KEYS];
static int s_nvs_entries;

/* Wipe the in-memory NVS so each test starts from an empty store. */
void mock_nvs_reset(void)
{
    memset(s_nvs, 0, sizeof(s_nvs));
    s_nvs_entries = 0;
}

/* Force a raw blob under a key (for stale/corrupt-blob tests). */
esp_err_t mock_nvs_force_blob(const char *key, const void *value, size_t len);

static mock_nvs_entry_t *mock_nvs_find(const char *key)
{
    for (int i = 0; i < s_nvs_entries; ++i) {
        if (strcmp(s_nvs[i].key, key) == 0) {
            return &s_nvs[i];
        }
    }
    return NULL;
}

static mock_nvs_entry_t *mock_nvs_put(const char *key, mock_nvs_type_t type,
                                      const void *value, size_t len)
{
    if (len > MOCK_NVS_MAX_BLOB) {
        return NULL;
    }
    mock_nvs_entry_t *e = mock_nvs_find(key);
    if (e == NULL) {
        if (s_nvs_entries >= MOCK_NVS_MAX_KEYS) {
            return NULL;
        }
        e = &s_nvs[s_nvs_entries++];
        strncpy(e->key, key, sizeof(e->key) - 1);
        e->key[sizeof(e->key) - 1] = '\0';
    }
    e->type = type;
    e->len = len;
    if (value != NULL && len > 0) {
        memcpy(e->data, value, len);
    }
    return e;
}

static esp_err_t mock_nvs_get(const char *key, mock_nvs_type_t type,
                              void *out, size_t *len)
{
    mock_nvs_entry_t *e = mock_nvs_find(key);
    if (e == NULL) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (e->type != type) {
        return ESP_ERR_NVS_TYPE_MISMATCH;
    }
    if (len != NULL) {
        if (*len < e->len) {
            *len = e->len;
            return ESP_ERR_NVS_INVALID_LENGTH;
        }
        *len = e->len;
    }
    if (out != NULL && e->len > 0) {
        memcpy(out, e->data, e->len);
    }
    return ESP_OK;
}

esp_err_t mock_nvs_force_blob(const char *key, const void *value, size_t len)
{
    return mock_nvs_put(key, MOCK_NVS_BLOB, value, len) ? ESP_OK : ESP_ERR_NO_MEM;
}

int mock_nvs_flash_init_err = 0;

esp_err_t nvs_flash_init(void)
{
    return (esp_err_t)mock_nvs_flash_init_err;
}

esp_err_t nvs_flash_erase(void)
{
    mock_nvs_reset();
    mock_nvs_flash_init_err = 0; /* after erasing, init succeeds again */
    return ESP_OK;
}

int mock_nvs_open_fail = 0;

esp_err_t nvs_open(const char *name, int open_mode, nvs_handle_t *out_handle)
{
    (void)name;
    (void)open_mode;
    if (mock_nvs_open_fail) {
        return ESP_FAIL;
    }
    if (out_handle != NULL) {
        *out_handle = 1;
    }
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    (void)handle;
}

int mock_nvs_get_u8_err = 0;

esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out)
{
    (void)h;
    if (mock_nvs_get_u8_err) {
        return (esp_err_t)mock_nvs_get_u8_err;
    }
    size_t len = sizeof(*out);
    return mock_nvs_get(key, MOCK_NVS_U8, out, &len);
}

esp_err_t nvs_get_u16(nvs_handle_t h, const char *key, uint16_t *out)
{
    (void)h;
    size_t len = sizeof(*out);
    return mock_nvs_get(key, MOCK_NVS_U16, out, &len);
}

esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *out)
{
    (void)h;
    size_t len = sizeof(*out);
    return mock_nvs_get(key, MOCK_NVS_U32, out, &len);
}

esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len)
{
    (void)h;
    return mock_nvs_get(key, MOCK_NVS_STR, out, len);
}

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    (void)h;
    return mock_nvs_get(key, MOCK_NVS_BLOB, out, len);
}

esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t value)
{
    (void)h;
    return mock_nvs_put(key, MOCK_NVS_U8, &value, sizeof(value)) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t nvs_set_u16(nvs_handle_t h, const char *key, uint16_t value)
{
    (void)h;
    return mock_nvs_put(key, MOCK_NVS_U16, &value, sizeof(value)) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t value)
{
    (void)h;
    return mock_nvs_put(key, MOCK_NVS_U32, &value, sizeof(value)) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value)
{
    (void)h;
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return mock_nvs_put(key, MOCK_NVS_STR, value, strlen(value) + 1U) ? ESP_OK
                                                                      : ESP_ERR_NO_MEM;
}

esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *value, size_t len)
{
    (void)h;
    return mock_nvs_put(key, MOCK_NVS_BLOB, value, len) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t nvs_commit(nvs_handle_t h)
{
    (void)h;
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    (void)h;
    for (int i = 0; i < s_nvs_entries; ++i) {
        if (strcmp(s_nvs[i].key, key) == 0) {
            s_nvs[i] = s_nvs[s_nvs_entries - 1];
            memset(&s_nvs[s_nvs_entries - 1], 0, sizeof(s_nvs[0]));
            s_nvs_entries--;
            return ESP_OK;
        }
    }
    return ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_erase_all(nvs_handle_t h)
{
    (void)h;
    mock_nvs_reset();
    return ESP_OK;
}

/* ---- ADC1 ---- */
int mock_adc_raw[16];
int mock_adc_ok = 1;

int mock_adc1_config_width_ok = 1;
int mock_adc1_config_atten_ok = 1;

esp_err_t adc1_config_width(adc_bits_width_t width)
{
    (void)width;
    return mock_adc1_config_width_ok ? ESP_OK : ESP_FAIL;
}

esp_err_t adc1_config_channel_atten(adc1_channel_t channel, adc_atten_t atten)
{
    (void)channel;
    (void)atten;
    return mock_adc1_config_atten_ok ? ESP_OK : ESP_FAIL;
}

int adc1_get_raw(adc1_channel_t channel)
{
    if (!mock_adc_ok || channel < 0 || channel >= 16) {
        return -1;
    }
    return mock_adc_raw[channel];
}

/* ---- LEDC ---- */
uint32_t mock_ledc_duty[8];
int mock_ledc_update_count = 0;

esp_err_t ledc_timer_config(const ledc_timer_config_t *timer_conf)
{
    (void)timer_conf;
    return ESP_OK;
}

esp_err_t ledc_channel_config(const ledc_channel_config_t *ledc_conf)
{
    (void)ledc_conf;
    return ESP_OK;
}

esp_err_t ledc_set_duty(ledc_mode_t speed_mode, ledc_channel_t channel, uint32_t duty)
{
    (void)speed_mode;
    if (channel >= 0 && channel < 8) {
        mock_ledc_duty[channel] = duty;
    }
    return ESP_OK;
}

esp_err_t ledc_update_duty(ledc_mode_t speed_mode, ledc_channel_t channel)
{
    (void)speed_mode;
    (void)channel;
    mock_ledc_update_count++;
    return ESP_OK;
}

/* ---- MCPWM ---- */
float mock_mcpwm_duty[2][3][2];

esp_err_t mcpwm_gpio_init(mcpwm_unit_t unit, mcpwm_io_signals_t io_signal, gpio_num_t gpio_num)
{
    (void)unit;
    (void)io_signal;
    (void)gpio_num;
    return ESP_OK;
}

esp_err_t mcpwm_init(mcpwm_unit_t unit, mcpwm_timer_t timer, const mcpwm_config_t *conf)
{
    (void)unit;
    (void)timer;
    (void)conf;
    return ESP_OK;
}

esp_err_t mcpwm_set_duty(mcpwm_unit_t unit, mcpwm_timer_t timer,
                         mcpwm_operator_t op, float duty)
{
    if (unit >= 0 && unit < 2 && timer >= 0 && timer < 3 && op >= 0 && op < 2) {
        mock_mcpwm_duty[unit][timer][op] = duty;
    }
    return ESP_OK;
}

esp_err_t mcpwm_set_duty_type(mcpwm_unit_t unit, mcpwm_timer_t timer,
                              mcpwm_operator_t op, mcpwm_duty_type_t type)
{
    (void)unit;
    (void)timer;
    (void)op;
    (void)type;
    return ESP_OK;
}

/* ---- esp_rom_sys ---- */
uint32_t mock_rom_delay_us_total = 0;

void esp_rom_delay_us(uint32_t us)
{
    mock_rom_delay_us_total += us;
}

/* ---- I2S std ---- */
static int s_i2s_dummy_channel;

int mock_i2s_new_channel_err = 0;
int mock_i2s_init_std_err = 0;
int mock_i2s_write_count = 0;

esp_err_t i2s_new_channel(const i2s_chan_config_t *chan_cfg,
                          i2s_chan_handle_t *tx_handle, i2s_chan_handle_t *rx_handle)
{
    (void)chan_cfg;
    (void)rx_handle;
    if (mock_i2s_new_channel_err) {
        return (esp_err_t)mock_i2s_new_channel_err;
    }
    if (tx_handle != NULL) {
        *tx_handle = (i2s_chan_handle_t)&s_i2s_dummy_channel;
    }
    return ESP_OK;
}

esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t handle, const i2s_std_config_t *std_cfg)
{
    (void)handle;
    (void)std_cfg;
    return (esp_err_t)mock_i2s_init_std_err;
}

esp_err_t i2s_channel_enable(i2s_chan_handle_t handle)
{
    (void)handle;
    return ESP_OK;
}

esp_err_t i2s_channel_write(i2s_chan_handle_t handle, const void *src, size_t size,
                            size_t *bytes_written, uint32_t timeout_ms)
{
    (void)handle;
    (void)src;
    (void)timeout_ms;
    mock_i2s_write_count++;
    if (bytes_written != NULL) {
        *bytes_written = size;
    }
    return ESP_OK;
}

/* ---- external flash / partition / littlefs (storage.c tests) ---- */

static uint8_t s_stub_flash_chip;
static esp_partition_t s_stub_partition;

int mock_spi_bus_init_err = 0;
int mock_spi_add_flash_err = 0;
int mock_flash_init_err = 0;
int mock_partition_register_err = 0;
int mock_lfs_register_err = 0;
int mock_lfs_format_err = 0;
int mock_lfs_info_err = 0;
int mock_flash_read_err = 0;
int mock_flash_write_err = 0;
int mock_flash_erase_err = 0;
uint32_t mock_flash_size = 16u * 1024u * 1024u;
size_t mock_lfs_total = 1024u * 1024u;
size_t mock_lfs_used = 256u * 1024u;
int mock_lfs_register_calls = 0;
int mock_lfs_unregister_calls = 0;
int mock_lfs_format_calls = 0;

esp_err_t spi_bus_initialize(spi_host_device_t host, const spi_bus_config_t *bus_config, int dma_chan)
{
    (void)host;
    (void)bus_config;
    (void)dma_chan;
    return (esp_err_t)mock_spi_bus_init_err;
}

esp_err_t spi_bus_add_flash_device(esp_flash_t **out_chip, const esp_flash_spi_device_config_t *config)
{
    (void)config;
    if (mock_spi_add_flash_err) {
        return (esp_err_t)mock_spi_add_flash_err;
    }
    if (out_chip != NULL) {
        *out_chip = (esp_flash_t *)&s_stub_flash_chip;
    }
    return ESP_OK;
}

esp_err_t esp_flash_init(esp_flash_t *chip)
{
    (void)chip;
    return (esp_err_t)mock_flash_init_err;
}

esp_err_t esp_flash_get_size(esp_flash_t *chip, uint32_t *out_size)
{
    (void)chip;
    if (out_size != NULL) {
        *out_size = mock_flash_size;
    }
    return ESP_OK;
}

esp_err_t esp_flash_read(esp_flash_t *chip, void *buffer, uint32_t address, uint32_t length)
{
    (void)chip;
    (void)address;
    if (mock_flash_read_err) {
        return (esp_err_t)mock_flash_read_err;
    }
    if (buffer != NULL) {
        memset(buffer, 0, length);
    }
    return ESP_OK;
}

esp_err_t esp_flash_write(esp_flash_t *chip, const void *buffer, uint32_t address, uint32_t length)
{
    (void)chip;
    (void)buffer;
    (void)address;
    (void)length;
    return (esp_err_t)mock_flash_write_err;
}

esp_err_t esp_flash_erase_region(esp_flash_t *chip, uint32_t start_address, uint32_t size)
{
    (void)chip;
    (void)start_address;
    (void)size;
    return (esp_err_t)mock_flash_erase_err;
}

esp_err_t esp_partition_register_external(esp_flash_t *flash_chip, size_t offset, size_t size,
                                          const char *label, uint8_t type, uint8_t subtype,
                                          const esp_partition_t **out_partition)
{
    (void)offset;
    if (mock_partition_register_err) {
        return (esp_err_t)mock_partition_register_err;
    }
    memset(&s_stub_partition, 0, sizeof(s_stub_partition));
    s_stub_partition.flash = flash_chip;
    s_stub_partition.address = 0;
    s_stub_partition.size = (uint32_t)size;
    s_stub_partition.type = type;
    s_stub_partition.subtype = subtype;
    if (label != NULL) {
        strncpy(s_stub_partition.label, label, sizeof(s_stub_partition.label) - 1);
    }
    if (out_partition != NULL) {
        *out_partition = &s_stub_partition;
    }
    return ESP_OK;
}

esp_err_t esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t *conf)
{
    (void)conf;
    mock_lfs_register_calls++;
    return (esp_err_t)mock_lfs_register_err;
}

esp_err_t esp_vfs_littlefs_unregister(const char *partition_label)
{
    (void)partition_label;
    mock_lfs_unregister_calls++;
    return ESP_OK;
}

esp_err_t esp_vfs_littlefs_unregister_partition(const esp_partition_t *partition)
{
    (void)partition;
    mock_lfs_unregister_calls++;
    return ESP_OK;
}

esp_err_t esp_littlefs_format(const char *partition_label)
{
    (void)partition_label;
    return ESP_OK;
}

esp_err_t esp_littlefs_format_partition(const esp_partition_t *partition)
{
    (void)partition;
    mock_lfs_format_calls++;
    return (esp_err_t)mock_lfs_format_err;
}

esp_err_t esp_littlefs_partition_info(const esp_partition_t *partition, size_t *total_bytes,
                                      size_t *used_bytes)
{
    (void)partition;
    if (mock_lfs_info_err) {
        return (esp_err_t)mock_lfs_info_err;
    }
    if (total_bytes != NULL) {
        *total_bytes = mock_lfs_total;
    }
    if (used_bytes != NULL) {
        *used_bytes = mock_lfs_used;
    }
    return ESP_OK;
}
