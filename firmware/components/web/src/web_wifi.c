#include "web_internal.h"
#include "web_util.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef TAG
#undef TAG
#endif
#define TAG "web_wifi"
/* Radio power. The ESP32-S3 PA runs hot; 10 dBm covers a layout room and is
 * the default. 20 dBm is the chip maximum; 5 dBm is useful for a workbench. */
#define WIFI_TX_MIN_DBM       5U
#define WIFI_TX_MAX_DBM       20U
#define WIFI_TX_DEFAULT_DBM   10U
/* Free the single-client slot after this many idle seconds (default 300). */
#define WIFI_AP_INACTIVE_SEC 120
#define WIFI_STA_RETRY_US     (15LL * 1000000LL)
/* ---------- WiFi ---------- */

static bool s_wifi_started;
static char s_ap_ip[16];
/* Must stay valid for the whole DHCP server lifetime (option 114 copies it). */
static char s_captive_uri[32];
static uint8_t s_ap_ip_bytes[4];
static uint32_t s_autooff_iter_cap;
static uint32_t s_dns_iter_cap;
static uint8_t s_auto_off_min;
static volatile bool s_dns_run;
static volatile uint8_t s_ap_sta_count;
static int64_t s_ap_started_us;
static int64_t s_last_client_us; /* last connect/disconnect; auto-off base */
/* Optional upstream station (APSTA). The AP stays up, so losing the home
 * network never locks the user out of the decoder. */
static esp_netif_t *s_sta_netif;
static volatile bool s_sta_enabled;
static bool s_client_role;
static bool s_sta_fallback;
static bool s_ap_active;
static bool s_ap_fallback;
static esp_netif_t *s_ap_netif;
static wifi_config_t s_ap_cfg;
static char s_ap_ip_cfg[16];
static int64_t s_client_start_us;
static volatile bool s_sta_connected;
static volatile int s_sta_rssi = -127;
static char s_sta_ip[16];
static int64_t s_sta_last_try_us;
static bool s_autooff_started;
/* 64-bit timestamps are not atomic on the 32-bit target: guard the event-task
 * writes and auto-off reads with one spinlock. */
static portMUX_TYPE s_wifi_time_mux = portMUX_INITIALIZER_UNLOCKED;

static void wifi_note_client_now(void)
{
    portENTER_CRITICAL(&s_wifi_time_mux);
    s_last_client_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_wifi_time_mux);
}

static int64_t wifi_last_client_us(void)
{
    portENTER_CRITICAL(&s_wifi_time_mux);
    int64_t value = s_last_client_us;
    portEXIT_CRITICAL(&s_wifi_time_mux);
    return value;
}

static void wifi_note_sta_try_now(void)
{
    portENTER_CRITICAL(&s_wifi_time_mux);
    s_sta_last_try_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_wifi_time_mux);
}

static int64_t wifi_sta_last_try_us(void)
{
    portENTER_CRITICAL(&s_wifi_time_mux);
    int64_t value = s_sta_last_try_us;
    portEXIT_CRITICAL(&s_wifi_time_mux);
    return value;
}

/* qdBm is 0.25 dBm units. The driver accepts 8..84 (2..21 dBm); our UI range
 * is deliberately narrower and equals the persisted value. */
static esp_err_t wifi_apply_tx_power(uint8_t dbm)
{
    if (dbm < WIFI_TX_MIN_DBM || dbm > WIFI_TX_MAX_DBM) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = esp_wifi_set_max_tx_power((int8_t)((unsigned)dbm * 4U));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TX power %u dBm not applied: %s", (unsigned)dbm, esp_err_to_name(err));
    }
    return err;
}

/* Parse a dotted-quad IPv4 address. 0.0.0.0 is rejected (not usable as an AP
 * address). Returns false when the text cannot be parsed. */
bool parse_ip4(const char *s, esp_ip4_addr_t *out)
{
    if (s == NULL || s[0] == '\0' || out == NULL) {
        return false;
    }
    unsigned octets[4] = {0};
    const char *p = s;
    for (size_t i = 0; i < 4U; ++i) {
        const char *start = p;
        unsigned digits = 0;
        while (isdigit((unsigned char)*p)) {
            if (++digits > 3U) return false;
            octets[i] = octets[i] * 10U + (unsigned)(*p++ - '0');
        }
        if (digits == 0U || octets[i] > 255U || (digits > 1U && start[0] == '0')) return false;
        if (i < 3U) { if (*p++ != '.') return false; }
        else if (*p != '\0') return false;
    }
    if (octets[0] == 0U || octets[0] == 127U || octets[0] >= 224U ||
        octets[3] == 0U || octets[3] == 255U) return false;
    out->addr = ESP_IP4TOADDR(octets[0], octets[1], octets[2], octets[3]);
    return true;
}

