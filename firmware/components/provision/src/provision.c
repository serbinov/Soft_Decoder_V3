#include "provision.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs_dev.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "settings.h"
#include "storage.h"
#include "motor.h"
#include "audio.h"
#include "audio_pack.h"
#include "web.h"
#include "selftest.h"
#include "sound.h"

static const char *TAG = "prov";
static const esp_partition_t *s_pending_boot;

#define NVS_NAMESPACE    "decoder"
#define NVS_KEY_PROV     "prov"
#define PROV_BAUD        921600
#define PROV_CHUNK       4096
#define PROV_PACK_FRAME  32768
#define PROV_PACK_CHUNK  32768
#define PROV_PACK_MAX    (15U * 1024U * 1024U)
#define PROV_WINDOW_MS   20000
/* The provisioning entry point erases the external NOR, so the host must send
 * an explicit confirmation after "PROV" before anything is destroyed. */
#define PROV_CONFIRM_TIMEOUT_MS 10000
#ifndef PROV_AUDIO_DIR
#define PROV_AUDIO_DIR   "/userdata/audio"
#endif

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define PROV_MKDIR(p) _mkdir(p)
#define PROV_FSYNC(f) _commit(_fileno(f))
#else
#include <unistd.h>
#define PROV_MKDIR(p) mkdir((p), 0755)
#define PROV_FSYNC(f) fsync(fileno(f))
#endif

/* Overridable so host tests can inject a write failure. */
#ifndef PROV_FWRITE
#define PROV_FWRITE(p, sz, n, f) fwrite((p), (sz), (n), (f))
#endif

/* True while the boot-time provisioning window is open; the background
 * listener ignores "PROV" in that state so it cannot re-trigger a reboot. */
static volatile bool s_provisioning = false;

/* True once the USB-Serial-JTAG driver is installed. The board is usually
 * connected to the host through the native USB-Serial-JTAG port, so the
 * provisioning channel must accept input from both UART0 and the USB port. */
static bool s_usbjtag_ok = false;

/* ---- NVS request flag ------------------------------------------------ */

static bool get_prov_flag(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t v = 0;
    (void)nvs_get_u8(h, NVS_KEY_PROV, &v);
    nvs_close(h);
    return v != 0;
}

static void clear_prov_flag(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    (void)nvs_set_u8(h, NVS_KEY_PROV, 0);
    (void)nvs_commit(h);
    nvs_close(h);
}

/* ---- UART helpers ----------------------------------------------------- */

/* Read one byte from UART0 or USB-Serial-JTAG before `deadline_us`. Polls
 * both ports non-blocking so a host on either link is served equally fast. */
static bool read_byte_any(uint8_t *c, int64_t deadline_us)
{
    while (esp_timer_get_time() < deadline_us) {
        int r = uart_read_bytes(UART_NUM_0, c, 1, 0);
        if (r == 1) {
            return true;
        }
        if (s_usbjtag_ok && usb_serial_jtag_read_bytes(c, 1, 0) == 1) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return false;
}

static bool read_line(char *line, size_t maxlen, uint32_t timeout_ms)
{
    size_t len = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        uint8_t c;
        if (!read_byte_any(&c, deadline)) {
            break;
        }
        if (c == '\n') {
            line[len] = '\0';
            return true;
        }
        if (len < maxlen - 1) {
            line[len++] = (char)c;
        }
    }
    line[len] = '\0';
    return false;
}

static bool uart_read_exact(uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    size_t got = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (got < len) {
        size_t want = len - got;
        int r = uart_read_bytes(UART_NUM_0, buf + got, (uint32_t)want, 0);
        if (r > 0) {
            got += (size_t)r;
            continue;
        }
        if (s_usbjtag_ok) {
            int n = usb_serial_jtag_read_bytes(buf + got, (uint32_t)want, 0);
            if (n > 0) {
                got += (size_t)n;
                continue;
            }
        }
        if (esp_timer_get_time() > deadline) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return true;
}

static bool s_pack_stream_active;
static uint8_t *s_pack_frame;
static size_t s_pack_frame_len;
static size_t s_pack_frame_pos;
static uint32_t s_pack_size;
static uint32_t s_pack_received;
static bool pack_stream_read(uint8_t *buf, size_t len, uint32_t timeout_ms);

/* During PACK installation the parser reads from acknowledged PACK-DATA
 * frames instead of the UART. FW/PUT paths use uart_read_exact directly. */
static bool read_exact(uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    if (s_pack_stream_active) {
        return pack_stream_read(buf, len, timeout_ms);
    }
    return uart_read_exact(buf, len, timeout_ms);
}

/* File scope (not a function-local static) so the white-box host tests can
 * observe the already-installed guard. */
static bool s_uart_driver_installed = false;

static void ensure_uart_driver(void)
{
    if (s_uart_driver_installed) {
        return;
    }
    /* The console uses the ROM-level UART driver on UART0; the standard UART
     * driver is not installed, so uart_read_bytes would fail. Install it here
     * (it coexists with the ROM console TX for printf). */
    esp_err_t err = uart_driver_install(UART_NUM_0, 32768, 0, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "uart driver install: %s", esp_err_to_name(err));
    }
    /* Only mark installed on success so a failed install is retried later. */
    s_uart_driver_installed = (err == ESP_OK || err == ESP_ERR_INVALID_STATE);
}

static void ensure_usbjtag_driver(void)
{
    if (s_usbjtag_ok) {
        return;
    }
    /* The USB-Serial-JTAG driver is not installed by the console unless an
     * esp_console REPL is started, so install it here to be able to receive
     * the provisioning commands from the native USB port. */
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 65536;
    cfg.tx_buffer_size = 4096;
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err == ESP_OK) {
        s_usbjtag_ok = true;
        /* Route console output through the driver so LL writes do not race
         * with the driver ISR on the shared TX FIFO. */
        usb_serial_jtag_vfs_use_driver();
    } else {
        ESP_LOGW(TAG, "usb_serial_jtag driver install: %s", esp_err_to_name(err));
    }
}

