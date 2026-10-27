#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define RFID_ENTRY 0
#define RFID_EXIT 1

esp_err_t mfrc522_init(void);
bool mfrc522_ready(int index);
esp_err_t mfrc522_reinit(int index);
bool mfrc522_read_uid(int index, uint8_t *uid, uint8_t *uid_len);
