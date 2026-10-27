#pragma once

#include <stddef.h>

#include "access_types.h"

/* Role B implements these. The copies in platform_stubs are RAM stand-ins.
   set_inside must update memory and return. It must not wait on the SD card;
   Role A's occupancy mutex is held across the call. Queue the disk write. */

bool user_lookup(const char *uid, user_record_t *out);
bool set_inside(const char *uid, bool inside);
void log_event(const log_event_t *event);

/* Boot helper so the live count matches the stored inside flags. */
int storage_count_inside(void);
void storage_print_users(void);