static void send_line(const char *s)
{
    uart_write_bytes(UART_NUM_0, (const uint8_t *)s, strlen(s));
    uart_write_bytes(UART_NUM_0, (const uint8_t *)"\n", 1);
    if (s_usbjtag_ok) {
        (void)usb_serial_jtag_write_bytes((const uint8_t *)s, strlen(s), pdMS_TO_TICKS(1000));
        (void)usb_serial_jtag_write_bytes((const uint8_t *)"\n", 1, pdMS_TO_TICKS(1000));
    }
}

/* Pack parser byte source. A frame is acknowledged only when the parser has
 * consumed all of it, which gives natural backpressure while WAV files are
 * written to the external NOR. The last frame is covered by PACK-OK. */
static bool pack_stream_read(uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    size_t got = 0;
    while (got < len) {
        if (s_pack_frame_pos < s_pack_frame_len) {
            size_t n = s_pack_frame_len - s_pack_frame_pos;
            if (n > len - got) {
                n = len - got;
            }
            memcpy(buf + got, s_pack_frame + s_pack_frame_pos, n);
            s_pack_frame_pos += n;
            s_pack_received += (uint32_t)n;
            got += n;
            continue;
        }

        if (s_pack_frame_len > 0) {
            char ack[48];
            snprintf(ack, sizeof(ack), "PACK-DATA-OK %lu", (unsigned long)s_pack_received);
            send_line(ack);
            s_pack_frame_len = 0;
            s_pack_frame_pos = 0;
        }

        char line[64];
        if (!read_line(line, sizeof(line), 30000)) {
            send_line("PACK-ERR timeout");
            return false;
        }
        if (strncmp(line, "PACK-DATA ", 10) != 0) {
            send_line("PACK-ERR cmd");
            return false;
        }
        long n = 0;
        if (sscanf(line + 10, "%ld", &n) != 1 || n <= 0 ||
            n > (long)PROV_PACK_FRAME ||
            s_pack_received + (uint32_t)n > s_pack_size) {
            send_line("PACK-ERR frame");
            return false;
        }
        if (!uart_read_exact(s_pack_frame, (size_t)n, timeout_ms)) {
            send_line("PACK-ERR timeout");
            return false;
        }
        s_pack_frame_len = (size_t)n;
        s_pack_frame_pos = 0;
    }
    return true;
}

/* ---- BEMF calibration over UART/USB ---------------------------------- */

static void send_line_vals(const char *label, const uint16_t *vals, uint8_t n)
{
    char buf[192];
    size_t used = 0;
    used += (size_t)snprintf(buf + used, sizeof(buf) - used, "    %s = { ", label);
    for (uint8_t i = 0; i < n; ++i) {
        used += (size_t)snprintf(buf + used, sizeof(buf) - used, "%s%u",
                                 i == 0 ? "" : ", ", (unsigned)vals[i]);
    }
    used += (size_t)snprintf(buf + used, sizeof(buf) - used, " },");
    send_line(buf);
}

/* Print the active coefficients as: BEMF-OK <count> <spd> <frac> <spd> <frac> ...
 * (frac is the rail fraction scaled by 1024). */
static void send_bemf_list(const motor_bemf_cal_info_t *info)
{
    char buf[192];
    size_t used = 0;
    used += (size_t)snprintf(buf + used, sizeof(buf) - used, "BEMF-OK %u",
                             (unsigned)info->count);
    for (uint8_t i = 0; i < info->count; ++i) {
        used += (size_t)snprintf(buf + used, sizeof(buf) - used, " %u %u",
                                 (unsigned)info->speed[i], (unsigned)info->frac[i]);
    }
    send_line(buf);
}

/* Print a ready-to-paste firmware base header with the current coefficients. */
static void send_bemf_header(const motor_bemf_cal_info_t *info)
{
    uint8_t n = info->count > MOTOR_BEMF_CAL_MAX_POINTS ? MOTOR_BEMF_CAL_MAX_POINTS
                                                        : info->count;
    uint16_t tmp[MOTOR_BEMF_CAL_MAX_POINTS];
    char buf[160];

    send_line("/* BEMF calibration base - GENERATED. Replace the file");
    send_line(" * firmware/components/motor/include/bemf_cal_base.h with this");
    send_line(" * content, then rebuild and flash the firmware. */");
    send_line("#ifndef BEMF_CAL_BASE_H");
    send_line("#define BEMF_CAL_BASE_H");
    send_line("");
    send_line("#include \"settings.h\"");
    send_line("");
    send_line("static const settings_bemf_cal_t BEMF_CAL_BASE = {");
    snprintf(buf, sizeof(buf), "    .count = %u,", (unsigned)n);
    send_line(buf);
    for (uint8_t i = 0; i < n; ++i) {
        tmp[i] = info->speed[i];
    }
    send_line_vals(".speed", tmp, n);
    for (uint8_t i = 0; i < n; ++i) {
        tmp[i] = info->frac[i];
    }
    send_line_vals(".frac", tmp, n);
    send_line("};");
    send_line("");
    send_line("#endif /* BEMF_CAL_BASE_H */");
    send_line("BEMF-HDR-END");
}

/* Serial commands (115200 baud, UART0 or native USB-Serial-JTAG):
 *   BEMF?      -> dump the active coefficients
 *   BEMF-HDR   -> print the full firmware base header for the coefficients
 *   BEMF=...   -> store coefficients (pairs "spd,frac", comma separated)
 *   BEMF-RAW   -> dump raw BEMF measurement diagnostics
 *   BEMF-ADC   -> dump direct ADC raw counts (bemf1 bemf2 rail)
 *   BEMF-TEST  -> drive the motor at a speed, e.g. "BEMF-TEST 60" or "BEMF-TEST 60 rev"
 *   BEMF-CAL   -> start the legacy 10-point calibration (motor unloaded)
 *   BEMF-CAL <mode> [points] [settle_ms] [interval_ms] [samples] [retries] [apply_pid]
 *              -> adaptive calibration: AUTO, FAST, BALANCED, THOROUGH, CUSTOM
 *   BEMF-CALSPD 12,18,24,... -> CUSTOM plan with at least 10 ascending speeds
 *   BEMF-PROFILE? -> dump the last adaptive profile/recommended PID
 *   BEMF-CLR   -> clear the stored calibration
 */
