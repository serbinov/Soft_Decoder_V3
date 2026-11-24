#pragma once
/* Frozen OTA/platform layout contract (v1). Changing any value here or in
 * partitions.csv requires a USB full-flash update, not OTA. Kept in sync
 * with tools/ota_compat.json by test/test_ota_compat.py. */
#define PLATFORM_LAYOUT_VERSION      1
#define PLATFORM_BOOTLOADER_MAX      0x8000u
#define PLATFORM_PART_TABLE_OFFSET   0x8000u
#define PLATFORM_OTADATA_OFFSET      0x1A000u
#define PLATFORM_OTADATA_SIZE        0x2000u
#define PLATFORM_OTA_0_OFFSET        0x20000u
#define PLATFORM_OTA_1_OFFSET        0x200000u
#define PLATFORM_OTA_SLOT_SIZE       0x1E0000u
