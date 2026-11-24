#include <stdint.h>
#include "esp_rom_gpio.h"
#include "soc/gpio_struct.h"
#include "soc/gpio_sig_map.h"

/* Force the linker to pull this object into the bootloader image. */
void bootloader_hooks_include(void) { }

/* Pins whose external load must not float while the ROM/bootloader run.
 * Keep in sync with components/pinmap/include/pinmap.h. */
#define BOARD_BOOT_SAFE_MASK (                  \
    (1ULL << 1)  | (1ULL << 3)  |               \
    (1ULL << 10) | (1ULL << 11) | (1ULL << 12) |\
    (1ULL << 33) | (1ULL << 34) | (1ULL << 35) |\
    (1ULL << 36) | (1ULL << 37) | (1ULL << 38) |\
    (1ULL << 39) | (1ULL << 40) | (1ULL << 41) |\
    (1ULL << 42) | (1ULL << 46))

void bootloader_before_init(void)
{
    uint64_t mask = BOARD_BOOT_SAFE_MASK;

    /* 1) Select GPIO function on IO_MUX (output still disabled). */
    for (uint32_t pin = 0; pin < 49U; ++pin) {
        if ((mask >> pin) & 1ULL) {
            esp_rom_gpio_pad_select_gpio(pin);
        }
    }

    /* 2) Preload the output latch LOW before enabling any pad driver, so
     * no pin can glitch high while the output is being turned on. */
    GPIO.out_w1tc = (uint32_t)mask;
    GPIO.out1_w1tc.val = (uint32_t)(mask >> 32);

    /* 3) Connect the GPIO register to the pad and enable the output. */
    for (uint32_t pin = 0; pin < 49U; ++pin) {
        if ((mask >> pin) & 1ULL) {
            esp_rom_gpio_connect_out_signal(pin, SIG_GPIO_OUT_IDX, false, false);
        }
    }
}