static bool bemf_parse_mode(const char *s, motor_bemf_cal_mode_t *mode)
{
    if (s == NULL || mode == NULL) return false;
    if (strcasecmp(s, "AUTO") == 0 || strcasecmp(s, "ADAPTIVE") == 0) {
        *mode = MOTOR_BEMF_MODE_AUTO;
    } else if (strcasecmp(s, "FAST") == 0) {
        *mode = MOTOR_BEMF_MODE_FAST;
    } else if (strcasecmp(s, "BALANCED") == 0) {
        *mode = MOTOR_BEMF_MODE_BALANCED;
    } else if (strcasecmp(s, "THOROUGH") == 0) {
        *mode = MOTOR_BEMF_MODE_THOROUGH;
    } else if (strcasecmp(s, "CUSTOM") == 0) {
        *mode = MOTOR_BEMF_MODE_CUSTOM;
    } else {
        return false;
    }
    return true;
}

static void handle_bemf_cmd(const char *line)
{
    if (strcmp(line, "BEMF?") == 0 || strcmp(line, "BEMF-HDR") == 0) {
        motor_bemf_cal_info_t info;
        motor_bemf_cal_info(&info);
        if (strcmp(line, "BEMF-HDR") == 0) {
            send_bemf_header(&info);
        } else {
            send_bemf_list(&info);
        }
        return;
    }
    if (strcmp(line, "BEMF-RAW") == 0) {
        motor_bemf_diag_t d;
        motor_bemf_diag(&d);
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "BEMF-RAW %lu %lu %lu %lu %u %u %u %u %u %lu %ld %ld %ld %u %ld",
                 (unsigned long)d.rail_mv, (unsigned long)d.bemf1_mv,
                 (unsigned long)d.bemf2_mv, (unsigned long)d.bemf_filtered_mv,
                 d.bemf_valid ? 1 : 0, d.cal_valid ? 1 : 0,
                 d.cal_active ? 1 : 0, (unsigned)d.cal_step,
                 (unsigned)d.applied_speed, (unsigned long)d.duty,
                 (long)d.error, (long)d.integral, (long)d.corr,
                 d.pid_ok ? 1 : 0, (long)d.target);
        send_line(buf);
        return;
    }
    if (strcmp(line, "BEMF-ADC") == 0) {
        uint16_t b1 = 0, b2 = 0, rail = 0;
        char buf[96];
        esp_err_t err = motor_bemf_adc_dump_checked(&b1, &b2, &rail);
        if (err != ESP_OK) {
            snprintf(buf, sizeof(buf), "BEMF-ADC-ERR %s", esp_err_to_name(err));
            send_line(buf);
            return;
        }
        snprintf(buf, sizeof(buf), "BEMF-ADC %u %u %u", (unsigned)b1, (unsigned)b2,
                 (unsigned)rail);
        send_line(buf);
        return;
    }
    if (strcmp(line, "BEMF-COAST") == 0) {
        uint16_t b1 = 0, b2 = 0;
        motor_bemf_coast_read(&b1, &b2);
        char buf[64];
        snprintf(buf, sizeof(buf), "BEMF-COAST %u %u", (unsigned)b1, (unsigned)b2);
        send_line(buf);
        return;
    }
    if (strcmp(line, "BEMF-PROFILE?") == 0) {
        motor_bemf_cal_info_t info;
        motor_bemf_cal_info(&info);
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "BEMF-PROFILE %u %u %u %u %u %u %u %u %u %u %u %u %u %u",
                 (unsigned)info.profile.mode, (unsigned)info.profile.planned_points,
                 (unsigned)info.profile.measured_points,
                 (unsigned)info.profile.stalled_points,
                 (unsigned)info.profile.ceiling_speed,
                 (unsigned)info.profile.first_move_speed,
                 (unsigned)info.profile.settle_ms_avg,
                 (unsigned)info.profile.plant_gain_x1000,
                 (unsigned)info.profile.kp_x100, (unsigned)info.profile.ki_x1000,
                 (unsigned)info.profile.kd_x100, (unsigned)info.profile.quality,
                 info.profile.pid_applied ? 1U : 0U, (unsigned)info.count);
        send_line(buf);
        return;
    }
    if (strcmp(line, "BEMF-CAL") == 0) {
        esp_err_t err = motor_bemf_cal_start();
        send_line(err == ESP_OK ? "BEMF-CAL-OK"
                                : (err == ESP_ERR_INVALID_STATE ? "BEMF-ERR busy"
                                                                : "BEMF-ERR start"));
        return;
    }
    if (strncmp(line, "BEMF-CALSPD ", 12) == 0) {
        motor_bemf_cal_config_t cfg = {0};
        cfg.mode = MOTOR_BEMF_MODE_CUSTOM;
        cfg.settle_ms = 500;
        cfg.settle_max_ms = 2000;
        cfg.interval_ms = 40;
        cfg.samples = 10;
        cfg.retries = 1;
        cfg.boost_ms = 120;
        uint8_t n = 0;
        int last = 0;
        const char *p = line + 12;
        bool bad = false;
        while (*p != '\0' && n < MOTOR_BEMF_CAL_MAX_POINTS) {
            char *end = NULL;
            long v = strtol(p, &end, 10);
            if (end == p || v < 1 || v > 126 || v <= last) {
                bad = true;
                break;
            }
            cfg.speed[n++] = (uint8_t)v;
            last = (int)v;
            p = end;
            while (*p == ' ') ++p;
            if (*p == ',') {
                ++p;
                while (*p == ' ') ++p;
            } else {
                break;
            }
        }
        while (*p == ' ') ++p;
        if (bad || *p != '\0' || n < MOTOR_BEMF_CAL_MIN_POINTS) {
            send_line("BEMF-ERR args");
            return;
        }
        cfg.speed_count = n;
        cfg.points = n;
        esp_err_t err = motor_bemf_cal_start_ex(&cfg);
        send_line(err == ESP_OK ? "BEMF-CAL-OK"
                                : (err == ESP_ERR_INVALID_STATE ? "BEMF-ERR busy"
                                                                : "BEMF-ERR start"));
        return;
    }
    if (strncmp(line, "BEMF-CAL ", 9) == 0) {
        char mode[16] = {0};
        int points = 0, settle = 0, interval = 0, samples = 0, retries = 0, apply = 0;
        int n = sscanf(line + 9, "%15s %d %d %d %d %d %d", mode, &points, &settle,
                       &interval, &samples, &retries, &apply);
        motor_bemf_cal_mode_t m;
        if (n < 1 || !bemf_parse_mode(mode, &m)) {
            send_line("BEMF-ERR args");
            return;
        }
        /* Leave unset fields at zero so start_ex() applies the selected
         * mode's defaults (FAST 10, BALANCED 12, THOROUGH/AUTO 16). */
        motor_bemf_cal_config_t cfg = {0};
        cfg.mode = m;
        if (n >= 2 && points > 0) cfg.points = (uint8_t)points;
        if (n >= 3 && settle > 0) cfg.settle_ms = (uint16_t)settle;
        if (n >= 4 && interval > 0) cfg.interval_ms = (uint16_t)interval;
        if (n >= 5 && samples > 0) cfg.samples = (uint8_t)samples;
        if (n >= 6) cfg.retries = (uint8_t)(retries < 0 ? 0 : retries);
        if (n >= 7) cfg.apply_pid = apply != 0;
        esp_err_t err = motor_bemf_cal_start_ex(&cfg);
        send_line(err == ESP_OK ? "BEMF-CAL-OK"
                                : (err == ESP_ERR_INVALID_STATE ? "BEMF-ERR busy"
                                                                : "BEMF-ERR start"));
        return;
    }
    if (strcmp(line, "BEMF-CLR") == 0) {
        esp_err_t err = motor_bemf_cal_clear();
        send_line(err == ESP_OK ? "BEMF-CLR-OK" : "BEMF-ERR busy");
        return;
    }
    if (strncmp(line, "BEMF-CVSET ", 11) == 0) {
        int idx = -1, val = -1;
        (void)sscanf(line, "BEMF-CVSET %d %d", &idx, &val);
        if (idx < 1 || idx > 512 || val < 0 || val > 255) {
            send_line("CVSET-ERR range");
            return;
        }
        (void)settings_cv_write((uint16_t)idx, (uint8_t)val);
        (void)settings_cv_commit();
        char ok[40];
        snprintf(ok, sizeof(ok), "CVSET-OK %d=%d", idx, val);
        send_line(ok);
        return;
    }
    if (strncmp(line, "BEMF-TEST", 9) == 0) {
        int spd = -1;
        bool fwd = true;
        char tail[32] = { 0 };
        (void)sscanf(line, "BEMF-TEST %d %31s", &spd, tail);
        if (tail[0] == 'r' || tail[0] == 'R') {
            fwd = false;
        }
        if (spd < 0 || spd > 126) {
            send_line("BEMF-ERR speed");
            return;
        }
        (void)motor_set_speed((uint8_t)spd, fwd);
        char ok[40];
        snprintf(ok, sizeof(ok), "BEMF-TEST-OK %u %s", (unsigned)spd, fwd ? "fwd" : "rev");
        send_line(ok);
        return;
    }
    if (strncmp(line, "BEMF=", 5) == 0) {
        settings_bemf_cal_t cal;
        memset(&cal, 0, sizeof(cal));
        const char *p = line + 5;
        uint8_t n = 0;
        while (*p != '\0' && n < SETTINGS_BEMF_CAL_MAX_POINTS) {
            char *end = NULL;
            long spd = strtol(p, &end, 10);
            if (end == p) {
                break;
            }
            p = (*end == ',') ? end + 1 : end;
            long frac = strtol(p, &end, 10);
            if (end == p) {
                break;
            }
            p = (*end == ',') ? end + 1 : end;
            if (spd < 0 || spd > 126 || frac < 0 || frac > 1024) {
                send_line("BEMF-ERR range");
                return;
            }
            cal.speed[n] = (uint8_t)spd;
            cal.frac[n] = (uint16_t)frac;
            n++;
        }
        if (n < 2) {
            send_line("BEMF-ERR parse");
            return;
        }
        cal.count = n;
        if (settings_bemf_cal_save(&cal) != ESP_OK) {
            send_line("BEMF-ERR save");
            return;
        }
        (void)motor_bemf_cal_reload();
        char ok[32];
        snprintf(ok, sizeof(ok), "BEMF-OK %u", (unsigned)n);
        send_line(ok);
        return;
    }
    send_line("BEMF-ERR unknown");
}

