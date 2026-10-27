#include "rfid_reader.h"

#include <stdio.h>

#include "board_config.h"
#include "console_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mfrc522.h"
#include "system_bus.h"

static void uid_to_text(const uint8_t *uid, uint8_t len, char *out)
{
    char *p = out;

    for (uint8_t i = 0; i < len && i < 7; i++) {
        sprintf(p, "%02X", uid[i]);
        p += 2;
    }
    *p = '\0';
}

static void reader_loop(int index, direction_t direction, const char *name)
{
    console_printf("%s started\n", name);
    for (;;) {
        uint8_t raw[10];
        uint8_t len = 0;
        char text[UID_STR_LEN];

        if (!mfrc522_ready(index)) {
            mfrc522_reinit(index);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        /* SPI cannot run from an interrupt, so the reader is a task.
           It only posts a scan. It does not decide or move a door. */
        if (!mfrc522_read_uid(index, raw, &len)) {
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }

        uid_to_text(raw, len, text);
        scan_queue_post(text, direction);
        vTaskDelay(pdMS_TO_TICKS(RFID_DEBOUNCE_MS));
    }
}

esp_err_t rfid_init(void)
{
    return mfrc522_init();
}

void rfid_in_task(void *arg)
{
    (void)arg;
    reader_loop(RFID_ENTRY, DIR_ENTRY, "rfid_in_task");
}

void rfid_out_task(void *arg)
{
    (void)arg;
    reader_loop(RFID_EXIT, DIR_EXIT, "rfid_out_task");
}
