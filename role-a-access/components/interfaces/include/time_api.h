#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Role D replaces this with SNTP (East Africa Time, UTC+3).
   The stub returns seconds since boot and time_is_synced() == false. */

int64_t time_now_s(void);
bool time_is_synced(void);
