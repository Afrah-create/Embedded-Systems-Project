#pragma once

#include "driver/gpio.h"

/* Entrance terminal wiring. GPIO 4 is left free as the SD-card chip-select
   for Role B. SCK, MISO and MOSI are the shared SPI bus. */

#define PIN_SPI_SCK GPIO_NUM_18
#define PIN_SPI_MISO GPIO_NUM_19
#define PIN_SPI_MOSI GPIO_NUM_23

#define PIN_RFID_IN_CS GPIO_NUM_5
#define PIN_RFID_IN_RST GPIO_NUM_21
#define PIN_RFID_OUT_CS GPIO_NUM_16
#define PIN_RFID_OUT_RST GPIO_NUM_17

#define PIN_SERVO_IN GPIO_NUM_13
#define PIN_SERVO_OUT GPIO_NUM_25
#define PIN_LED_IN_G GPIO_NUM_26
#define PIN_LED_IN_R GPIO_NUM_32
#define PIN_LED_OUT_G GPIO_NUM_33
#define PIN_LED_OUT_R GPIO_NUM_14
#define PIN_BUZZER GPIO_NUM_27

/* GPIO 34 and 35 are input-only and have no internal pull-up.
   diagram.json provides the 10 kΩ pull-ups. */
#define PIN_TAIL_IN GPIO_NUM_34
#define PIN_TAIL_OUT GPIO_NUM_35

#define SCAN_QUEUE_LEN 8
#define DOOR_QUEUE_LEN 8
#define TAIL_QUEUE_LEN 8

#define DOOR_OPEN_MS 3000
#define DENIAL_LIMIT 3
#define DENIAL_BLOCK_MS 30000
#define RFID_DEBOUNCE_MS 800
