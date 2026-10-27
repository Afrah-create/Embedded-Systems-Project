#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Shared contract. Coordinator: do not rename these types.
   UID text is 8 hex digits (4-byte card) or 14 hex digits (7-byte card),
   uppercase, with no colons. Wokwi "01:02:03:04" is stored as "01020304". */
#define UID_STR_LEN 15

typedef enum {
    DIR_ENTRY = 0,
    DIR_EXIT = 1,
} direction_t;

typedef enum {
    REASON_GRANTED = 0,
    REASON_UNKNOWN_TAG,
    REASON_OUTSIDE_HOURS,
    REASON_ALREADY_INSIDE,
    REASON_NOT_INSIDE,
    REASON_TAG_BLOCKED,
    REASON_TAILGATE,
    REASON_STORAGE,
    REASON_COUNT,
} access_reason_t;

typedef enum {
    ACCESS_RESULT_GRANTED = 0,
    ACCESS_RESULT_DENIED = 1,
    ACCESS_RESULT_ALERT = 2,
} access_result_t;

typedef enum {
    USER_ROLE_STUDENT = 0,
    USER_ROLE_STAFF = 1,
    USER_ROLE_ADMIN = 2,
} user_role_t;

typedef struct {
    char uid[UID_STR_LEN];
    char name[24];
    user_role_t role;
    bool inside;
} user_record_t;

typedef struct {
    int64_t time_s;
    bool time_synced;
    char uid[UID_STR_LEN];
    direction_t direction;
    access_result_t result;
    access_reason_t reason;
    int inside_count;
} log_event_t;

const char *access_reason_name(access_reason_t reason);
const char *access_result_name(access_result_t result);
const char *direction_name(direction_t direction);
const char *user_role_name(user_role_t role);
bool uid_normalize(const char *in, char *out, size_t out_len);
