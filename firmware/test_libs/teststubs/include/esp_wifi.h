#ifndef MOCK_ESP_WIFI_H
#define MOCK_ESP_WIFI_H

/* Host shim for the ESP-IDF Wi-Fi API subset used by components/web. */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"

typedef enum {
    WIFI_MODE_NULL = 0,
    WIFI_MODE_STA,
    WIFI_MODE_AP,
    WIFI_MODE_APSTA,
} wifi_mode_t;

typedef enum {
    WIFI_IF_STA = 0,
    WIFI_IF_AP,
} wifi_interface_t;

typedef enum {
    WIFI_STORAGE_FLASH = 0,
    WIFI_STORAGE_RAM,
} wifi_storage_t;

typedef enum {
    WIFI_AUTH_OPEN = 0,
    WIFI_AUTH_WPA2_PSK = 4,
} wifi_auth_mode_t;

typedef enum {
    WIFI_BW20 = 1,
} wifi_bandwidth_t;

typedef enum {
    WIFI_PS_NONE = 0,
    WIFI_PS_MIN_MODEM,
    WIFI_PS_MAX_MODEM,
} wifi_ps_type_t;

typedef struct {
    uint8_t ssid[33];
    uint8_t password[65];
    uint8_t ssid_len;
    uint8_t channel;
    wifi_auth_mode_t authmode;
    uint8_t max_connection;
    uint16_t beacon_interval;
} wifi_ap_config_t;

typedef struct {
    wifi_ap_config_t ap;
} wifi_config_t;

typedef struct {
    bool show_hidden;
    struct {
        struct {
            uint32_t min;
            uint32_t max;
        } active;
        struct {
            uint32_t min;
            uint32_t max;
        } passive;
    } scan_time;
} wifi_scan_config_t;

typedef struct {
    int8_t rssi;
    uint8_t primary;
} wifi_ap_record_t;

typedef struct {
    uint8_t mac[6];
    uint8_t aid;
} wifi_event_ap_staconnected_t;

typedef struct {
    uint8_t aid;
    uint8_t reason;
} wifi_event_ap_stadisconnected_t;

typedef struct {
    esp_ip4_addr_t ip;
    uint8_t mac[6];
} ip_event_assigned_ip_to_client_t;

typedef struct {
    int _placeholder;
} wifi_init_config_t;

#define WIFI_INIT_CONFIG_DEFAULT() \
    (wifi_init_config_t) { 0 }

esp_err_t esp_wifi_init(const wifi_init_config_t *config);
esp_err_t esp_wifi_set_storage(wifi_storage_t storage);
esp_err_t esp_wifi_set_mode(wifi_mode_t mode);
esp_err_t esp_wifi_set_config(wifi_interface_t interface, wifi_config_t *conf);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *config, bool block);
esp_err_t esp_wifi_scan_get_ap_num(uint16_t *number);
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *number, wifi_ap_record_t *ap_records);
esp_err_t esp_wifi_clear_ap_list(void);
esp_err_t esp_wifi_set_bandwidth(wifi_interface_t interface, wifi_bandwidth_t bandwidth);
esp_err_t esp_wifi_set_ps(wifi_ps_type_t type);

#endif /* MOCK_ESP_WIFI_H */
