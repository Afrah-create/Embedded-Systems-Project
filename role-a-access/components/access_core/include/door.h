#pragma once

#include <stdbool.h>

typedef enum {
    DOOR_ENTRY = 0,
    DOOR_EXIT = 1,
} door_id_t;

typedef enum {
    DOOR_CMD_GRANT = 0,
    DOOR_CMD_DENY = 1,
    DOOR_CMD_ALERT = 2,
} door_action_t;

typedef struct {
    door_id_t door;
    door_action_t action;
} door_command_t;

void door_init(void);
void door_task(void *arg);
void tailgate_task(void *arg);
bool door_post(const door_command_t *cmd);
bool door_grant_is_fresh(door_id_t door);