/* ---- provisioning proper ---------------------------------------------- */

/* Receive a new application image over the provisioning link and write it to the
 * next OTA slot (esp_ota). Command: "FW <size>" -> "FW-OK", data chunks with
 * "CHUNK" acks, then "FW-DONE". The new image is set as the boot partition and
 * will be booted after "DONE" (esp_restart). */
static esp_err_t provision_fw(const char *cmd)
{
    long size = 0;
    if (sscanf(cmd, "FW %ld", &size) < 1 || size <= 0) {
        send_line("FW-ERR parse");
        return ESP_ERR_INVALID_ARG;
    }
    if (size > (4 * 1024 * 1024)) {
        send_line("FW-ERR size");
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (update == NULL) {
        send_line("FW-ERR nopart");
        return ESP_ERR_NOT_FOUND;
    }
    if ((uint32_t)size > update->size) {
        send_line("FW-ERR big");
        return ESP_ERR_INVALID_SIZE;
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "provision: FW begin %s", esp_err_to_name(err));
        send_line("FW-ERR begin");
        return err;
    }
    send_line("FW-OK");

    uint8_t *buf = (uint8_t *)malloc(PROV_CHUNK);
    if (buf == NULL) {
        (void)esp_ota_abort(handle);
        send_line("FW-ERR mem");
        return ESP_ERR_NO_MEM;
    }

    long remaining = size;
    while (remaining > 0L) {
        size_t want = (size_t)(remaining > (long)PROV_CHUNK ? (long)PROV_CHUNK : remaining);
        if (!read_exact(buf, want, 60000)) {
            ESP_LOGE(TAG, "provision: FW data timeout");
            free(buf);
            (void)esp_ota_abort(handle);
            send_line("FW-ERR timeout");
            return ESP_ERR_TIMEOUT;
        }
        err = esp_ota_write(handle, buf, want);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "provision: FW write %s", esp_err_to_name(err));
            free(buf);
            (void)esp_ota_abort(handle);
            send_line("FW-ERR write");
            return err;
        }
        remaining -= (long)want;
        if (remaining > 0L) {
            send_line("CHUNK");
        }
    }
    free(buf);

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "provision: FW end %s", esp_err_to_name(err));
        send_line("FW-ERR end");
        return err;
    }
    s_pending_boot = update;
    ESP_LOGW(TAG, "Firmware OTA received (%ld bytes), boot selection deferred to DONE", size);
    return ESP_OK;
}

