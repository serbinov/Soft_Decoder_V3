#ifndef MOCK_ESP_NETIF_H
#define MOCK_ESP_NETIF_H

/* Host shim for the ESP-IDF netif API subset used by components/web. */

#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint32_t addr; /* network byte order (first octet in the low byte) */
} esp_ip4_addr_t;

#define ESP_IP4TOADDR(a, b, c, d)                                          \
    ((uint32_t)((a) & 0xff) | ((uint32_t)((b) & 0xff) << 8) |              \
     ((uint32_t)((c) & 0xff) << 16) | ((uint32_t)((d) & 0xff) << 24))

#define IPSTR "%d.%d.%d.%d"
#define IP2STR(a)                                                                \
    (int)((a)->addr & 0xff), (int)(((a)->addr >> 8) & 0xff),                     \
        (int)(((a)->addr >> 16) & 0xff), (int)(((a)->addr >> 24) & 0xff)

#define ESP_IPADDR_TYPE_V4 0

typedef struct {
    uint8_t type;
    union {
        esp_ip4_addr_t ip4;
    } u_addr;
} esp_ip_addr_t;

typedef struct {
    esp_ip4_addr_t ip;
    esp_ip4_addr_t netmask;
    esp_ip4_addr_t gw;
} esp_netif_ip_info_t;

typedef struct {
    esp_ip_addr_t ip;
} esp_netif_dns_info_t;

typedef enum {
    ESP_NETIF_DNS_MAIN = 0,
    ESP_NETIF_DNS_BACKUP = 1,
} esp_netif_dns_type_t;

typedef struct esp_netif_obj esp_netif_t;

esp_err_t esp_netif_init(void);
esp_netif_t *esp_netif_create_default_wifi_ap(void);
esp_netif_t *esp_netif_create_default_wifi_sta(void);
esp_err_t esp_netif_dhcps_stop(esp_netif_t *netif);
esp_err_t esp_netif_dhcps_start(esp_netif_t *netif);
esp_err_t esp_netif_set_ip_info(esp_netif_t *netif, const esp_netif_ip_info_t *ip_info);
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *ip_info);
esp_err_t esp_netif_set_dns_info(esp_netif_t *netif, esp_netif_dns_type_t type,
                                 esp_netif_dns_info_t *dns);
esp_err_t esp_netif_str_to_ip4(const char *src, esp_ip4_addr_t *dst);

#endif /* MOCK_ESP_NETIF_H */