/* Wi-Fi event log: without it a dropped/deauthenticated client is invisible,
 * which makes a link problem indistinguishable from an HTTP problem. */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_AP_START:
                ESP_LOGI(TAG, "AP started");
                break;
            case WIFI_EVENT_AP_STOP:
                ESP_LOGI(TAG, "AP stopped");
                break;
            case WIFI_EVENT_AP_STACONNECTED: {
                const wifi_event_ap_staconnected_t *e = data;
                if (s_ap_sta_count < 255U) {
                    s_ap_sta_count++;
                }
                wifi_note_client_now();
                ESP_LOGI(TAG, "STA %02x:%02x:%02x:%02x:%02x:%02x connected (aid=%d), clients=%u",
                         e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5],
                         (int)e->aid, (unsigned)s_ap_sta_count);
                break;
            }
            case WIFI_EVENT_AP_STADISCONNECTED: {
                const wifi_event_ap_stadisconnected_t *e = data;
                if (s_ap_sta_count > 0U) {
                    s_ap_sta_count--;
                }
                wifi_note_client_now();
                ESP_LOGW(TAG, "STA disconnected (aid=%d, reason=%d), clients=%u",
                         (int)e->aid, (int)e->reason, (unsigned)s_ap_sta_count);
                break;
            }
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "station started");
                break;
            case WIFI_EVENT_STA_CONNECTED: {
                const wifi_event_sta_connected_t *e = data;
                s_sta_connected = true;
                ESP_LOGI(TAG, "station connected to %.*s (channel %u)",
                         (int)e->ssid_len, (const char *)e->ssid, (unsigned)e->channel);
                break;
            }
            case WIFI_EVENT_STA_DISCONNECTED: {
                const wifi_event_sta_disconnected_t *e = data;
                s_sta_connected = false;
                s_sta_rssi = -127;
                s_sta_ip[0] = '\0';
                ESP_LOGW(TAG, "station disconnected (reason=%d)", (int)e->reason);
                /* The supervisor task retries when sta_enabled is set; doing it
                 * here would spin on a wrong password. */
                break;
            }
            default:
                break;
        }
    } else if (base == IP_EVENT) {
        switch (id) {
            case IP_EVENT_STA_GOT_IP: {
                const ip_event_got_ip_t *e = data;
                snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&e->ip_info.ip));
                s_sta_connected = true;
                wifi_ap_record_t ap = { 0 };
                if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                    s_sta_rssi = ap.rssi;
                }
                ESP_LOGI(TAG, "station got IP %s", s_sta_ip);
                break;
            }
            case IP_EVENT_STA_LOST_IP:
                s_sta_ip[0] = '\0';
                break;
            case IP_EVENT_ASSIGNED_IP_TO_CLIENT: {
                const ip_event_assigned_ip_to_client_t *e = data;
                ESP_LOGI(TAG, "Client got IP " IPSTR, IP2STR(&e->ip));
                break;
            }
            default:
                break;
        }
    }
}

/* The three non-overlapping 2.4 GHz channels. */
static const uint8_t s_wifi_channels[3] = { 1, 6, 11 };

/* Spectral overlap of a 20 MHz network on ap_ch with a 20 MHz AP candidate.
 * Adjacent channels still share most of the band; two channels away share
 * half of it, three channels away only the skirt. */
static int wifi_overlap_pct(int cand, int ap_ch)
{
    int d = cand > ap_ch ? cand - ap_ch : ap_ch - cand;
    switch (d) {
        case 0: return 100;
        case 1: return 60;
        case 2: return 25;
        case 3: return 8;
        default: return 0;
    }
}

/* Convert RSSI to an interference weight: a -40 dBm neighbour damages the
 * link far more than a -80 dBm one, so do not count them equally. */
static int wifi_rssi_weight(int rssi)
{
    if (rssi >= -50) return 100;
    if (rssi >= -60) return 80;
    if (rssi >= -70) return 55;
    if (rssi >= -80) return 30;
    if (rssi >= -90) return 10;
    return 0; /* far below the noise floor: ignore */
}

