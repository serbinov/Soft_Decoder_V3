#pragma once

#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

typedef int uart_port_t;

#define UART_NUM_0 0

esp_err_t uart_driver_install(uart_port_t uart_num, int rx_buffer_size, int tx_buffer_size,
                              int queue_size, void *uart_queue, int intr_alloc_flags);
int uart_read_bytes(uart_port_t uart_num, uint8_t *buf, uint32_t length, TickType_t ticks_to_wait);
int uart_write_bytes(uart_port_t uart_num, const void *src, size_t size);
esp_err_t uart_flush_input(uart_port_t uart_num);
esp_err_t uart_set_baudrate(uart_port_t uart_num, uint32_t baud_rate);
