#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "settings.h"

#define AP_IP_DEFAULT "192.168.100.1"

/* WiFi/AP + DNS captive portal, split out of the HTTP router. */
bool parse_ip4(const char *s, esp_ip4_addr_t *out);
esp_err_t wifi_start(const settings_config_t *cfg);
void start_dns_hijack(void);
void wifi_start_auto_off(void);
esp_err_t captive_handler(httpd_req_t *req, httpd_err_code_t err);
bool web_wifi_is_started(void);
