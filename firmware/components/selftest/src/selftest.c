#include "selftest.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "audio.h"
#include "auxio.h"
#include "dcc.h"
#include "motor.h"
#include "pinmap.h"
#include "settings.h"
#include "storage.h"
#include "web.h"

static const char *TAG = "selftest";

#define SELFTEST_NVS_NS   "selftest"
#define SELFTEST_NVS_MAGIC 0x5E1F7E57u

/* Overridable so the host tests can point the scratch file at a temp dir. */
#ifndef SELFTEST_TMP_PATH
#define SELFTEST_TMP_PATH "/userdata/.selftest.tmp"
#endif

/* File-I/O wrappers so the host tests can inject failures (same pattern as
 * PROV_FWRITE / WEB_FWRITE). */
#ifndef SELFTEST_FOPEN
#define SELFTEST_FOPEN(p, m) fopen((p), (m))
#endif
#ifndef SELFTEST_FWRITE
#define SELFTEST_FWRITE(p, sz, n, f) fwrite((p), (sz), (n), (f))
#endif
#ifndef SELFTEST_FREAD
#define SELFTEST_FREAD(p, sz, n, f) fread((p), (sz), (n), (f))
#endif

/* Not const so the host test can redirect it at a missing directory. */
static const char *s_tmp_path = SELFTEST_TMP_PATH;

const char *selftest_state_name(selftest_state_t state)
{
    switch (state) {
        case SELFTEST_PASS:
            return "PASS";
        case SELFTEST_FAIL:
            return "FAIL";
        default:
            return "SKIP";
    }
}

static void add(selftest_report_t *r, const char *name, selftest_state_t state)
{
    if (r->count < SELFTEST_MAX_CHECKS) {
        r->items[r->count].name = name;
        r->items[r->count].state = state;
        r->count++;
    }
}

static selftest_state_t check_heap(void)
{
    return (esp_get_free_heap_size() > 4096U) ? SELFTEST_PASS : SELFTEST_FAIL;
}

static selftest_state_t check_pinmap(void)
{
    return (pinmap_validate() == ESP_OK) ? SELFTEST_PASS : SELFTEST_FAIL;
}

static selftest_state_t check_settings_cv(void)
{
    uint8_t v = 0;
    if (settings_cv_read(1, &v) != ESP_OK) {
        return SELFTEST_FAIL;
    }
    if (settings_cv_read(0, &v) != ESP_ERR_INVALID_ARG) {
        return SELFTEST_FAIL;
    }
    if (settings_cv_read(SETTINGS_CV_COUNT + 1U, &v) != ESP_ERR_INVALID_ARG) {
        return SELFTEST_FAIL;
    }
    return SELFTEST_PASS;
}

static selftest_state_t check_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(SELFTEST_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return SELFTEST_FAIL;
    }
    uint32_t back = 0;
    esp_err_t err = nvs_set_u32(h, "magic", SELFTEST_NVS_MAGIC);
    if (err == ESP_OK) {
        err = nvs_get_u32(h, "magic", &back);
    }
    (void)nvs_erase_key(h, "magic");
    (void)nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        return SELFTEST_FAIL;
    }
    return (back == SELFTEST_NVS_MAGIC) ? SELFTEST_PASS : SELFTEST_FAIL;
}

static selftest_state_t check_storage(void)
{
    if (!storage_is_mounted()) {
        return SELFTEST_SKIP;
    }
    const char payload[] = "selftest";
    bool ok = false;
    FILE *f = SELFTEST_FOPEN(s_tmp_path, "wb");
    if (f != NULL) {
        size_t w = SELFTEST_FWRITE(payload, 1, sizeof(payload), f);
        (void)fflush(f);
        (void)fclose(f);
        if (w == sizeof(payload)) {
            char back[16] = { 0 };
            f = SELFTEST_FOPEN(s_tmp_path, "rb");
            if (f != NULL) {
                size_t r = SELFTEST_FREAD(back, 1, sizeof(payload), f);
                (void)fclose(f);
                ok = (r == sizeof(payload)) &&
                     memcmp(back, payload, sizeof(payload)) == 0;
            }
        }
        (void)remove(s_tmp_path);
    }
    return ok ? SELFTEST_PASS : SELFTEST_FAIL;
}

static selftest_state_t check_adc(void)
{
    uint16_t b1 = 0, b2 = 0, rail = 0;
    motor_bemf_adc_dump(&b1, &b2, &rail);
    if (b1 > 4095U || b2 > 4095U || rail > 4095U) {
        return SELFTEST_FAIL;
    }
    return SELFTEST_PASS;
}

