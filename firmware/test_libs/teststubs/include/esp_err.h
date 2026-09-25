#ifndef MOCK_ESP_ERR_H
#define MOCK_ESP_ERR_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef int32_t esp_err_t;

#define ESP_OK                      0
#define ESP_FAIL                    (-1)
#define ESP_ERR_NO_MEM              0x101
#define ESP_ERR_INVALID_ARG         0x102
#define ESP_ERR_INVALID_STATE       0x103
#define ESP_ERR_INVALID_SIZE        0x104
#define ESP_ERR_NOT_FOUND           0x105
#define ESP_ERR_NOT_SUPPORTED       0x106
#define ESP_ERR_TIMEOUT             0x107
#define ESP_ERR_INVALID_RESPONSE    0x108
#define ESP_ERR_INVALID_CRC         0x109
#define ESP_ERR_INVALID_VERSION     0x10A
#define ESP_ERR_INVALID_MAC         0x10B
#define ESP_ERR_INVALID_DATA        0x10C
#define ESP_ERR_NOT_ALLOWED         0x10D

#define ESP_ERROR_CHECK(x) \
    do { esp_err_t __ec = (x); if (__ec != ESP_OK) { \
        fprintf(stderr, "ESP_ERROR_CHECK failed: %d at %s:%d\n", (int)__ec, __FILE__, __LINE__); \
        abort(); } } while (0)

const char *esp_err_to_name(esp_err_t code);

#endif /* MOCK_ESP_ERR_H */