/* Add one 20 MHz interferer to all candidate scores. */
static void wifi_score_ap(int score[3], int ap_ch, int weight)
{
    if (ap_ch < 1 || ap_ch > 13) {
        return; /* 40 MHz extension outside the 2.4 GHz band */
    }
    for (int k = 0; k < 3; ++k) {
        score[k] += weight * wifi_overlap_pct((int)s_wifi_channels[k], ap_ch) / 100;
    }
}

/* Pick the least-crowded of the three non-overlapping 2.4 GHz channels
 * (1/6/11) so the AP does not have to share the crowded default channel 1.
 * Visible APs contribute by spectral overlap, RSSI and occupied bandwidth:
 * a strong 40 MHz AP on channel 1, for example, also buries channel 6.
 * Returns 1 (the historical default) on any scan problem. */
static uint8_t wifi_pick_channel(void)
{
    wifi_scan_config_t sc = { 0 };
    sc.show_hidden = true;
    /* Active probe scan. 40..100 ms per channel is a compromise between
     * catching every beacon/probe response and keeping the boot fast; the
     * earlier 20..60 ms scan regularly missed weaker neighbours and picked
     * a busier channel. */
    sc.scan_time.active.min = 40;
    sc.scan_time.active.max = 100;
    esp_err_t serr = esp_wifi_scan_start(&sc, true);
    if (serr != ESP_OK) {
        ESP_LOGW(TAG, "channel scan failed (%s), retrying", esp_err_to_name(serr));
        serr = esp_wifi_scan_start(&sc, true);
    }
    if (serr != ESP_OK) {
        return 1;
    }
    uint16_t num = 0;
    if (esp_wifi_scan_get_ap_num(&num) != ESP_OK || num == 0) {
        (void)esp_wifi_clear_ap_list(); /* free the scan result set */
        return 1;
    }
    if (num > 16U) {
        num = 16U; /* records are sorted by RSSI: the strongest ones matter */
    }
    wifi_ap_record_t recs[16];
    uint16_t got = num;
    if (esp_wifi_scan_get_ap_records(&got, recs) != ESP_OK) {
        (void)esp_wifi_clear_ap_list();
        return 1;
    }
    int score[3] = { 0, 0, 0 };
    for (uint16_t i = 0; i < got; ++i) {
        int weight = wifi_rssi_weight(recs[i].rssi);
        if (weight == 0) {
            continue;
        }
        int primary = recs[i].primary;
        wifi_score_ap(score, primary, weight);
        /* HT40 uses a second 20 MHz half four channels away. */
        if (recs[i].second == WIFI_SECOND_CHAN_ABOVE) {
            wifi_score_ap(score, primary + 4, weight);
        } else if (recs[i].second == WIFI_SECOND_CHAN_BELOW) {
            wifi_score_ap(score, primary - 4, weight);
        }
    }
    int best = 0;
    for (int k = 1; k < 3; ++k) {
        if (score[k] < score[best]) {
            best = k;
        }
    }
    ESP_LOGI(TAG, "channel scan: %u APs, interference %d/%d/%d -> channel %u",
             (unsigned)got, score[0], score[1], score[2],
             (unsigned)s_wifi_channels[best]);
    return s_wifi_channels[best];
}

/* Power save: stop the AP when no client has been connected for the configured
 * time (auto_off_min, 0 = disabled). The same task supervises the optional
 * upstream station link: reconnects after the home AP reboots and refreshes
 * RSSI for the UI. Auto-off is skipped while STA mode is enabled, otherwise
 * the decoder would lose its home-network link. */
