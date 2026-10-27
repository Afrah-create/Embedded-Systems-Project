#pragma once

#include <stdbool.h>

typedef enum {
    OCC_OK = 0,
    OCC_ALREADY_INSIDE,
    OCC_NOT_INSIDE,
    OCC_STORAGE,
} occ_result_t;

void occupancy_init(void);
int get_building_count(void);

/* Check and update under the state mutex. inside=true is an entry,
   inside=false is an exit. Returns OCC_ALREADY_INSIDE or OCC_NOT_INSIDE
   when anti-passback rejects the change. */
occ_result_t occupancy_set_inside(const char *uid, bool inside);
