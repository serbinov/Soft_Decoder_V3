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
#include <string.h>

#ifdef TAG
#undef TAG
#endif
#define TAG "web_wifi"
/* Radio at full 20 dBm (unit: 0.25 dBm). The built-in antenna has little
 * margin, so a lower PA setting shows up as retries and stalled transfers. */
#define WIFI_MAX_TX_POWER_QDBM 80
/* Free the single-client slot after this many idle seconds (default 300). */
#define WIFI_AP_INACTIVE_SEC 120
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
                s_last_client_us = esp_timer_get_time();
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
                s_last_client_us = esp_timer_get_time();
                ESP_LOGW(TAG, "STA disconnected (aid=%d, reason=%d), clients=%u",
                         (int)e->aid, (int)e->reason, (unsigned)s_ap_sta_count);
                break;
            }
            default:
                break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_ASSIGNED_IP_TO_CLIENT) {
        const ip_event_assigned_ip_to_client_t *e = data;
        ESP_LOGI(TAG, "Client got IP " IPSTR, IP2STR(&e->ip));
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
 * time (auto_off_min, 0 = disabled). */
static void wifi_auto_off_task(void *arg)
{
    (void)arg;
    uint32_t iters = 0;
    while (s_autooff_iter_cap == 0U || iters < s_autooff_iter_cap) {
        iters++;
        vTaskDelay(pdMS_TO_TICKS(10000));
        uint8_t minutes = s_auto_off_min;
        if (!s_wifi_started || minutes == 0U || s_ap_sta_count != 0U) {
            continue;
        }
        /* Time since the last client was seen (or since AP start when no one
         * ever connected), not since boot, so a client that was connected for
         * a long time does not cause an immediate off right after leaving. */
        if (esp_timer_get_time() - s_last_client_us <
            (int64_t)minutes * 60LL * 1000000LL) {
            continue;
        }
        ESP_LOGW(TAG, "WiFi auto-off: no client for %u min, stopping AP",
                 (unsigned)minutes);
        (void)esp_wifi_stop();
        s_wifi_started = false;
        s_dns_run = false; /* let the DNS hijack task exit and free its socket */
        vTaskDelete(NULL);
    }
}

esp_err_t wifi_start(const settings_config_t *cfg)
{
    if (cfg->wifi_mode == 0) {
        return ESP_OK;
    }

    s_auto_off_min = cfg->auto_off_min;
    (void)nvs_flash_init();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    /* Temporary station interface: used only to scan for the quietest channel
     * at boot. It stays unused afterwards (AP-only design). */
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (ap_netif == NULL) return ESP_ERR_NO_MEM;

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&wcfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    /* The config lives in our own NVS store; keep the driver from writing. */
    (void)esp_wifi_set_storage(WIFI_STORAGE_RAM);

    (void)esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &wifi_event_handler, NULL, NULL);
    (void)esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT,
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

    /* AP-only: a stored STA/APSTA mode is ignored. */
    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err != ESP_OK) return err;

    /* Access point address: configurable (default 192.168.100.1). */
    esp_ip4_addr_t ap_addr = { 0 };
    if (!parse_ip4(cfg->ap_ip, &ap_addr)) {
        (void)esp_netif_str_to_ip4(AP_IP_DEFAULT, &ap_addr);
    }
    memcpy(s_ap_ip_bytes, &ap_addr.addr, sizeof(s_ap_ip_bytes));

    if (ap_netif != NULL) {
        esp_netif_ip_info_t ip = { 0 };
        ip.ip.addr = ap_addr.addr;
        ip.gw.addr = ap_addr.addr;
        ip.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0);
        err = esp_netif_dhcps_stop(ap_netif);
        if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) return err;
        err = esp_netif_set_ip_info(ap_netif, &ip);
        if (err != ESP_OK) return err;

        /* Hand the AP IP to DHCP clients as their DNS server: every name
         * resolves to the board so connectivity probes land on the HTTP
         * server (captive portal opens the page automatically). */
        esp_netif_dns_info_t dns = { 0 };
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4.addr = ap_addr.addr;
        err = esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns);
        if (err != ESP_OK) return err;

        /* DHCP option 114 (RFC 8910): Windows 10/11 and Android use this URI
         * for the "Sign in to network" / captive portal page. This makes the
         * decoder page open instead of the NCSI msftconnecttest redirect. */
        snprintf(s_captive_uri, sizeof(s_captive_uri), "http://" IPSTR "/",
                 IP2STR(&ap_addr));
        err = esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                                     ESP_NETIF_CAPTIVEPORTAL_URI,
                                     s_captive_uri, (uint32_t)strlen(s_captive_uri));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "DHCP captive portal URI option failed: %s", esp_err_to_name(err));
        }
    }

    err = esp_wifi_start();
    if (err != ESP_OK) return err;

    /* Never let a reduced PHY setting shrink the link budget: this is the
     * single biggest software knob for range and packet loss. */
    err = esp_wifi_set_max_tx_power(WIFI_MAX_TX_POWER_QDBM);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "max TX power not applied: %s", esp_err_to_name(err));
    }

    err = esp_netif_dhcps_start(ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        (void)esp_wifi_stop();
        return err;
    }

    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20);

    /* No radio power-save: a sleeping AP radio throttles uploads to ~2 KB/s. */
    esp_wifi_set_ps(WIFI_PS_NONE);

    /* The AP holds exactly one client. If a phone vanishes without sending a
     * deauth (out of range, Wi-Fi toggled off), the default 300 s timeout
     * keeps the slot busy and blocks a reconnect. 120 s still gives an idle
     * but alive client plenty of margin (phones send null frames). */
    err = esp_wifi_set_inactive_time(WIFI_IF_AP, WIFI_AP_INACTIVE_SEC);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "inactive time not applied: %s", esp_err_to_name(err));
    }

    s_ap_sta_count = 0;
    s_ap_started_us = esp_timer_get_time();
    s_last_client_us = s_ap_started_us;
    s_wifi_started = true;

    snprintf(s_ap_ip, sizeof(s_ap_ip), IPSTR, IP2STR(&ap_addr));
    if (ap_netif != NULL) {
        esp_netif_ip_info_t actual = { 0 };
        if (esp_netif_get_ip_info(ap_netif, &actual) == ESP_OK) {
            snprintf(s_ap_ip, sizeof(s_ap_ip), IPSTR, IP2STR(&actual.ip));
            memcpy(s_ap_ip_bytes, &actual.ip.addr, sizeof(s_ap_ip_bytes));
        }
    }
    ESP_LOGI(TAG, "Wi-Fi started (AP-only, channel %u, ip=%s)",
             (unsigned)ap.ap.channel, s_ap_ip);
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
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "dns: bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "DNS hijack running on :53");

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
    if (xTaskCreate(wifi_auto_off_task, "wifi_off", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "wifi auto-off task create failed");
    }
}

bool web_wifi_is_started(void)
{
    return s_wifi_started;
}

