#ifndef MOCK_DRIVER_I2S_STD_H
#define MOCK_DRIVER_I2S_STD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/gpio.h"

typedef struct i2s_channel_obj_t *i2s_chan_handle_t;

typedef enum {
    I2S_NUM_0 = 0,
    I2S_NUM_1,
} i2s_port_t;

typedef enum {
    I2S_ROLE_MASTER = 0,
    I2S_ROLE_SLAVE,
} i2s_role_t;

typedef enum {
    I2S_DATA_BIT_WIDTH_8BIT = 8,
    I2S_DATA_BIT_WIDTH_16BIT = 16,
    I2S_DATA_BIT_WIDTH_24BIT = 24,
    I2S_DATA_BIT_WIDTH_32BIT = 32,
} i2s_data_bit_width_t;

typedef enum {
    I2S_SLOT_MODE_MONO = 1,
    I2S_SLOT_MODE_STEREO = 2,
} i2s_slot_mode_t;

#define I2S_GPIO_UNUSED ((gpio_num_t)-1)

typedef struct {
    i2s_port_t id;
    i2s_role_t role;
    bool auto_clear;
    uint32_t dma_desc_num;
    uint32_t dma_frame_num;
} i2s_chan_config_t;

typedef struct {
    uint32_t sample_rate_hz;
} i2s_std_clk_config_t;

typedef struct {
    i2s_data_bit_width_t data_bit_width;
    i2s_data_bit_width_t slot_bit_width;
    i2s_slot_mode_t slot_mode;
} i2s_std_slot_config_t;

typedef struct {
    bool mclk_inv;
    bool bclk_inv;
    bool ws_inv;
} i2s_std_gpio_invert_t;

typedef struct {
    gpio_num_t mclk;
    gpio_num_t bclk;
    gpio_num_t ws;
    gpio_num_t dout;
    gpio_num_t din;
    i2s_std_gpio_invert_t invert_flags;
} i2s_std_gpio_config_t;

typedef struct {
    i2s_std_clk_config_t clk_cfg;
    i2s_std_slot_config_t slot_cfg;
    i2s_std_gpio_config_t gpio_cfg;
} i2s_std_config_t;

#define I2S_CHANNEL_DEFAULT_CONFIG(port, r) \
    { .id = (port), .role = (r), .auto_clear = false, .dma_desc_num = 6, .dma_frame_num = 240 }

#define I2S_STD_CLK_DEFAULT_CONFIG(rate) \
    { .sample_rate_hz = (rate) }

#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, mode) \
    { .data_bit_width = (bits), .slot_bit_width = (bits), .slot_mode = (mode) }

extern int mock_i2s_write_count;
extern int mock_i2s_enable_err;
extern int mock_i2s_disable_count;
extern int mock_i2s_delete_count;

esp_err_t i2s_new_channel(const i2s_chan_config_t *chan_cfg,
                          i2s_chan_handle_t *tx_handle, i2s_chan_handle_t *rx_handle);
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t handle, const i2s_std_config_t *std_cfg);
esp_err_t i2s_channel_enable(i2s_chan_handle_t handle);
esp_err_t i2s_channel_disable(i2s_chan_handle_t handle);
esp_err_t i2s_del_channel(i2s_chan_handle_t handle);
esp_err_t i2s_channel_write(i2s_chan_handle_t handle, const void *src, size_t size,
                            size_t *bytes_written, uint32_t timeout_ms);

#endif /* MOCK_DRIVER_I2S_STD_H */
