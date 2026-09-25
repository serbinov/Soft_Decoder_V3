#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esp_err.h"
#include "esp_partition.h"

typedef struct {
    const char *base_path;
    const char *partition_label;
    const esp_partition_t *partition;
    bool format_if_mount_failed;
    bool dont_mount;
} esp_vfs_littlefs_conf_t;

esp_err_t esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t *conf);
esp_err_t esp_vfs_littlefs_unregister(const char *partition_label);
esp_err_t esp_vfs_littlefs_unregister_partition(const esp_partition_t *partition);
esp_err_t esp_littlefs_format(const char *partition_label);
esp_err_t esp_littlefs_format_partition(const esp_partition_t *partition);
esp_err_t esp_littlefs_partition_info(const esp_partition_t *partition,
                                      size_t *total_bytes, size_t *used_bytes);
