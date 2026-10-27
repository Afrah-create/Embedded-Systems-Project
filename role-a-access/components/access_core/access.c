#include "access.h"

#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "console_api.h"
#include "door.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "occupancy.h"
#include "schedule_api.h"
#include "storage_api.h"
#include "system_bus.h"
#include "time_api.h"

#define DENIAL_SLOTS 8

typedef struct {
    char uid[UID_STR_LEN];
    uint8_t denies;
    int64_t blocked_until_us;
    bool used;
} denial_slot_t;

static denial_slot_t denials[DENIAL_SLOTS];
static SemaphoreHandle_t denial_mu;

void access_init(void)
{
    denial_mu = xSemaphoreCreateMutex();
    memset(denials, 0, sizeof(denials));
}

static denial_slot_t *find_slot(const char *uid, bool create)
{
    denial_slot_t *free_slot = NULL;

    for (int i = 0; i < DENIAL_SLOTS; i++) {
        if (denials[i].used && strcmp(denials[i].uid, uid) == 0) {
            return &denials[i];
        }
        if (!denials[i].used && free_slot == NULL) {
            free_slot = &denials[i];
        }
    }
    if (!create) {
        return NULL;
    }
    if (free_slot == NULL) {
        free_slot = &denials[0];
        memset(free_slot, 0, sizeof(*free_slot));
    }
    strncpy(free_slot->uid, uid, UID_STR_LEN - 1);
    free_slot->used = true;
    free_slot->denies = 0;
    free_slot->blocked_until_us = 0;
    return free_slot;
}

static bool denial_is_blocked(const char *uid)
{
    bool blocked = false;
    int64_t now = esp_timer_get_time();

    xSemaphoreTake(denial_mu, portMAX_DELAY);
    denial_slot_t *slot = find_slot(uid, false);
    if (slot != NULL && slot->blocked_until_us > now) {
        blocked = true;
    } else if (slot != NULL && slot->blocked_until_us != 0) {
        slot->denies = 0;
        slot->blocked_until_us = 0;
    }
    xSemaphoreGive(denial_mu);
    return blocked;
}

static void denial_refresh(const char *uid)
{
    xSemaphoreTake(denial_mu, portMAX_DELAY);
    denial_slot_t *slot = find_slot(uid, false);
    if (slot != NULL) {
        slot->blocked_until_us = esp_timer_get_time() + (int64_t)DENIAL_BLOCK_MS * 1000;
    }
    xSemaphoreGive(denial_mu);
}

static bool note_denial(const char *uid)
{
    bool just_blocked = false;

    xSemaphoreTake(denial_mu, portMAX_DELAY);
    denial_slot_t *slot = find_slot(uid, true);
    if (slot->denies < 255) {
        slot->denies++;
    }
    if (slot->denies >= DENIAL_LIMIT && slot->blocked_until_us == 0) {
        slot->blocked_until_us = esp_timer_get_time() + (int64_t)DENIAL_BLOCK_MS * 1000;
        just_blocked = true;
    }
    xSemaphoreGive(denial_mu);
    return just_blocked;
}

static void denial_clear(const char *uid)
{
    xSemaphoreTake(denial_mu, portMAX_DELAY);
    denial_slot_t *slot = find_slot(uid, false);
    if (slot != NULL) {
        memset(slot, 0, sizeof(*slot));
    }
    xSemaphoreGive(denial_mu);
}

void access_clear_blocks(void)
{
    xSemaphoreTake(denial_mu, portMAX_DELAY);
    memset(denials, 0, sizeof(denials));
    xSemaphoreGive(denial_mu);
    console_printf("TEST blocks cleared\n");
}

void access_print_blocks(void)
{
    denial_slot_t copy[DENIAL_SLOTS];
    int64_t now = esp_timer_get_time();
    bool any = false;

    xSemaphoreTake(denial_mu, portMAX_DELAY);
    memcpy(copy, denials, sizeof(copy));
    xSemaphoreGive(denial_mu);

    for (int i = 0; i < DENIAL_SLOTS; i++) {
        if (!copy[i].used) {
            continue;
        }
        any = true;
        if (copy[i].blocked_until_us > now) {
            int left = (int)((copy[i].blocked_until_us - now) / 1000000);
            console_printf("BLOCK uid=%s denies=%u left=%ds\n", copy[i].uid, copy[i].denies, left);
        } else {
            console_printf("WATCH uid=%s denies=%u\n", copy[i].uid, copy[i].denies);
        }
    }
    if (!any) {
        console_printf("blocks: none\n");
    }
}