/* Parse a complete .asp stream and write every payload as an individual WAV.
 * During PACK, read_exact() is redirected to pack_stream_read(). */
static esp_err_t provision_pack_parse(const char *cmd)
{
    long size = 0;
    if (sscanf(cmd, "PACK %ld", &size) < 1 || size < (long)AUDIO_PACK_HDR_LEN) {
        send_line("PACK-ERR parse");
        return ESP_ERR_INVALID_ARG;
    }
    if (size > (long)PROV_PACK_MAX) {
        send_line("PACK-ERR size");
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t hdr[AUDIO_PACK_HDR_LEN];
    audio_pack_header_t ph;
    audio_pack_entry_t *entries = NULL;
    uint8_t *buf = NULL;
    esp_err_t err = ESP_OK;

    if (!read_exact(hdr, sizeof(hdr), 60000)) {
        send_line("PACK-ERR timeout");
        return ESP_ERR_TIMEOUT;
    }
    if (!audio_pack_header_parse(hdr, sizeof(hdr), &ph) ||
        ph.data_off > (uint32_t)size) {
        send_line("PACK-ERR header");
        return ESP_ERR_INVALID_ARG;
    }

    entries = (audio_pack_entry_t *)calloc(ph.count, sizeof(*entries));
    buf = (uint8_t *)malloc(PROV_PACK_CHUNK);
    if (entries == NULL || buf == NULL) {
        send_line("PACK-ERR mem");
        err = ESP_ERR_NO_MEM;
        goto done;
    }

    uint64_t pos = AUDIO_PACK_HDR_LEN;
    for (uint32_t i = 0; i < ph.count; ++i) {
        uint8_t eh[AUDIO_PACK_ENTRY_HDR_LEN];
        uint8_t nlen = 0;
        uint8_t name[AUDIO_PACK_NAME_MAX];
        if (!read_exact(eh, sizeof(eh), 60000)) {
            send_line("PACK-ERR timeout");
            err = ESP_ERR_TIMEOUT;
            goto done;
        }
        pos += sizeof(eh);
        if (!audio_pack_entry_head(eh, (uint64_t)size, &entries[i], &nlen)) {
            send_line("PACK-ERR index");
            err = ESP_ERR_INVALID_ARG;
            goto done;
        }
        if (!read_exact(name, nlen, 60000)) {
            send_line("PACK-ERR timeout");
            err = ESP_ERR_TIMEOUT;
            goto done;
        }
        pos += nlen;
        if (!audio_pack_entry_name(&entries[i], name, nlen)) {
            send_line("PACK-ERR name");
            err = ESP_ERR_INVALID_ARG;
            goto done;
        }
    }
    if (pos > ph.data_off) {
        send_line("PACK-ERR index");
        err = ESP_ERR_INVALID_ARG;
        goto done;
    }
    while (pos < ph.data_off) {
        size_t want = (size_t)(ph.data_off - pos > PROV_PACK_CHUNK ? PROV_PACK_CHUNK : ph.data_off - pos);
        if (!read_exact(buf, want, 60000)) {
            send_line("PACK-ERR timeout");
            err = ESP_ERR_TIMEOUT;
            goto done;
        }
        pos += want;
    }

    unsigned wav_count = 0;
    for (uint32_t i = 0; i < ph.count; ++i) {
        audio_pack_entry_t *e = &entries[i];
        if ((uint64_t)e->data_off < pos) {
            send_line("PACK-ERR order");
            err = ESP_ERR_INVALID_ARG;
            goto done;
        }
        while (pos < e->data_off) {
            size_t want = (size_t)(e->data_off - pos > PROV_PACK_CHUNK ? PROV_PACK_CHUNK : e->data_off - pos);
            if (!read_exact(buf, want, 60000)) {
                send_line("PACK-ERR timeout");
                err = ESP_ERR_TIMEOUT;
                goto done;
            }
            pos += want;
        }

        if (e->bits == AUDIO_PACK_BITS_SCHEME) {
            /* The serial pack path installs sounds only; an embedded scheme is
             * skipped (use the web pack upload if a scheme is required). */
            uint32_t rem = e->data_len;
            while (rem > 0U) {
                size_t want = rem > PROV_PACK_CHUNK ? PROV_PACK_CHUNK : rem;
                if (!read_exact(buf, want, 60000)) {
                    send_line("PACK-ERR timeout");
                    err = ESP_ERR_TIMEOUT;
                    goto done;
                }
                pos += want;
                rem -= (uint32_t)want;
            }
            continue;
        }

        char path[128];
        snprintf(path, sizeof(path), PROV_AUDIO_DIR "/%s.wav", e->name);
        FILE *f = fopen(path, "wb");
        if (f == NULL) {
            ESP_LOGE(TAG, "provision: cannot create %s", path);
            send_line("PACK-ERR create");
            err = ESP_FAIL;
            goto done;
        }

        uint8_t wh[AUDIO_PACK_WAV_HDR_MAX];
        size_t whl = audio_pack_wav_header(wh, sizeof(wh), e->data_len,
                                           e->sample_rate, e->channels, e->bits);
        bool written = whl > 0U && PROV_FWRITE(wh, 1, whl, f) == whl;
        uint32_t rem = e->data_len;
        while (written && rem > 0U) {
            size_t want = rem > PROV_PACK_CHUNK ? PROV_PACK_CHUNK : rem;
            if (!read_exact(buf, want, 60000)) {
                send_line("PACK-ERR timeout");
                err = ESP_ERR_TIMEOUT;
                written = false;
                break;
            }
            if (PROV_FWRITE(buf, 1, want, f) != want) {
                send_line("PACK-ERR write");
                err = ESP_FAIL;
                written = false;
                break;
            }
            pos += want;
            rem -= (uint32_t)want;
        }
        /* No per-file fflush/fsync: 300 files would add minutes. fclose
         * flushes and closes the file; a failed pack can be uploaded again. */
        if (fclose(f) != 0) {
            written = false;
        }
        if (!written) {
            (void)remove(path);
            if (err == ESP_OK) {
                send_line("PACK-ERR write");
                err = ESP_FAIL;
            }
            goto done;
        }
        ++wav_count;
    }

    /* Consume any trailing bytes so the next read_line starts on a command. */
    while (pos < (uint64_t)size) {
        size_t want = (size_t)((uint64_t)size - pos > PROV_PACK_CHUNK ? PROV_PACK_CHUNK : (uint64_t)size - pos);
        if (!read_exact(buf, want, 60000)) {
            send_line("PACK-ERR timeout");
            err = ESP_ERR_TIMEOUT;
            goto done;
        }
        pos += want;
    }

    char ok[48];
    snprintf(ok, sizeof(ok), "PACK-OK %u", wav_count);
    send_line(ok);
    ESP_LOGW(TAG, "Pack received (%ld bytes, %u WAV files)", size, wav_count);

done:
    free(buf);
    free(entries);
    return err;
}

/* Receive one complete AURA Sound Pack (.asp) as framed PACK-DATA packets and
 * unpack it directly into /userdata/audio. A frame is acknowledged only after
 * the parser has consumed it, so the USB/UART RX buffers cannot overflow while
 * the pack is written once (no temporary pack copy). */
static esp_err_t provision_pack(const char *cmd)
{
    long size = 0;
    if (sscanf(cmd, "PACK %ld", &size) < 1 || size < (long)AUDIO_PACK_HDR_LEN) {
        send_line("PACK-ERR parse");
        return ESP_ERR_INVALID_ARG;
    }
    if (size > (long)PROV_PACK_MAX) {
        send_line("PACK-ERR size");
        return ESP_ERR_INVALID_SIZE;
    }

    s_pack_frame = (uint8_t *)malloc(PROV_PACK_FRAME);
    if (s_pack_frame == NULL) {
        send_line("PACK-ERR mem");
        return ESP_ERR_NO_MEM;
    }
    s_pack_size = (uint32_t)size;
    s_pack_received = 0;
    s_pack_frame_len = 0;
    s_pack_frame_pos = 0;
    s_pack_stream_active = true;
    send_line("PACK-READY");

    esp_err_t err = provision_pack_parse(cmd);
    s_pack_stream_active = false;
    if (err == ESP_OK &&
        (s_pack_received != s_pack_size || s_pack_frame_pos != s_pack_frame_len)) {
        send_line("PACK-ERR size");
        err = ESP_ERR_INVALID_SIZE;
    }
    free(s_pack_frame);
    s_pack_frame = NULL;
    s_pack_frame_len = 0;
    s_pack_frame_pos = 0;
    return err;
}

/* Returns true only when the session completed and the restart was issued. */
static bool provision_transfer(bool *maintenance_started)
{
    /* Destructive step ahead (erase + format of the external NOR): require an
     * explicit confirmation so a stray "PROV" line cannot wipe the sounds. */
    send_line("PROV-CONFIRM?");
    char confirm[32];
    bool have_confirm = read_line(confirm, sizeof(confirm), PROV_CONFIRM_TIMEOUT_MS);
    if (have_confirm) {
        /* The host may terminate the line with CRLF (or CR): strip a trailing
         * CR before the exact comparison, like the listener does when it reads
         * a command line. */
        size_t clen = strlen(confirm);
        if (clen > 0U && confirm[clen - 1U] == '\r') {
            confirm[clen - 1U] = '\0';
        }
    }
    if (!have_confirm || strcmp(confirm, "PROV-CONFIRM") != 0) {
        ESP_LOGW(TAG, "provision: no confirmation, aborting (nothing erased)");
        send_line("PROV-ABORT");
        return false;
    }

    esp_err_t err = web_maintenance_begin(true);
    if (err != ESP_OK) {
        send_line("PROV-ERR busy");
        return false;
    }
    *maintenance_started = true;
    s_pending_boot = NULL;
    send_line("PROV-OK");

    /* Both sides switch to the fast baud for the bulk transfer. */
    uart_flush_input(UART_NUM_0);
    (void)uart_set_baudrate(UART_NUM_0, PROV_BAUD);

    /* Admission is closed and all file holders acknowledged quiescence. */
    err = storage_format();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "provision: external NOR unavailable, aborting");
        send_line("PROV-ERR storage");
        return false;
    }
    (void)PROV_MKDIR(PROV_AUDIO_DIR);

    settings_track_t tracks[SETTINGS_MAX_TRACKS];
    memset(tracks, 0, sizeof(tracks));
    size_t tcount = 0;

    for (;;) {
        char cmd[96];
        if (!read_line(cmd, sizeof(cmd), 120000)) {
            ESP_LOGE(TAG, "provision: command timeout");
            return false;
        }
        if (strncmp(cmd, "DONE", 4) == 0) {
            break;
        }
        if (strncmp(cmd, "PACK ", 5) == 0) {
            esp_err_t perr = provision_pack(cmd);
            if (perr != ESP_OK) {
                ESP_LOGE(TAG, "provision: PACK failed (%s), aborting", esp_err_to_name(perr));
                return false;
            }
            continue;
        }
        if (strncmp(cmd, "FW ", 3) == 0) {
            esp_err_t ferr = provision_fw(cmd);
            if (ferr != ESP_OK) {
                ESP_LOGE(TAG, "provision: FW failed (%s), aborting", esp_err_to_name(ferr));
                return false;
            }
            send_line("FW-DONE");
            continue;
        }
        if (strncmp(cmd, "PUT", 3) != 0) {
            continue;
        }

        int slot = 0;
        long size = 0;
        char label[SETTINGS_TRACK_LABEL_MAX] = { 0 };
        if (sscanf(cmd, "PUT %d %ld %63[^\n]", &slot, &size, label) < 2) {
            continue;
        }
        if (slot < 1 || slot > SETTINGS_MAX_TRACKS || size < 0L || size > (4 * 1024 * 1024)) {
            ESP_LOGE(TAG, "provision: bad PUT %s", cmd);
            return false;
        }

        char path[64];
        snprintf(path, sizeof(path), PROV_AUDIO_DIR "/slot%d.wav", slot);
        FILE *f = fopen(path, "wb");
        if (f == NULL) {
            ESP_LOGE(TAG, "provision: cannot create %s", path);
            return false;
        }

        uint8_t *buf = (uint8_t *)malloc(PROV_CHUNK);
        if (buf == NULL) {
            fclose(f);
            return false;
        }

        long remaining = size;
        send_line("PUT-OK");
        while (remaining > 0L) {
            size_t want = (size_t)(remaining > (long)PROV_CHUNK ? (long)PROV_CHUNK : remaining);
            if (!read_exact(buf, want, 60000)) {
                ESP_LOGE(TAG, "provision: data timeout for slot %d", slot);
                free(buf);
                fclose(f);
                return false;
            }
            if (PROV_FWRITE(buf, 1, want, f) != want) {
                ESP_LOGE(TAG, "provision: write failed for slot %d", slot);
                free(buf);
                fclose(f);
                return false;
            }
            remaining -= (long)want;
            if (remaining > 0L) {
                send_line("CHUNK");
            }
        }
        free(buf);
        bool written = fflush(f) == 0 && PROV_FSYNC(f) == 0;
        if (fclose(f) != 0) { written = false; }
        if (!written) {
            send_line("PROV-ERR write");
            return false;
        }

        if (tcount < SETTINGS_MAX_TRACKS) {
            tracks[tcount].slot = (uint8_t)slot;
            snprintf(tracks[tcount].file, sizeof(tracks[tcount].file),
                     "audio/slot%d.wav", slot);
            snprintf(tracks[tcount].label, sizeof(tracks[tcount].label),
                     "%s", label[0] != '\0' ? label : "Слот");
            tracks[tcount].enabled = true;
            tcount++;
        }
        send_line("FILE-OK");
    }

    /* Formatting is finished. Reopen filesystem leases for validation and
     * metadata backup, while command/audio admission stays closed. */
    storage_maintenance_end();
    for (size_t i = 0; i < tcount; ++i) {
        char path[160];
        snprintf(path, sizeof(path), "%s/%s", storage_get_root(), tracks[i].file);
        if (audio_validate_wav(path) != ESP_OK) {
            send_line("PROV-ERR wav");
            return false;
        }
    }
    err = settings_tracks_save(tracks, tcount);
    if (err != ESP_OK) {
        send_line("PROV-ERR metadata");
        return false;
    }
    if (s_pending_boot != NULL && esp_ota_set_boot_partition(s_pending_boot) != ESP_OK) {
        send_line("FW-ERR boot");
        return false;
    }
    send_line("DONE-OK");
    ESP_LOGW(TAG, "Provisioning complete (%u tracks), restarting", (unsigned)tcount);

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return true;
}