static void wifi_auto_off_task(void *arg)
{
    (void)arg;
    uint32_t iters = 0;
    while (s_autooff_iter_cap == 0U || iters < s_autooff_iter_cap) {
        iters++;
        vTaskDelay(pdMS_TO_TICKS(10000));
        if (!s_wifi_started) {
            continue;
        }
        if (s_sta_enabled) {
            if (s_sta_connected) {
                wifi_ap_record_t ap = { 0 };
                if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                    s_sta_rssi = ap.rssi;
                }
            } else if (esp_timer_get_time() - wifi_sta_last_try_us() >=
                       WIFI_STA_RETRY_US) {
                wifi_note_sta_try_now();
                ESP_LOGI(TAG, "station reconnect attempt");
                (void)esp_wifi_connect();
            }
        }
        if (s_client_role) {
            if (!s_sta_connected && s_sta_fallback && !s_ap_active &&
                esp_timer_get_time() - s_client_start_us >= 30LL * 1000000LL) {
                esp_err_t ferr = web_wifi_enable_ap_fallback();
                if (ferr != ESP_OK) {
                    ESP_LOGW(TAG, "client fallback failed: %s", esp_err_to_name(ferr));
                }
            }
            continue; /* no AP auto-off while in client mode */
        }
        uint8_t minutes = s_auto_off_min;
        if (s_sta_enabled || minutes == 0U || s_ap_sta_count != 0U) {
            continue;
        }
        /* Time since the last client was seen (or since AP start when no one
         * ever connected), not since boot, so a client that was connected for
         * a long time does not cause an immediate off right after leaving. */
        if (esp_timer_get_time() - wifi_last_client_us() <
            (int64_t)minutes * 60LL * 1000000LL) {
            continue;
        }
        ESP_LOGW(TAG, "WiFi auto-off: no client for %u min, stopping AP",
                 (unsigned)minutes);
        (void)esp_wifi_stop();
        s_wifi_started = false;
        s_sta_connected = false;
        s_ap_active = false;
        s_ap_fallback = false;
        s_dns_run = false; /* let the DNS hijack task exit and free its socket */
        s_autooff_started = false;
        vTaskDelete(NULL);
    }
    s_autooff_started = false;
}

/* Configure the AP address/DNS/captive portal. Called before the AP radio is
 * started, and again when a client-mode boot falls back to the access point. */
static esp_err_t wifi_ap_netif_prepare(void)
{
    if (s_ap_netif == NULL) {
        return ESP_OK;
    }
    esp_ip4_addr_t ap_addr = { 0 };
    if (!parse_ip4(s_ap_ip_cfg, &ap_addr)) {
        (void)esp_netif_str_to_ip4(AP_IP_DEFAULT, &ap_addr);
    }
    memcpy(s_ap_ip_bytes, &ap_addr.addr, sizeof(s_ap_ip_bytes));
    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr = ap_addr.addr;
    ip.gw.addr = ap_addr.addr;
    ip.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0);
    esp_err_t err = esp_netif_dhcps_stop(s_ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) return err;
    err = esp_netif_set_ip_info(s_ap_netif, &ip);
    if (err != ESP_OK) return err;

    esp_netif_dns_info_t dns = { 0 };
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ap_addr.addr;
    err = esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns);
    if (err != ESP_OK) return err;

    snprintf(s_captive_uri, sizeof(s_captive_uri), "http://" IPSTR "/",
             IP2STR(&ap_addr));
    err = esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                                 ESP_NETIF_CAPTIVEPORTAL_URI,
                                 s_captive_uri, (uint32_t)strlen(s_captive_uri));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DHCP captive portal URI option failed: %s", esp_err_to_name(err));
    }
    snprintf(s_ap_ip, sizeof(s_ap_ip), "%s", s_ap_ip_cfg);
    return ESP_OK;
}

static esp_err_t wifi_ap_netif_start(void)
{
    if (s_ap_netif == NULL) {
        return ESP_OK;
    }
    esp_err_t err = esp_netif_dhcps_start(s_ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        return err;
    }
    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20);
    err = esp_wifi_set_inactive_time(WIFI_IF_AP, WIFI_AP_INACTIVE_SEC);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "inactive time not applied: %s", esp_err_to_name(err));
    }
    esp_netif_ip_info_t actual = { 0 };
    if (esp_netif_get_ip_info(s_ap_netif, &actual) == ESP_OK) {
        snprintf(s_ap_ip, sizeof(s_ap_ip), IPSTR, IP2STR(&actual.ip));
        memcpy(s_ap_ip_bytes, &actual.ip.addr, sizeof(s_ap_ip_bytes));
    }
    return ESP_OK;
}

/* Client mode starts with the AP radio off. If the upstream station cannot
 * connect and the user enabled the fallback, turn the AP on so the panel is
 * still reachable. */
