#ifndef PINMAP_H
#define PINMAP_H

#include "esp_err.h"

/* ESP32-S3FH4R2 pinout (master document v4.0). */

/* Motor DRV8870 */
#define PIN_MOTOR_IN1      12
#define PIN_MOTOR_IN2      11

/* DCC / service track */
#define PIN_DCC_IN          9
#define PIN_ACK_LOAD        3

/* Analog (ADC1) */
#define PIN_BEMF1           4
#define PIN_BEMF2           5
#define PIN_RAIL_SENSE      6
#define PIN_CURRENT_SENSE   7

/* Audio I2S (MAX98357A) */
#define PIN_AUDIO_BCLK     34
#define PIN_AUDIO_WS       35
#define PIN_AUDIO_DOUT     33
#define PIN_AUDIO_SD_MODE  37

/* External SPI NOR (W25Q128) */
#define PIN_STORAGE_CS     13
#define PIN_STORAGE_SCK    16
#define PIN_STORAGE_MOSI   15
#define PIN_STORAGE_MISO   14

/* AUX outputs (PWM) */
#define PIN_AUX_F0F        10
#define PIN_AUX_F0R        46
#define PIN_AUX1           41
#define PIN_AUX2           40
#define PIN_AUX3            1
#define PIN_AUX4           36
#define PIN_AUX5           42
#define PIN_AUX6           39
#define PIN_AUX7           38

/* I2C */
#define PIN_I2C_SDA        17
#define PIN_I2C_SCL        18

esp_err_t pinmap_validate(void);

#endif