static bool provision_run(void)
{
    bool maintenance_started = false;
    bool result = provision_transfer(&maintenance_started);
    if (maintenance_started) {
        web_maintenance_end();
        (void)uart_set_baudrate(UART_NUM_0, 115200);
    }
    s_pending_boot = NULL;
    s_provisioning = false;
    return result;
}

bool provision_try(void)
{
    /* Only enter provisioning when the background listener requested it. */
    if (!get_prov_flag()) {
        return false;
    }
    clear_prov_flag();
    s_provisioning = true;
    ensure_uart_driver();
    ensure_usbjtag_driver();

    ESP_LOGW(TAG, "PROV-WINDOW open: waiting for PROV");
    char cmd[96];
    if (!read_line(cmd, sizeof(cmd), PROV_WINDOW_MS) || strncmp(cmd, "PROV", 4) != 0) {
        ESP_LOGW(TAG, "provision: no PROV in window");
        s_provisioning = false;
        return false;
    }
    return provision_run();
}

/* ---- background listener ---------------------------------------------- */

/* Test hook: 0 runs forever (production); host tests set a small cap. */
static uint32_t s_listen_iter_cap;

/* Print a machine-readable self-test report:
 *   SELFTEST-BEGIN
 *   TEST <name> <PASS|FAIL|SKIP>
 *   SELFTEST-END <passed>/<total>
 * test/hil/run_hil.ps1 parses these lines. The checks are non-destructive. */