esp_err_t web_wifi_enable_ap_fallback(void)
{
    if (!s_wifi_started || s_ap_active) {
        return ESP_OK;
    }
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_AP, &s_ap_cfg);
    if (err != ESP_OK) return err;
    err = wifi_ap_netif_prepare();
    if (err != ESP_OK) return err;
    err = wifi_ap_netif_start();
    if (err != ESP_OK) return err;
    s_ap_active = true;
    s_ap_fallback = true;
    s_ap_started_us = esp_timer_get_time();
    ESP_LOGW(TAG, "client fallback: access point enabled at %s", s_ap_ip);
    return ESP_OK;
}

bool web_wifi_ap_active(void)
{
    return s_ap_active;
}

void web_wifi_set_sta_fallback(bool enabled)
{
    s_sta_fallback = enabled;
}

esp_err_t wifi_start(const settings_config_t *cfg)
{
    if (cfg->wifi_mode == 0) {
        return ESP_OK;
    }

    s_auto_off_min = cfg->auto_off_min;
    s_client_role = cfg->wifi_role == 1U;
    s_sta_fallback = cfg->sta_fallback != 0U;
    s_ap_active = false;
    s_ap_fallback = false;
    s_client_start_us = esp_timer_get_time();
    /* Role AP: the upstream station stays disconnected even if credentials are
     * stored. Role client: join the home network; the AP is started only as an
     * explicit fallback when the user enables it. */
    s_sta_enabled = s_client_role && cfg->sta_enabled != 0U &&
                    cfg->sta_ssid[0] != '\0';
    s_sta_connected = false;
    s_sta_ip[0] = '\0';
    s_sta_rssi = -127;
    (void)nvs_flash_init();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    /* The station interface is always created: the boot scan uses it, and the
     * optional upstream link (APSTA) needs it at runtime. */
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (ap_netif == NULL) return ESP_ERR_NO_MEM;
    s_sta_netif = sta_netif;

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&wcfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    /* The config lives in our own NVS store; keep the driver from writing. */
    (void)esp_wifi_set_storage(WIFI_STORAGE_RAM);

    (void)esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &wifi_event_handler, NULL, NULL);
    (void)esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                              &wifi_event_handler, NULL, NULL);

    wifi_config_t ap = { 0 };
    const char *ssid = cfg->ap_ssid[0] ? cfg->ap_ssid : "ADDITIPUS AURA-X";
    size_t ssid_len = strlen(ssid);
    if (ssid_len > sizeof(ap.ap.ssid)) return ESP_ERR_INVALID_ARG;
    memcpy(ap.ap.ssid, ssid, ssid_len);
    ap.ap.ssid_len = (uint8_t)ssid_len;
    ap.ap.channel = 1;
    /* Single-controller access point: the decoder must not be driven from two
     * phones/PCs at once. A browser may still open several tabs over the one
     * association. */
    ap.ap.max_connection = 1;
    ap.ap.beacon_interval = 100;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    if (cfg->ap_password[0] != '\0' && strlen(cfg->ap_password) >= 8U) {
        size_t password_len = strlen(cfg->ap_password);
        if (password_len > sizeof(ap.ap.password)) return ESP_ERR_INVALID_ARG;
        if (password_len == 64U) {
            for (size_t i = 0; i < password_len; ++i)
                if (!isxdigit((unsigned char)cfg->ap_password[i])) return ESP_ERR_INVALID_ARG;
        }
        memcpy(ap.ap.password, cfg->ap_password, password_len);
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else if (cfg->ap_password[0] != '\0') {
        /* A short password would leave the AP open without the user noticing. */
        ESP_LOGW(TAG, "AP password ignored (<8 chars): access point stays open");
    }

    /* Choose the AP channel BEFORE the AP is started. Scanning needs the radio
     * up, so bring the station up briefly, scan, then stop it. Doing this
     * before the AP starts avoids switching the AP channel right after it came
     * up (which dropped clients that were already associating and delayed
     * their DHCP/IP). */
    if (sta_netif != NULL) {
        if (esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK && esp_wifi_start() == ESP_OK) {
            uint8_t ch = wifi_pick_channel();
            (void)esp_wifi_stop();
            if (ch != ap.ap.channel) {
                ap.ap.channel = ch;
                ESP_LOGI(TAG, "AP channel set to %u (least used of 1/6/11)", (unsigned)ch);
            }
        }
    }

    /* Role AP: start the access point. Role client: start only the station;
     * the AP is enabled later only if the fallback checkbox is set and the
     * upstream link does not come up. */
    /* AP role uses APSTA even without an upstream connection: the idle STA
     * interface is what makes network scanning work while the access point
     * stays up. Client role starts as STA-only (AP is a fallback). */
    err = esp_wifi_set_mode(s_client_role ? WIFI_MODE_STA : WIFI_MODE_APSTA);
    if (err != ESP_OK) return err;
    s_ap_cfg = ap;
    s_ap_netif = ap_netif;
    snprintf(s_ap_ip_cfg, sizeof(s_ap_ip_cfg), "%s",
             cfg->ap_ip[0] ? cfg->ap_ip : AP_IP_DEFAULT);
    if (!s_client_role) {
        err = esp_wifi_set_config(WIFI_IF_AP, &ap);
        if (err != ESP_OK) return err;
        err = wifi_ap_netif_prepare();
        if (err != ESP_OK) return err;
    }

    if (s_sta_enabled) {
        wifi_config_t sta = { 0 };
        size_t sta_ssid_len = strlen(cfg->sta_ssid);
        size_t sta_pass_len = strlen(cfg->sta_password);
        if (sta_ssid_len == 0U || sta_ssid_len > sizeof(sta.sta.ssid) ||
            sta_pass_len > sizeof(sta.sta.password)) {
            ESP_LOGW(TAG, "stored station credentials are invalid: STA disabled");
            s_sta_enabled = false;
        } else {
            memcpy(sta.sta.ssid, cfg->sta_ssid, sta_ssid_len);
            memcpy(sta.sta.password, cfg->sta_password, sta_pass_len);
            /* Accept an open upstream network; WPA2/3 still negotiates. */
            sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
            esp_err_t serr = esp_wifi_set_config(WIFI_IF_STA, &sta);
            if (serr != ESP_OK) {
                ESP_LOGW(TAG, "station config failed (%s): STA disabled",
                         esp_err_to_name(serr));
                s_sta_enabled = false;
            }
        }
    }

    err = esp_wifi_start();
    if (err != ESP_OK) return err;

    /* TX power is the main thermal knob for the radio. The persisted value is
     * clamped by settings_load(), but stay defensive here. */
    uint8_t tx_dbm = cfg->wifi_tx_dbm;
    if (tx_dbm < WIFI_TX_MIN_DBM) tx_dbm = WIFI_TX_MIN_DBM;
    if (tx_dbm > WIFI_TX_MAX_DBM) tx_dbm = WIFI_TX_MAX_DBM;
    (void)wifi_apply_tx_power(tx_dbm);

    if (!s_client_role) {
        err = wifi_ap_netif_start();
        if (err != ESP_OK) {
            (void)esp_wifi_stop();
            return err;
        }
        s_ap_active = true;
        s_ap_sta_count = 0;
        portENTER_CRITICAL(&s_wifi_time_mux);
        s_ap_started_us = esp_timer_get_time();
        s_last_client_us = s_ap_started_us;
        portEXIT_CRITICAL(&s_wifi_time_mux);
    } else {
        s_ap_active = false;
    }

    /* No radio power-save: a sleeping AP radio throttles uploads to ~2 KB/s. */
    esp_wifi_set_ps(WIFI_PS_NONE);

    s_wifi_started = true;
    if (s_sta_enabled) {
        wifi_note_sta_try_now();
        esp_err_t serr = esp_wifi_connect();
        if (serr != ESP_OK) {
            ESP_LOGW(TAG, "station connect failed: %s", esp_err_to_name(serr));
        }
    }
    if (s_client_role && s_sta_fallback && !s_sta_enabled) {
        /* No credentials: the user asked for the AP fallback, so bring it up. */
        esp_err_t ferr = web_wifi_enable_ap_fallback();
        if (ferr != ESP_OK) {
            ESP_LOGW(TAG, "client fallback failed: %s", esp_err_to_name(ferr));
        }
    }

    ESP_LOGI(TAG, "Wi-Fi started (%s, tx=%u dBm, role=%s, sta=%s, ap=%s)",
             s_client_role ? "client" : "AP", (unsigned)tx_dbm,
             cfg->wifi_role == 1U ? "client" : "ap",
             s_sta_enabled ? cfg->sta_ssid : "off",
             s_ap_active ? s_ap_ip : "off");
    /* The supervisor also handles the optional station reconnect and the AP
     * fallback, so it is needed even when AP auto-off is disabled. */
    wifi_start_auto_off();
    return ESP_OK;
}

