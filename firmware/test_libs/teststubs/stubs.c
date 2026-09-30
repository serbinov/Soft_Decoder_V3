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
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/mcpwm_prelude.h"
#include "driver/i2s_std.h"
#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_flash.h"
#include "esp_flash_spi_init.h"
#include "esp_partition.h"
#include "esp_littlefs.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_vfs_dev.h"


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
/* Allow the first N creates to succeed, then fail all later ones (-1 = off). */
int mock_queue_create_fail_after = -1;
int mock_queue_create_calls = 0;
int mock_queue_send_fail = 0;
/* Allow the first N sends to succeed, then fail all later ones (-1 = off). */
int mock_queue_send_fail_after = -1;
int mock_queue_send_calls = 0;

QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item_size)
{
    (void)len;
    mock_queue_create_calls++;
    if (mock_queue_create_fail ||
        (mock_queue_create_fail_after >= 0 &&
         mock_queue_create_calls > mock_queue_create_fail_after)) {
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
    mock_queue_send_calls++;
    if (mock_queue_send_fail || (mock_queue_send_fail_after >= 0 &&
                                 mock_queue_send_calls > mock_queue_send_fail_after) ||
        mq == NULL || mq->count >= 16) {
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
/* Allow the first N task creations to succeed, then fail all later ones
 * (-1 = off). Shared by xTaskCreate and xTaskCreatePinnedToCore. */
int mock_task_create_fail_after = -1;
int mock_task_create_calls = 0;

static BaseType_t mock_task_create_result(void)
{
    mock_task_create_calls++;
    if (!mock_task_create_ok) {
        return pdFAIL;
    }
    if (mock_task_create_fail_after >= 0 &&
        mock_task_create_calls > mock_task_create_fail_after) {
        return pdFAIL;
    }
    return pdPASS;
}

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
    return mock_task_create_result();
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
    return mock_task_create_result();
}

void vTaskDelay(const TickType_t ticks)
{
    /* 1 tick == 1 ms in this mock: advance the virtual clock so read loops
     * that poll against a deadline terminate once input runs out. */
    mock_timer_now_us += (int64_t)ticks * 1000;
    mock_vtask_delay_count++;
}

void vTaskDelayUntil(TickType_t *pxPreviousWakeTime, TickType_t xTimeIncrement)
{
    (void)pxPreviousWakeTime;
    mock_timer_now_us += (int64_t)xTimeIncrement * 1000;
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
/* Self-test hooks: force nvs u32 read/write failures or a corrupted read. */
int mock_nvs_get_u32_err = 0;
int mock_nvs_set_u32_err = 0;
int mock_nvs_u32_corrupt = 0;

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
    if (mock_nvs_get_u32_err) {
        return (esp_err_t)mock_nvs_get_u32_err;
    }
    size_t len = sizeof(*out);
    esp_err_t err = mock_nvs_get(key, MOCK_NVS_U32, out, &len);
    if (err == ESP_OK && mock_nvs_u32_corrupt && out != NULL) {
        *out ^= 0xFFFFFFFFu;
    }
    return err;
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
    if (mock_nvs_set_u32_err) {
        return (esp_err_t)mock_nvs_set_u32_err;
    }
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

/* ---- ADC1 (adc_oneshot) ---- */
int mock_adc_raw[16];
int mock_adc_ok = 1;
int mock_adc_unit_new_ok = 1;
int mock_adc_config_ok = 1;

struct adc_oneshot_unit_ctx {
    int unit_id;
};
static struct adc_oneshot_unit_ctx s_mock_adc_unit;

esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *init_config,
                               adc_oneshot_unit_handle_t *ret_unit)
{
    (void)init_config;
    if (!mock_adc_unit_new_ok) {
        return ESP_FAIL;
    }
    if (ret_unit != NULL) {
        *ret_unit = &s_mock_adc_unit;
    }
    return ESP_OK;
}

esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t handle,
                                     adc_channel_t channel,
                                     const adc_oneshot_chan_cfg_t *config)
{
    (void)handle;
    (void)channel;
    (void)config;
    return mock_adc_config_ok ? ESP_OK : ESP_FAIL;
}

esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t handle,
                           adc_channel_t chan, int *out_raw)
{
    (void)handle;
    if (!mock_adc_ok || chan < 0 || chan >= 16 || out_raw == NULL) {
        return ESP_FAIL;
    }
    *out_raw = mock_adc_raw[chan];
    return ESP_OK;
}

esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t handle)
{
    (void)handle;
    return ESP_OK;
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

/* ---- MCPWM (mcpwm_prelude) ---- */
uint32_t mock_mcpwm_period_ticks = 1;
float mock_mcpwm_duty[16];
int mock_mcpwm_count = 0;

struct mcpwm_timer_ctx {
    int id;
};
struct mcpwm_oper_ctx {
    int id;
};
struct mcpwm_compr_ctx {
    int id;
};
struct mcpwm_gen_ctx {
    int id;
};

static struct mcpwm_timer_ctx s_mock_timer;
static struct mcpwm_oper_ctx s_mock_oper[16];
static struct mcpwm_compr_ctx s_mock_cmpr[16];
static struct mcpwm_gen_ctx s_mock_gen[16];
static int s_mock_oper_n;
static int s_mock_cmpr_n;
static int s_mock_gen_n;

void mock_mcpwm_reset(void)
{
    s_mock_oper_n = 0;
    s_mock_cmpr_n = 0;
    s_mock_gen_n = 0;
    mock_mcpwm_count = 0;
    mock_mcpwm_period_ticks = 1;
    for (int i = 0; i < 16; ++i) {
        mock_mcpwm_duty[i] = -1.0f;
    }
}

esp_err_t mcpwm_new_timer(const mcpwm_timer_config_t *config, mcpwm_timer_handle_t *ret_timer)
{
    if (config != NULL) {
        mock_mcpwm_period_ticks = config->period_ticks ? config->period_ticks : 1U;
    }
    if (ret_timer != NULL) {
        *ret_timer = &s_mock_timer;
    }
    return ESP_OK;
}

esp_err_t mcpwm_new_operator(const mcpwm_operator_config_t *config, mcpwm_oper_handle_t *ret_oper)
{
    (void)config;
    if (ret_oper == NULL) {
        return ESP_FAIL;
    }
    int i = s_mock_oper_n++ % 16;
    s_mock_oper[i].id = i;
    *ret_oper = &s_mock_oper[i];
    return ESP_OK;
}

esp_err_t mcpwm_operator_connect_timer(mcpwm_oper_handle_t oper, mcpwm_timer_handle_t timer)
{
    (void)oper;
    (void)timer;
    return ESP_OK;
}

esp_err_t mcpwm_new_comparator(mcpwm_oper_handle_t oper,
                               const mcpwm_comparator_config_t *config,
                               mcpwm_cmpr_handle_t *ret_cmpr)
{
    (void)oper;
    (void)config;
    if (ret_cmpr == NULL) {
        return ESP_FAIL;
    }
    int i = s_mock_cmpr_n++ % 16;
    s_mock_cmpr[i].id = i;
    mock_mcpwm_duty[i] = 0.0f;
    mock_mcpwm_count = s_mock_cmpr_n;
    *ret_cmpr = &s_mock_cmpr[i];
    return ESP_OK;
}

esp_err_t mcpwm_new_generator(mcpwm_oper_handle_t oper,
                              const mcpwm_generator_config_t *config,
                              mcpwm_gen_handle_t *ret_gen)
{
    (void)oper;
    (void)config;
    if (ret_gen == NULL) {
        return ESP_FAIL;
    }
    int i = s_mock_gen_n++ % 16;
    s_mock_gen[i].id = i;
    *ret_gen = &s_mock_gen[i];
    return ESP_OK;
}

esp_err_t mcpwm_generator_set_action_on_timer_event(mcpwm_gen_handle_t generator, int action)
{
    (void)generator;
    (void)action;
    return ESP_OK;
}

esp_err_t mcpwm_generator_set_action_on_compare_event(mcpwm_gen_handle_t generator, int action)
{
    (void)generator;
    (void)action;
    return ESP_OK;
}

esp_err_t mcpwm_comparator_set_compare_value(mcpwm_cmpr_handle_t cmpr, uint32_t value)
{
    if (cmpr == NULL) {
        return ESP_FAIL;
    }
    int i = cmpr->id % 16;
    mock_mcpwm_duty[i] = ((float)value * 100.0f) / (float)mock_mcpwm_period_ticks;
    return ESP_OK;
}

esp_err_t mcpwm_timer_enable(mcpwm_timer_handle_t timer)
{
    (void)timer;
    return ESP_OK;
}

esp_err_t mcpwm_timer_start_stop(mcpwm_timer_handle_t timer, int command)
{
    (void)timer;
    (void)command;
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
uint32_t mock_partition_registered_size = 0;
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
    mock_partition_registered_size = (uint32_t)size;
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

/* ---- UART / USB-Serial-JTAG / OTA (provision.c tests) ---- */

uint8_t mock_uart_rx[16384];
size_t mock_uart_rx_len = 0;
size_t mock_uart_rx_pos = 0;
char mock_uart_tx[65536];
size_t mock_uart_tx_len = 0;

int mock_uart_driver_install_err = 0;
int mock_usbjtag_install_ok = 0; /* 0 -> USB-Serial-JTAG driver unavailable */
int mock_usbjtag_byte = -1;      /* >=0 -> one byte available on the USB port */
int mock_ota_begin_err = 0;
int mock_ota_write_err = 0;
/* Let the first N writes succeed, then fail the rest (-1 = off). */
int mock_ota_write_fail_after = -1;
int mock_ota_write_calls = 0;
int mock_ota_end_err = 0;
int mock_ota_set_boot_err = 0;
int mock_ota_partition_absent = 0;
int mock_esp_restart_calls = 0;
uint32_t mock_ota_bytes = 0;
uint32_t mock_ota_next_size = 4u * 1024u * 1024u;

static esp_partition_t s_ota_partition;

void mock_uart_reset(void)
{
    mock_uart_rx_len = 0;
    mock_uart_rx_pos = 0;
    mock_uart_tx_len = 0;
    mock_uart_tx[0] = '\0';
}

void mock_uart_feed(const void *data, size_t n)
{
    if (mock_uart_rx_len + n <= sizeof(mock_uart_rx)) {
        memcpy(mock_uart_rx + mock_uart_rx_len, data, n);
        mock_uart_rx_len += n;
    }
}

esp_err_t uart_driver_install(uart_port_t uart_num, int rx, int tx, int q, void *queue, int flags)
{
    (void)uart_num;
    (void)rx;
    (void)tx;
    (void)q;
    (void)queue;
    (void)flags;
    return (esp_err_t)mock_uart_driver_install_err;
}

int uart_read_bytes(uart_port_t uart_num, uint8_t *buf, uint32_t length, TickType_t ticks)
{
    (void)uart_num;
    (void)ticks;
    size_t avail = mock_uart_rx_len - mock_uart_rx_pos;
    size_t n = length < avail ? length : avail;
    if (n > 0 && buf != NULL) {
        memcpy(buf, mock_uart_rx + mock_uart_rx_pos, n);
        mock_uart_rx_pos += n;
    }
    return (int)n;
}

int uart_write_bytes(uart_port_t uart_num, const void *src, size_t size)
{
    (void)uart_num;
    if (mock_uart_tx_len + size < sizeof(mock_uart_tx) && src != NULL) {
        memcpy(mock_uart_tx + mock_uart_tx_len, src, size);
        mock_uart_tx_len += size;
        mock_uart_tx[mock_uart_tx_len] = '\0';
    }
    return (int)size;
}

esp_err_t uart_flush_input(uart_port_t uart_num)
{
    (void)uart_num;
    return ESP_OK;
}

esp_err_t uart_set_baudrate(uart_port_t uart_num, uint32_t baud)
{
    (void)uart_num;
    (void)baud;
    return ESP_OK;
}

esp_err_t usb_serial_jtag_driver_install(const usb_serial_jtag_driver_config_t *cfg)
{
    (void)cfg;
    return mock_usbjtag_install_ok ? ESP_OK : ESP_FAIL;
}

int usb_serial_jtag_read_bytes(void *buf, uint32_t length, uint32_t ticks)
{
    (void)length;
    (void)ticks;
    if (mock_usbjtag_byte >= 0 && buf != NULL) {
        *(uint8_t *)buf = (uint8_t)mock_usbjtag_byte;
        mock_usbjtag_byte = -1;
        return 1;
    }
    return 0;
}

int usb_serial_jtag_write_bytes(const void *src, size_t size, uint32_t ticks)
{
    (void)src;
    (void)ticks;
    return (int)size;
}

void usb_serial_jtag_vfs_use_driver(void)
{
}

const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *start_from)
{
    (void)start_from;
    if (mock_ota_partition_absent) {
        return NULL;
    }
    memset(&s_ota_partition, 0, sizeof(s_ota_partition));
    s_ota_partition.size = mock_ota_next_size;
    return &s_ota_partition;
}

esp_err_t esp_ota_begin(const esp_partition_t *partition, size_t image_size, esp_ota_handle_t *out)
{
    (void)partition;
    (void)image_size;
    if (mock_ota_begin_err) {
        return (esp_err_t)mock_ota_begin_err;
    }
    if (out != NULL) {
        *out = 1;
    }
    mock_ota_bytes = 0;
    return ESP_OK;
}

esp_err_t esp_ota_write(esp_ota_handle_t handle, const void *data, size_t size)
{
    (void)handle;
    (void)data;
    mock_ota_write_calls++;
    if (mock_ota_write_err ||
        (mock_ota_write_fail_after >= 0 && mock_ota_write_calls > mock_ota_write_fail_after)) {
        return (esp_err_t)(mock_ota_write_err ? mock_ota_write_err : ESP_FAIL);
    }
    mock_ota_bytes += (uint32_t)size;
    return ESP_OK;
}

esp_err_t esp_ota_end(esp_ota_handle_t handle)
{
    (void)handle;
    return (esp_err_t)mock_ota_end_err;
}

esp_err_t esp_ota_abort(esp_ota_handle_t handle)
{
    (void)handle;
    return ESP_OK;
}

esp_err_t esp_ota_set_boot_partition(const esp_partition_t *partition)
{
    (void)partition;
    return (esp_err_t)mock_ota_set_boot_err;
}

void esp_restart(void)
{
    mock_esp_restart_calls++;
}