static void run_selftest_console(void)
{
    selftest_report_t rep;
    uint8_t passed = selftest_run(&rep);
    send_line("SELFTEST-BEGIN");
    for (uint8_t i = 0; i < rep.count; ++i) {
        char out[48];
        snprintf(out, sizeof(out), "TEST %s %s", rep.items[i].name,
                 selftest_state_name(rep.items[i].state));
        send_line(out);
    }
    char end[48];
    snprintf(end, sizeof(end), "SELFTEST-END %u/%u",
             (unsigned)passed, (unsigned)rep.count);
    send_line(end);
}

/* Actuating HIL commands (bounded; auto-restore/stop):
 *   HIL-AUX <ch> <ms>      turn AUX ch on for ms, then restore its state
 *   HIL-SOUND <slot> <ms>  play sound slot for ms, then stop
 *   HIL-MOTOR <spd> <ms>   drive the motor at spd for ms, then stop
 * Replies HIL-*-OK / HIL-*-ERR. Only reachable from an explicit serial line;
 * test/hil/run_hil.ps1 sends them only with -Actuate / -Motor. */
static void run_hil_act_console(const char *line)
{
    char out[64];
    int a = 0, b = 0;
    if (strncmp(line, "HIL-AUX-SWEEP ", 14) == 0 && sscanf(line, "HIL-AUX-SWEEP %d", &a) == 1) {
        uint8_t n = selftest_act_aux_sweep((uint16_t)a);
        snprintf(out, sizeof(out), n ? "HIL-AUX-SWEEP-OK %u" : "HIL-AUX-SWEEP-ERR", n);
    } else if (strncmp(line, "HIL-AUX ", 8) == 0 && sscanf(line, "HIL-AUX %d %d", &a, &b) == 2) {
        esp_err_t err = selftest_act_aux((uint8_t)a, (uint16_t)b);
        snprintf(out, sizeof(out), err == ESP_OK ? "HIL-AUX-OK %d %d" : "HIL-AUX-ERR %d", a, b);
    } else if (strncmp(line, "HIL-FN-SWEEP ", 13) == 0 && sscanf(line, "HIL-FN-SWEEP %d", &a) == 1) {
        uint8_t n = selftest_act_fn_sweep((uint16_t)a);
        snprintf(out, sizeof(out), n ? "HIL-FN-SWEEP-OK %u" : "HIL-FN-SWEEP-ERR", n);
    } else if (strncmp(line, "HIL-FN ", 7) == 0 && sscanf(line, "HIL-FN %d %d", &a, &b) == 2) {
        esp_err_t err = selftest_act_function((uint8_t)a, b != 0);
        snprintf(out, sizeof(out), err == ESP_OK ? "HIL-FN-OK %d %d" : "HIL-FN-ERR %d", a, b);
    } else if (strncmp(line, "HIL-SOUND ", 10) == 0 && sscanf(line, "HIL-SOUND %d %d", &a, &b) == 2) {
        esp_err_t err = selftest_act_sound((uint8_t)a, (uint16_t)b);
        if (err == ESP_OK) {
            snprintf(out, sizeof(out), "HIL-SOUND-OK %d %d", a, b);
        } else {
            snprintf(out, sizeof(out), "HIL-SOUND-ERR %d %s", a,
                     (err == ESP_ERR_NOT_FOUND) ? "no-track" : "bad-arg");
        }
    } else if (strncmp(line, "HIL-MOTOR ", 10) == 0 && sscanf(line, "HIL-MOTOR %d %d", &a, &b) == 2) {
        /* Range-check before the uint8_t cast: 300 must not wrap to 44. */
        if (a < 1 || a > 126 || b < 0) {
            snprintf(out, sizeof(out), "HIL-MOTOR-ERR %d", a);
        } else {
            esp_err_t err = selftest_act_motor((uint8_t)a, (uint16_t)b);
            snprintf(out, sizeof(out), err == ESP_OK ? "HIL-MOTOR-OK %d %d" : "HIL-MOTOR-ERR %d", a, b);
        }
    } else if (strncmp(line, "HIL-ENGINE ", 11) == 0 && sscanf(line, "HIL-ENGINE %d", &a) == 1) {
        sound_engine_power(a != 0);
        snprintf(out, sizeof(out), "HIL-ENGINE-OK %d", a != 0 ? 1 : 0);
    } else if (strncmp(line, "HIL-SCHEME-SPEED ", 17) == 0 &&
               sscanf(line, "HIL-SCHEME-SPEED %d %d", &a, &b) == 2) {
        if (a < 0 || a > 255) {
            snprintf(out, sizeof(out), "HIL-SCHEME-SPEED-ERR %d", a);
        } else {
            sound_set_speed((uint8_t)a, b != 0);
            snprintf(out, sizeof(out), "HIL-SCHEME-SPEED-OK %d %d", a, b);
        }
    } else {
        snprintf(out, sizeof(out), "HIL-ERR");
    }
    send_line(out);
}