static selftest_state_t check_audio(void)
{
    return (audio_get_volume() <= 100U) ? SELFTEST_PASS : SELFTEST_FAIL;
}

static selftest_state_t check_auxio(void)
{
    bool enabled = false;
    return (auxio_get_enabled(0, &enabled) == ESP_OK) ? SELFTEST_PASS : SELFTEST_FAIL;
}

static selftest_state_t check_dcc(void)
{
    (void)dcc_last_packet_us();
    return SELFTEST_PASS;
}

uint8_t selftest_run(selftest_report_t *report)
{
    if (report == NULL) {
        return 0;
    }
    memset(report, 0, sizeof(*report));

    add(report, "heap", check_heap());
    add(report, "pinmap", check_pinmap());
    add(report, "settings_cv", check_settings_cv());
    add(report, "nvs", check_nvs());
    add(report, "storage", check_storage());
    add(report, "adc", check_adc());
    add(report, "audio", check_audio());
    add(report, "auxio", check_auxio());
    add(report, "dcc", check_dcc());

    uint8_t passed = 0;
    for (uint8_t i = 0; i < report->count; ++i) {
        if (report->items[i].state == SELFTEST_PASS) {
            passed++;
        }
    }
    ESP_LOGI(TAG, "selftest: %u/%u passed", (unsigned)passed, (unsigned)report->count);
    return passed;
}

/* ---------------- actuating HIL commands ---------------- */

esp_err_t selftest_act_aux(uint8_t channel, uint16_t ms)
{
    if (channel >= AUXIO_CH_COUNT || ms < SELFTEST_ACT_MS_MIN ||
        ms > SELFTEST_ACT_MS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    bool prev = false;
    if (auxio_get_enabled(channel, &prev) != ESP_OK) {
        return ESP_FAIL;
    }
    if (auxio_set_enabled(channel, true) != ESP_OK) {
        (void)auxio_set_enabled(channel, prev);
        return ESP_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(ms));
    (void)auxio_set_enabled(channel, prev);
    return ESP_OK;
}

esp_err_t selftest_act_sound(uint8_t slot, uint16_t ms)
{
    if (slot < 1U || slot > SETTINGS_MAX_TRACKS || ms < SELFTEST_ACT_MS_MIN ||
        ms > SELFTEST_ACT_MS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    settings_track_t tracks[SETTINGS_MAX_TRACKS];
    size_t count = 0;
    if (settings_tracks_load(tracks, &count) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    const char *file = NULL;
    for (size_t i = 0; i < count; ++i) {
        if (tracks[i].slot == slot && tracks[i].enabled && tracks[i].file[0] != '\0') {
            file = tracks[i].file;
            break;
        }
    }
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", storage_get_root(), file);
    esp_err_t err = audio_voice_play(0, path, false, audio_get_volume());
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(ms));
    (void)audio_voice_stop(0);
    return ESP_OK;
}

esp_err_t selftest_act_motor(uint8_t speed, uint16_t ms)
{
    if (speed < 1U || speed > SELFTEST_MOTOR_SPEED_MAX ||
        ms < SELFTEST_ACT_MS_MIN || ms > SELFTEST_ACT_MS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (motor_set_speed(speed, true) != ESP_OK) {
        return ESP_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(ms));
    motor_stop();
    return ESP_OK;
}

esp_err_t selftest_act_function(uint8_t fn, bool on)
{
    if (fn >= SETTINGS_FUNC_MAP_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Same entry point the DCC decoder and the web UI use. */
    web_apply_function(fn, on);
    return ESP_OK;
}

uint8_t selftest_act_fn_sweep(uint16_t ms)
{
    if (ms < SELFTEST_ACT_MS_MIN || ms > SELFTEST_SWEEP_MS_MAX) {
        return 0U;
    }
    for (uint8_t fn = 0; fn < SETTINGS_FUNC_MAP_COUNT; ++fn) {
        web_apply_function(fn, true);
        vTaskDelay(pdMS_TO_TICKS(ms));
        web_apply_function(fn, false);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return SETTINGS_FUNC_MAP_COUNT;
}

uint8_t selftest_act_aux_sweep(uint16_t ms)
{
    if (ms < SELFTEST_ACT_MS_MIN || ms > SELFTEST_SWEEP_MS_MAX) {
        return 0U;
    }
    uint8_t done = 0;
    for (uint8_t ch = 0; ch < AUXIO_CH_COUNT; ++ch) {
        if (selftest_act_aux(ch, ms) == ESP_OK) {
            done++;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return done;
}
