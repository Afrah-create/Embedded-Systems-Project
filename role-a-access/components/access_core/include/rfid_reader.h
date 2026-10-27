#pragma once

#include "esp_err.h"

esp_err_t rfid_init(void);
void rfid_in_task(void *arg);
void rfid_out_task(void *arg);
