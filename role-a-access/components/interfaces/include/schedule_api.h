#pragma once

#include <stdbool.h>

/* Role D implements the getters and publish().
   The two setters exist only in the Role A test harness so outside-hours
   and emergency can be demonstrated before Role D and Role E are linked.
   Role E's real command must call Role D's validated schedule setter. */

bool is_building_open(void);
bool exit_allowed(void);
bool emergency_active(void);
void publish(const char *topic, const char *json);

void stub_set_building_open(bool open);
void stub_set_emergency(bool on);