static void finish(const scan_event_t *scan, access_result_t result, access_reason_t reason, bool count_denial)
{
    log_event_t log;
    char json[180];
    bool just_blocked = false;
    door_command_t cmd;

    if (count_denial) {
        just_blocked = note_denial(scan->uid);
    }

    memset(&log, 0, sizeof(log));
    log.time_s = scan->time_s != 0 ? scan->time_s : time_now_s();
    log.time_synced = time_is_synced();
    strncpy(log.uid, scan->uid, UID_STR_LEN - 1);
    log.direction = scan->direction;
    log.result = result;
    log.reason = reason;
    log.inside_count = get_building_count();
    log_event(&log);

    snprintf(json, sizeof(json),
             "{\"uid\":\"%s\",\"dir\":\"%s\",\"result\":\"%s\",\"reason\":\"%s\"}",
             log.uid, direction_name(scan->direction), access_result_name(result), access_reason_name(reason));
    publish("cocis/entrance/decision", json);

    if (just_blocked) {
        snprintf(json, sizeof(json),
                 "{\"event\":\"tag_blocked\",\"uid\":\"%s\",\"seconds\":%d}",
                 scan->uid, DENIAL_BLOCK_MS / 1000);
        publish("cocis/entrance/alert", json);
    }

    cmd.door = scan->direction == DIR_EXIT ? DOOR_EXIT : DOOR_ENTRY;
    cmd.action = result == ACCESS_RESULT_GRANTED ? DOOR_CMD_GRANT : DOOR_CMD_DENY;
    door_post(&cmd);
}

static void handle_scan(const scan_event_t *scan)
{
    user_record_t user;
    bool emergency_exit = scan->direction == DIR_EXIT && emergency_active();

    /* Order: block list, registered tag, schedule, then anti-passback.
       Anti-passback is the atomic check inside occupancy_set_inside. */
    if (!emergency_exit && denial_is_blocked(scan->uid)) {
        denial_refresh(scan->uid);
        finish(scan, ACCESS_RESULT_DENIED, REASON_TAG_BLOCKED, false);
        return;
    }

    if (!user_lookup(scan->uid, &user)) {
        finish(scan, ACCESS_RESULT_DENIED, REASON_UNKNOWN_TAG, true);
        return;
    }

    if (scan->direction == DIR_ENTRY) {
        if (!is_building_open()) {
            finish(scan, ACCESS_RESULT_DENIED, REASON_OUTSIDE_HOURS, true);
            return;
        }
        switch (occupancy_set_inside(scan->uid, true)) {
        case OCC_OK:
            denial_clear(scan->uid);
            finish(scan, ACCESS_RESULT_GRANTED, REASON_GRANTED, false);
            return;
        case OCC_ALREADY_INSIDE:
            finish(scan, ACCESS_RESULT_DENIED, REASON_ALREADY_INSIDE, true);
            return;
        case OCC_NOT_INSIDE:
        case OCC_STORAGE:
        default:
            finish(scan, ACCESS_RESULT_DENIED, REASON_STORAGE, false);
            return;
        }
    }

    if (!exit_allowed()) {
        finish(scan, ACCESS_RESULT_DENIED, REASON_OUTSIDE_HOURS, true);
        return;
    }
    switch (occupancy_set_inside(scan->uid, false)) {
    case OCC_OK:
        denial_clear(scan->uid);
        finish(scan, ACCESS_RESULT_GRANTED, REASON_GRANTED, false);
        return;
    case OCC_NOT_INSIDE:
        finish(scan, ACCESS_RESULT_DENIED, REASON_NOT_INSIDE, true);
        return;
    case OCC_ALREADY_INSIDE:
    case OCC_STORAGE:
    default:
        finish(scan, ACCESS_RESULT_DENIED, REASON_STORAGE, false);
        return;
    }
}

void access_task(void *arg)
{
    (void)arg;
    console_printf("access_task started\n");
    for (;;) {
        scan_event_t scan;
        if (xQueueReceive(scan_queue, &scan, portMAX_DELAY) == pdTRUE) {
            handle_scan(&scan);
        }
    }
}
