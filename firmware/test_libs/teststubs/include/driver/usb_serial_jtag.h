#pragma once

#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

typedef struct {
    int tx_buffer_size;
    int rx_buffer_size;
    int intr_priority;
    int flags;
} usb_serial_jtag_driver_config_t;

#define USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT() \
    (usb_serial_jtag_driver_config_t) { 0, 0, 0, 0 }

esp_err_t usb_serial_jtag_driver_install(const usb_serial_jtag_driver_config_t *cfg);
int usb_serial_jtag_read_bytes(void *buf, uint32_t length, uint32_t ticks_to_wait);
int usb_serial_jtag_write_bytes(const void *src, size_t size, uint32_t ticks_to_wait);
