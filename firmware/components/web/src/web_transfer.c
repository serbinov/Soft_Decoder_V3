/* Single asynchronous transfer worker, progress endpoint and deadline handling.
 * Owns upload progress, the storage lease and the worker supervisor. */

#include "web.h"
#include "web_internal.h"
#include "web_util.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "storage.h"

static const char *TAG_TRANSFER = "web";

static httpd_handle_t s_progress_server;
static volatile int s_up_total;
static volatile int s_up_received;
static volatile bool s_up_active;
static volatile bool s_up_ota;
static volatile uint8_t s_up_slot;
static TaskHandle_t s_transfer_supervisor;
static volatile bool s_up_aborted;
static bool s_up_lease_held;
static bool s_transfer_wdt_subscribed;
static uint32_t s_pipe_iter_cap;
static bool s_pipe_write_err_inject;
static int64_t s_transfer_deadline;
static bool s_transfer_reboot;
static uint32_t s_file_sequence;

static void transfer_supervisor_task(void *arg);
void web_transfer_wdt_feed(size_t bytes);

int64_t web_transfer_deadline_get(void)
{
    web_control_state_enter();
    int64_t value = s_transfer_deadline;
    web_control_state_exit();
    return value;
}

void web_transfer_deadline_set(int64_t value)
{
    web_control_state_enter();
    s_transfer_deadline = value;
    web_control_state_exit();
}

uint32_t web_transfer_next_sequence(void)
{
    web_control_state_enter();
    uint32_t value = ++s_file_sequence;
    web_control_state_exit();
    return value;
}

void web_transfer_progress_begin(int total, bool ota, uint8_t slot)
{
    web_control_state_enter();
    s_up_total = total;
    s_up_received = 0;
    s_up_active = true;
    s_up_ota = ota;
    s_up_slot = slot;
    web_control_state_exit();
}

void web_transfer_progress_slot(uint8_t slot)
{
    web_control_state_enter();
    s_up_slot = slot;
    web_control_state_exit();
}

void web_transfer_progress_bytes(int received)
{
    s_up_received = received;
}

void web_transfer_progress_end(void)
{
    s_up_active = false;
}

void web_transfer_reboot_set(bool reboot)
{
    web_control_state_enter();
    s_transfer_reboot = reboot;
    web_control_state_exit();
}

esp_err_t web_transfer_start_supervisor(void)
{
    if (s_transfer_supervisor != NULL) return ESP_OK;
    if (xTaskCreate(transfer_supervisor_task, "web_tx_sup", 3072, NULL, 4,
                    &s_transfer_supervisor) != pdPASS) {
        s_transfer_supervisor = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

typedef struct {
    httpd_req_t *req;
    esp_err_t (*handler)(httpd_req_t *);
} transfer_job_t;

bool web_transfer_expired(void)
{
    int64_t deadline = web_transfer_deadline_get();
    return deadline != 0 && esp_timer_get_time() >= deadline;
}

/* OTA uploads range from a small app image to the full 8 MiB limit. A fixed
 * 120 s window rejects a slow-but-healthy link halfway through, so derive a
 * budget from the declared body length at a conservative 16 KiB/s, with a
 * 300 s floor. */
int64_t web_transfer_ota_deadline(int content_len)
{
    const int64_t min_us = 300000000LL;
    if (content_len <= 0) return esp_timer_get_time() + min_us;
    int64_t seconds = ((int64_t)content_len + 16383LL) / 16384LL;
    int64_t budget = seconds * 1000000LL;
    if (budget < min_us) budget = min_us;
    return esp_timer_get_time() + budget;
}

esp_err_t web_transfer_pipe_upload(httpd_req_t *req, FILE *f, int *out_total)
{
    uint8_t *buf = malloc(PIPE_BUF_SIZE);
    if (buf == NULL) return ESP_ERR_NO_MEM;
    int64_t deadline = web_transfer_deadline_get();
    if (deadline == 0) deadline = esp_timer_get_time() + 120000000;
    int remaining = (int)req->content_len;
    int total = 0;
    int idle = 0;
    esp_err_t err = s_pipe_write_err_inject ? ESP_FAIL : ESP_OK;
    uint32_t next_prog = 0;
    uint32_t iterations = 0;
    while (err == ESP_OK && remaining > 0) {
        if (s_pipe_iter_cap != 0U && iterations++ >= s_pipe_iter_cap) { err = ESP_ERR_TIMEOUT; break; }
        if (web_transfer_aborted()) { err = ESP_ERR_TIMEOUT; break; }
        if (esp_timer_get_time() >= deadline) { err = ESP_ERR_TIMEOUT; break; }
        size_t want = remaining > PIPE_BUF_SIZE ? PIPE_BUF_SIZE : (size_t)remaining;
        int r = httpd_req_recv(req, (char *)buf, want);
        if (r > 0) {
            idle = 0;
            if ((size_t)r > want || WEB_FWRITE(buf, 1, (size_t)r, f) != (size_t)r) {
                err = ESP_FAIL; break;
            }
            total += r;
            remaining -= r;
            s_up_received = total;
            web_transfer_wdt_feed((size_t)r);
            if (total - (int)next_prog >= WEB_UP_PROGRESS_STEP) {
                next_prog = (uint32_t)total;
                ESP_LOGI(TAG_TRANSFER, "UP: progress %d/%d", total, (int)req->content_len);
            }
        } else if ((r == 0 || r == -3) && ++idle < 4) {
            ESP_LOGI(TAG_TRANSFER, "UP: recv idle #%d (got %d/%d)", idle, total, (int)req->content_len);
            continue;
        } else {
            ESP_LOGW(TAG_TRANSFER, "UP: recv failed (%d, got %d/%d)", r, total, (int)req->content_len);
            err = ESP_FAIL;
            break;
        }
    }

    *out_total = total;
    free(buf);
    return remaining == 0 && err == ESP_OK ? ESP_OK : err == ESP_OK ? ESP_FAIL : err;
}

/* GET /progress (port 81): read-only received-byte progress, CORS-enabled for
 * the cross-port fetch from the UI. No transfer or mutation admission here. */
static esp_err_t progress_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    char json[128];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"slot\":%u,\"ota\":%s,\"total\":%d,\"received\":%d,\"active\":%s}",
             (unsigned)s_up_slot, s_up_ota ? "true" : "false", s_up_total, s_up_received,
             s_up_active ? "true" : "false");
    return send_json(req, json);
}

