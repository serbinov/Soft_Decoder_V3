#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_pack.h"
#include "settings.h"
#include "sound_graph_store.h"
#include "web_internal.h"

typedef struct {
    char staged[160];
    char canonical[160];
    bool published;
} pack_file_t;

typedef struct {
    char backup_dir[96];
    char (*old_names)[WEB_LIB_NAME_MAX];
    bool *old_moved;
    size_t old_count;
    char (*new_names)[WEB_LIB_NAME_MAX];
    size_t new_count;
    pack_file_t *files;
    size_t file_count;
    char journal[160];
    bool journal_written;
    char old_selection[SG_ID_CAP];
    uint32_t old_selection_rev;
    bool old_selection_valid;
    char new_selection[SG_ID_CAP];
    bool new_selection_valid;
} pack_tx_t;

#define PACK_JOURNAL_MAGIC "AURAPKJ2"
#define PACK_JOURNAL_MAGIC_OLD "AURAPKJ1"
#define PACK_JOURNAL_PREPARED 1U
#define PACK_JOURNAL_COMMITTED 2U
#define PACK_JOURNAL_MAX_NAMES 512U
#define PACK_JOURNAL_PATH WEB_AUDIO_DIR "/.packjournal"
#define PACK_JOURNAL_TMP  PACK_JOURNAL_PATH ".tmp"

typedef struct {
    char magic[8];
    uint32_t phase;
    uint32_t old_count;
    uint32_t new_count;
    char backup_dir[96];
    char old_selection[SG_ID_CAP];
    uint32_t old_selection_rev;
    uint32_t old_selection_valid;
    char new_selection[SG_ID_CAP];
    uint32_t new_selection_valid;
} pack_journal_hdr_t;

bool pack_journal_name_ok(const char *name);
bool pack_backup_cleanup(const char *backup_dir, char (*names)[WEB_LIB_NAME_MAX],
                         size_t count);