/* ---------- captive portal ---------- */

esp_err_t captive_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    if (strncmp(req->uri, "/sound-editor", 13) == 0 ||
        strncmp(req->uri, "/api/", 5) == 0) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    /* Captive portal: redirect every unknown path to the page so the phone's
     * connectivity probe opens it automatically (the DNS hijack routes those
     * probes here). Targets the configured AP address. */
    char loc[40];
    snprintf(loc, sizeof(loc), "http://%s/", s_ap_ip);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", loc);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, NULL, 0);
}

static void dns_server_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns: socket failed");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
    };
    uint32_t ap_ip = 0;
    memcpy(&ap_ip, s_ap_ip_bytes, sizeof(ap_ip));   /* already network order */
    /* Bind to the SoftAP address so the hijack stays off any STA interface.
     * If that address cannot be bound (AP not up yet or replaced), retry on
     * ANY with a fresh socket so DNS is not lost for the task's lifetime. */
    addr.sin_addr.s_addr = (ap_ip != 0U) ? ap_ip : htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        int err = errno;
        if (ap_ip == 0U) {
            ESP_LOGE(TAG, "dns: bind failed (%d)", err);
            close(sock);
            vTaskDelete(NULL);
            return;
        }
        ESP_LOGW(TAG, "DNS hijack bind to AP failed (%d), falling back to ANY", err);
        close(sock);
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock < 0) {
            ESP_LOGE(TAG, "dns: socket failed");
            vTaskDelete(NULL);
            return;
        }
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            ESP_LOGE(TAG, "dns: bind to ANY failed (%d)", errno);
            close(sock);
            vTaskDelete(NULL);
            return;
        }
        ESP_LOGI(TAG, "DNS hijack bound to ANY");
    } else if (ap_ip != 0U) {
        esp_ip4_addr_t ap = { .addr = ap_ip };
        ESP_LOGI(TAG, "DNS hijack bound to AP " IPSTR, IP2STR(&ap));
    } else {
        ESP_LOGI(TAG, "DNS hijack bound to ANY");
    }

    /* Receive timeout so the loop can observe s_dns_run after the AP is
     * stopped instead of blocking forever on recvfrom. */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    s_dns_run = true;
    uint8_t q[DNS_MSG_MAX];
    uint8_t r[DNS_MSG_MAX];
    uint32_t iters = 0;
    while ((s_dns_iter_cap == 0U || iters < s_dns_iter_cap) && s_dns_run) {
        iters++;
        struct sockaddr_in from = { 0 };
        socklen_t flen = sizeof(from);
        int n = recvfrom(sock, q, sizeof(q), 0, (struct sockaddr *)&from, &flen);
        if (n <= 12) {
            continue;
        }
        size_t rlen = dns_build_response(q, n, r, s_ap_ip_bytes);
        if (rlen > 0) {
            (void)sendto(sock, r, rlen, 0, (struct sockaddr *)&from, flen);
        }
    }
    close(sock);
    vTaskDelete(NULL);
}

