#pragma once

#include <stdint.h>

void esp_restart(void);

/* Implemented by the test suite that needs it (selftest). */
uint32_t esp_get_free_heap_size(void);