static void listener_task(void *arg)
{
    (void)arg;
    char line[256];
    size_t len = 0;
    uint32_t iters = 0;
    while (s_listen_iter_cap == 0U || iters < s_listen_iter_cap) {
        uint8_t c;
        int64_t deadline = esp_timer_get_time() + 1000000;
        if (read_byte_any(&c, deadline)) {
            if (c == '\n') {
                line[len] = '\0';
                if (!s_provisioning) {
                    if (strncmp(line, "PROV", 4) == 0) {
                        ESP_LOGW(TAG, "provision requested, starting...");
                        s_provisioning = true;
                        /* Run provisioning in-place (no reboot: a reboot would
                         * drop the native USB-Serial-JTAG link and the host port
                         * mid-protocol). The task has a big stack for this. */
                        provision_run();
                        s_provisioning = false;
                    } else if (strncmp(line, "BEMF", 4) == 0) {
                        handle_bemf_cmd(line);
                    } else if (strncmp(line, "SELFTEST", 8) == 0) {
                        run_selftest_console();
                    } else if (strncmp(line, "HIL-", 4) == 0) {
                        run_hil_act_console(line);
                    }
                }
                len = 0;
            } else if (len < sizeof(line) - 1 && c != '\r') {
                line[len++] = (char)c;
            }
        }
        iters++;
    }
}

void provision_listener_start(void)
{
    ensure_uart_driver();
    ensure_usbjtag_driver();
    if (xTaskCreate(listener_task, "prov_listen", 8192, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "listener task create failed");
    }
    ESP_LOGI(TAG, "Provisioning listener started (send PROV over UART/USB to provision)");
}
