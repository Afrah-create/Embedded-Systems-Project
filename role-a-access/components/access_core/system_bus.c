#include "system_bus.h"

#include <string.h>

#include "board_config.h"
#include "console_api.h"
#include "door.h"
#include "time_api.h"

QueueHandle_t scan_queue;
QueueHandle_t door_queue;
QueueHandle_t tailgate_queue;

void system_bus_init(void)
{
    scan_queue = xQueueCreate(SCAN_QUEUE_LEN, sizeof(scan_event_t));
    door_queue = xQueueCreate(DOOR_QUEUE_LEN, sizeof(door_command_t));
    tailgate_queue = xQueueCreate(TAIL_QUEUE_LEN, sizeof(tailgate_event_t));
}

bool scan_queue_post(const char *uid, direction_t direction)
{
    scan_event_t ev;

    memset(&ev, 0, sizeof(ev));
    if (uid != NULL) {
        strncpy(ev.uid, uid, UID_STR_LEN - 1);
    }
    ev.direction = direction;
    ev.time_s = time_now_s();

    /* Print before the send. access_task has higher priority, so it runs
       inside xQueueSend and its LOG line then follows this SCAN line. */
    console_printf("SCAN uid=%s dir=%s\n", ev.uid, direction_name(direction));
    if (scan_queue == NULL || xQueueSend(scan_queue, &ev, 0) != pdTRUE) {
        console_printf("SCAN_DROPPED uid=%s dir=%s\n", ev.uid, direction_name(direction));
        return false;
    }
    return true;
}