void start_dns_hijack(void)
{
    if (xTaskCreate(dns_server_task, "dns_hijack", 4096, NULL, 9, NULL) != pdPASS) {
        ESP_LOGE(TAG, "dns: task create failed");
    }
}

void wifi_start_auto_off(void)
{
    if (s_autooff_started) {
        return;
    }
    if (xTaskCreate(wifi_auto_off_task, "wifi_off", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "wifi auto-off task create failed");
        return;
    }
    s_autooff_started = true;
}

esp_err_t web_wifi_set_tx_power(uint8_t dbm)
{
    if (dbm < WIFI_TX_MIN_DBM || dbm > WIFI_TX_MAX_DBM) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Before Wi-Fi is up the value is only persisted; wifi_start() applies it
     * after the radio starts. */
    return s_wifi_started ? wifi_apply_tx_power(dbm) : ESP_OK;
}

esp_err_t web_wifi_apply_sta(const char *ssid, const char *password, bool enable)
{
    if (ssid == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!enable) {
        s_sta_enabled = false;
        s_sta_connected = false;
        s_sta_ip[0] = '\0';
        s_sta_rssi = -127;
        if (s_wifi_started) {
            (void)esp_wifi_disconnect();
        }
        return ESP_OK;
    }
    if (!s_wifi_started) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t ssid_len = strlen(ssid);
    size_t pass_len = strlen(password);
    if (ssid_len == 0U || ssid_len > 32U || pass_len > 64U) {
        return ESP_ERR_INVALID_ARG;
    }
    wifi_config_t sta = { 0 };
    memcpy(sta.sta.ssid, ssid, ssid_len);
    memcpy(sta.sta.password, password, pass_len);
    sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
    if (!s_client_role) {
        /* The user pressed Connect without saving the mode first. Keep the AP
         * up for this session (so the page is not lost) and switch to APSTA;
         * the saved client role takes effect after the reboot. */
        s_client_role = true;
        (void)esp_wifi_set_mode(WIFI_MODE_APSTA);
        s_ap_active = true;
    }
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta);
    if (err != ESP_OK) {
        return err;
    }
    s_sta_enabled = true;
    s_sta_connected = false;
    s_sta_ip[0] = '\0';
    s_sta_rssi = -127;
    wifi_note_sta_try_now();
    if (!s_autooff_started) {
        wifi_start_auto_off(); /* reconnect supervisor */
    }
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "station connect failed: %s", esp_err_to_name(err));
    }
    return err;
}

