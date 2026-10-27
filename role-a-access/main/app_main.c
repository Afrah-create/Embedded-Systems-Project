#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "access.h"
#include "console.h"
#include "console_api.h"
#include "door.h"
#include "occupancy.h"
#include "rfid_reader.h"
#include "schedule_api.h"
#include "storage_api.h"
#include "stubs.h"
#include "system_bus.h"

void app_main(void)
{
    /* Queues and mutexes are created before any worker task. A task that
       runs early must not touch a queue that does not exist yet.
       access_task is priority 6, above the readers at 5, so a posted scan
       is decided before the reader polls again. */
    stubs_init();
    system_bus_init();
    occupancy_init();
    access_init();
    door_init();
    if (rfid_init() != ESP_OK) {
        console_printf("RFID init failed. Serial IN and OUT still reach access_task.\n");
    }

    console_printf("=== CoCIS entrance / Role A access control ===\n");
    storage_print_users();
    console_printf("inside=%d building=%s emergency=%s\n", get_building_count(),
                   is_building_open() ? "OPEN" : "CLOSED", emergency_active() ? "ON" : "OFF");

    xTaskCreate(console_task, "console", 6144, NULL, 4, NULL);
    xTaskCreate(access_task, "access", 6144, NULL, 6, NULL);
    xTaskCreate(door_task, "door", 4096, NULL, 5, NULL);
    xTaskCreate(tailgate_task, "tailgate", 4096, NULL, 5, NULL);
    xTaskCreate(rfid_in_task, "rfid_in", 8192, NULL, 5, NULL);
    xTaskCreate(rfid_out_task, "rfid_out", 8192, NULL, 5, NULL);
}
