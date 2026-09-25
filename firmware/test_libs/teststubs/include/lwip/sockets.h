#ifndef MOCK_LWIP_SOCKETS_H
#define MOCK_LWIP_SOCKETS_H

/* Minimal host shim for the lwIP socket API used by the captive-portal DNS
 * hijack in components/web/src/web.c. Every call is routed to a mock so the
 * DNS task can be driven without real network access. */

#include <stddef.h>
#include <stdint.h>

#define AF_INET 2
#define SOCK_DGRAM 2
#define IPPROTO_UDP 17
#define INADDR_ANY 0u

typedef unsigned short mock_sa_family_t;

struct in_addr {
    uint32_t s_addr;
};

struct sockaddr {
    mock_sa_family_t sa_family;
    char sa_data[14];
};

struct sockaddr_in {
    mock_sa_family_t sin_family;
    uint16_t sin_port;
    struct in_addr sin_addr;
    char sin_zero[8];
};

typedef unsigned int mock_socklen_t;

#define socklen_t mock_socklen_t

uint16_t mock_lwip_htons(uint16_t v);
uint32_t mock_lwip_htonl(uint32_t v);

int mock_socket(int domain, int type, int protocol);
int mock_bind(int sockfd, const struct sockaddr *addr, mock_socklen_t addrlen);
int mock_recvfrom(int sockfd, void *buf, size_t len, int flags,
                  struct sockaddr *src_addr, mock_socklen_t *addrlen);
int mock_sendto(int sockfd, const void *buf, size_t len, int flags,
                const struct sockaddr *dest_addr, mock_socklen_t addrlen);
int mock_socket_close(int fd);

#define htons  mock_lwip_htons
#define htonl  mock_lwip_htonl
#define socket mock_socket
#define bind   mock_bind
#define recvfrom mock_recvfrom
#define sendto mock_sendto
#define close  mock_socket_close

#endif /* MOCK_LWIP_SOCKETS_H */
