#include <ctype.h>
#include <string.h>

#include "access_types.h"

const char *access_reason_name(access_reason_t reason)
{
    switch (reason) {
    case REASON_GRANTED:
        return "GRANTED";
    case REASON_UNKNOWN_TAG:
        return "UNKNOWN_TAG";
    case REASON_OUTSIDE_HOURS:
        return "OUTSIDE_HOURS";
    case REASON_ALREADY_INSIDE:
        return "ALREADY_INSIDE";
    case REASON_NOT_INSIDE:
        return "NOT_INSIDE";
    case REASON_TAG_BLOCKED:
        return "TAG_BLOCKED";
    case REASON_TAILGATE:
        return "TAILGATE";
    case REASON_STORAGE:
        return "STORAGE";
    default:
        return "UNKNOWN";
    }
}

const char *access_result_name(access_result_t result)
{
    switch (result) {
    case ACCESS_RESULT_GRANTED:
        return "GRANTED";
    case ACCESS_RESULT_DENIED:
        return "DENIED";
    case ACCESS_RESULT_ALERT:
        return "ALERT";
    default:
        return "UNKNOWN";
    }
}

const char *direction_name(direction_t direction)
{
    return direction == DIR_EXIT ? "OUT" : "IN";
}

const char *user_role_name(user_role_t role)
{
    switch (role) {
    case USER_ROLE_STUDENT:
        return "student";
    case USER_ROLE_STAFF:
        return "staff";
    case USER_ROLE_ADMIN:
        return "admin";
    default:
        return "unknown";
    }
}

bool uid_normalize(const char *in, char *out, size_t out_len)
{
    size_t n = 0;

    if (in == NULL || out == NULL || out_len < UID_STR_LEN) {
        return false;
    }

    for (const char *p = in; *p != '\0'; p++) {
        if (*p == ':' || *p == ' ' || *p == '-') {
            continue;
        }
        if (!isxdigit((unsigned char)*p)) {
            return false;
        }
        if (n + 1 >= UID_STR_LEN) {
            return false;
        }
        out[n++] = (char)toupper((unsigned char)*p);
    }

    if (n != 8 && n != 14) {
        return false;
    }

    out[n] = '\0';
    return true;
}