esp_err_t web_transfer_start_progress_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 81;
    cfg.ctrl_port = 32769;
    cfg.stack_size = 4096;
    cfg.max_uri_handlers = 4;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 3;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;

    if (httpd_start(&s_progress_server, &cfg) != ESP_OK) {
        ESP_LOGW(TAG_TRANSFER, "progress httpd_start failed");
        return ESP_FAIL;
    }
    httpd_uri_t u = { .uri = "/progress", .method = HTTP_GET, .handler = progress_handler };
    httpd_register_uri_handler(s_progress_server, &u);
    ESP_LOGI(TAG_TRANSFER, "Progress server started on port 81");
    return ESP_OK;
}

bool web_transfer_aborted(void)
{
    web_control_state_enter();
    bool aborted = s_up_aborted;
    web_control_state_exit();
    return aborted;
}

/* Feed the TWDT only when a read/write actually moved bytes. Idle recv retries
 * and deadline polling must not hide a stalled peer. */
void web_transfer_wdt_feed(size_t bytes)
{
    if (bytes == 0U || !s_transfer_wdt_subscribed) return;
    (void)esp_task_wdt_reset();
}

/* Both the worker and the supervisor may race to release the lease after an
 * abort. Claim it under the control spinlock so it is released exactly once. */
static bool transfer_claim_lease(void)
{
    web_control_state_enter();
    bool held = s_up_lease_held;
    s_up_lease_held = false;
    web_control_state_exit();
    return held;
}

/* Timeout action: mark the transfer aborted. The worker owns its socket,
 * storage lease and maintenance, so it releases them after the current I/O
 * returns (all recv/send paths are bounded by the server timeouts). Touching
 * the async request, lease or maintenance from this task would race with a
 * worker that is completing normally. A worker that never returns is reset by
 * the task watchdog; leaving actuation inhibited is the fail-safe outcome.
 * Returns true when it aborted a live transfer. Exposed (static) so the host
 * test can simulate the supervisor while an upload worker is stalled. */
static bool transfer_supervisor_check(void)
{
    web_control_state_enter();
    bool active = s_up_active;
    bool already = s_up_aborted;
    int64_t deadline = s_transfer_deadline;
    if (!active || already || deadline == 0 || esp_timer_get_time() < deadline) {
        web_control_state_exit();
        return false;
    }
    s_up_aborted = true;
    web_control_state_exit();

    ESP_LOGW(TAG_TRANSFER, "transfer deadline exceeded; worker abort requested");
    return true;
}

static void transfer_supervisor_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        (void)transfer_supervisor_check();
    }
}