void web_wifi_sta_status(char *ip, size_t ip_len, int *rssi, bool *connected,
                         bool *enabled)
{
    if (ip != NULL && ip_len > 0U) {
        snprintf(ip, ip_len, "%s", s_sta_ip[0] ? s_sta_ip : "0.0.0.0");
    }
    if (rssi != NULL) {
        *rssi = s_sta_connected ? (int)s_sta_rssi : -127;
    }
    if (connected != NULL) {
        *connected = s_sta_connected;
    }
    if (enabled != NULL) {
        *enabled = s_sta_enabled;
    }
}

esp_err_t wifi_scan_get(httpd_req_t *req)
{
    if (!s_wifi_started) {
        return send_json(req, "{\"ok\":false,\"error\":\"wifi off\"}");
    }
    wifi_scan_config_t sc = { 0 };
    sc.show_hidden = false;
    sc.scan_time.active.min = 40;
    sc.scan_time.active.max = 100;
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        (void)esp_wifi_clear_ap_list();
        return send_json(req, "{\"ok\":false,\"error\":\"scan failed\"}");
    }
    uint16_t num = 0;
    err = esp_wifi_scan_get_ap_num(&num);
    if (err != ESP_OK) {
        (void)esp_wifi_clear_ap_list();
        return send_json(req, "{\"ok\":false,\"error\":\"scan failed\"}");
    }
    if (num == 0U) {
        (void)esp_wifi_clear_ap_list();
        return send_json(req, "{\"ok\":true,\"aps\":[]}");
    }
    if (num > 20U) {
        num = 20U;
    }
    wifi_ap_record_t *recs = calloc(num, sizeof(*recs));
    char *out = malloc(4096);
    if (recs == NULL || out == NULL) {
        free(recs);
        free(out);
        (void)esp_wifi_clear_ap_list();
        return send_json(req, "{\"ok\":false,\"error\":\"no memory\"}");
    }
    uint16_t got = num;
    err = esp_wifi_scan_get_ap_records(&got, recs);
    if (err != ESP_OK) {
        free(recs);
        free(out);
        (void)esp_wifi_clear_ap_list();
        return send_json(req, "{\"ok\":false,\"error\":\"scan failed\"}");
    }
    size_t used = (size_t)snprintf(out, 4096, "{\"ok\":true,\"aps\":[");
    bool first = true;
    for (uint16_t i = 0; i < got && used < 3800U; ++i) {
        if (recs[i].ssid[0] == '\0') {
            continue;
        }
        char ssid[SETTINGS_SSID_MAX * 6];
        json_escape((const char *)recs[i].ssid, ssid, sizeof(ssid));
        used += (size_t)snprintf(out + used, 4096U - used,
                                 "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d,\"ch\":%u}",
                                 first ? "" : ",", ssid, (int)recs[i].rssi,
                                 (int)recs[i].authmode, (unsigned)recs[i].primary);
        first = false;
    }
    snprintf(out + used, 4096U - used, "]}");
    err = send_json(req, out);
    free(recs);
    free(out);
    (void)esp_wifi_clear_ap_list();
    return err;
}

bool web_wifi_is_started(void)
{
    return s_wifi_started;
}

