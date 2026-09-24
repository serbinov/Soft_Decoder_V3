#ifndef PROVISION_H
#define PROVISION_H

#include <stdbool.h>

/* Runs at boot (before audio/web). If the background listener flagged a
 * provisioning request, erases the external NOR, receives the sound files over
 * UART and restarts. Returns true only when provisioning ran. */
bool provision_try(void);

/* Background UART listener: watches UART0 for "PROV\n". On trigger, stores a
 * request flag in NVS and reboots so the next boot enters provisioning. */
void provision_listener_start(void);

#endif