static void transfer_worker(void *arg)
{
    transfer_job_t *job = arg;
    web_control_set_maintenance_owner(xTaskGetCurrentTaskHandle());
    web_control_state_enter();
    s_up_aborted = false;
    s_up_lease_held = false;
    web_control_state_exit();
    s_transfer_wdt_subscribed = esp_task_wdt_add(NULL) == ESP_OK;
    if (!s_transfer_wdt_subscribed) {
        ESP_LOGW(TAG_TRANSFER, "transfer worker not subscribed to TWDT");
    }
    web_transfer_deadline_set(esp_timer_get_time() + 120000000);
    s_transfer_reboot = false;
    esp_err_t err = web_control_maintenance_quiesce(false);
    if (err == ESP_OK && !web_transfer_aborted()) {
        err = storage_access_begin();
        if (err == ESP_OK) {
            web_control_state_enter();
            s_up_lease_held = true;
            web_control_state_exit();
            /* The supervisor may have aborted between the lease acquisition and
             * publishing s_up_lease_held. */
            if (web_transfer_aborted()) {
                if (transfer_claim_lease()) storage_access_end();
                err = ESP_ERR_TIMEOUT;
            }
        }
    }
    bool handler_ran = false;
    if (err == ESP_OK && !web_transfer_aborted()) {
        httpd_resp_set_hdr(job->req, "Connection", "close");
        handler_ran = true;
        (void)job->handler(job->req);
    } else if (err == ESP_OK) {
        err = ESP_ERR_TIMEOUT;
    }
    if (!handler_ran) {
        (void)httpd_resp_send_err(job->req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                  web_transfer_aborted() ? "transfer aborted by watchdog"
                                                     : "transfer quiescence/admission failed");
    }
    if (transfer_claim_lease()) storage_access_end();
    s_up_active = false;
    web_transfer_deadline_set(0);
    int fd = httpd_req_to_sockfd(job->req);
    if (fd >= 0) (void)shutdown(fd, SHUT_RDWR);
    web_control_state_enter();
    s_up_aborted = false;
    web_control_state_exit();
    /* Complete wakes httpd to observe EOF and close the session. Do not queue
     * a later close by fd: the server could already have reused that number. */
    (void)httpd_req_async_handler_complete(job->req);
    free(job);
    if (s_transfer_wdt_subscribed) {
        (void)esp_task_wdt_delete(NULL);
        s_transfer_wdt_subscribed = false;
    }
    if (s_transfer_reboot) {
        /* Keep inhibit/admission closed across reboot. Filesystem and boot
         * selection cannot be one power-loss-atomic transaction. */
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    } else {
        web_maintenance_end();
    }
    vTaskDelete(NULL);
}

esp_err_t web_transfer_start(httpd_req_t *req, esp_err_t (*handler)(httpd_req_t *), bool is_ota)
{
    web_control_state_enter();
    /* An aborted worker may still be unwinding after the supervisor released
     * maintenance; do not admit a second upload over it. Normal concurrency is
     * already excluded by the maintenance gate. */
    bool busy = s_up_aborted;
    web_control_state_exit();
    if (busy) {
        return close_rejected_body(req,
            send_json(req, "{\"ok\":false,\"error\":\"transfer busy\"}"));
    }
    transfer_job_t *job = malloc(sizeof(*job));
    if (job == NULL) return close_rejected_body(req,
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no worker memory"));
    esp_err_t err = web_control_maintenance_reserve(false);
    if (err != ESP_OK) { free(job); return close_rejected_body(req,
        send_json(req, "{\"ok\":false,\"error\":\"maintenance busy\"}")); }
    err = httpd_req_async_handler_begin(req, &job->req);
    if (err != ESP_OK) {
        free(job); web_maintenance_end();
        return close_rejected_body(req,
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "async admission failed"));
    }
    job->handler = handler;
    s_up_active = true;
    s_up_total = (int)req->content_len;
    s_up_received = 0;
    s_up_slot = 0;
    s_up_ota = is_ota;
    if (xTaskCreate(transfer_worker, "web_transfer", 16384, job, 6, NULL) != pdPASS) {
        (void)httpd_resp_send_err(job->req, HTTPD_500_INTERNAL_SERVER_ERROR, "worker creation failed");
        int fd = httpd_req_to_sockfd(job->req);
        if (fd >= 0) (void)shutdown(fd, SHUT_RDWR);
        (void)httpd_req_async_handler_complete(job->req);
        free(job); s_up_active = false;
        web_maintenance_end();
    }
    return ESP_OK;
}
