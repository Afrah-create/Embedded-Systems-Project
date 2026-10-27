#pragma once

#include <stdbool.h>

#include "access_types.h"
#include "door.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef struct {
    char uid[UID_STR_LEN];
    direction_t direction;
    int64_t time_s;
} scan_event_t;

typedef struct {
    door_id_t door;
} tailgate_event_t;

extern QueueHandle_t scan_queue;
extern QueueHandle_t door_queue;
extern QueueHandle_t tailgate_queue;

void system_bus_init(void);

/* Drops the scan if the queue is full. Readers must not block on the
   decision task, and a dropped scan is not a decision, so it is not logged. */
bool scan_queue_post(const char *uid, direction_t direction);
