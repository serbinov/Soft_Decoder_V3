#ifndef SOUND_EDITOR_ASSETS_H
#define SOUND_EDITOR_ASSETS_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char *path;
    const char *mime;
    const unsigned char *data;
    size_t length;
    bool immutable;
} sound_editor_asset_t;

extern const sound_editor_asset_t sound_editor_assets[];
extern const size_t sound_editor_asset_count;

#endif
