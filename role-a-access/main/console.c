#include "console.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "access.h"
#include "console_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "occupancy.h"
#include "schedule_api.h"
#include "storage_api.h"
#include "system_bus.h"

static void print_help(void)
{
    console_printf(
        "Commands (Role A test harness, not the management CLI):\n"
        "  HELP                         show this list\n"
        "  STATUS                       count, schedule, blocks\n"
        "  USERS                        stub registry\n"
        "  OPEN / CLOSED                building schedule stand-in\n"
        "  EMERGENCY ON / EMERGENCY OFF exit override stand-in\n"
        "  IN <uid> / OUT <uid>         post a scan, same path as a reader\n"
        "  RAPID                        entry+exit for Alice and Bob\n"
        "  LOCKOUT                      four unknown scans, then a block\n"
        "  UNBLOCK                      clear the denial table (test only)\n"
        "Cards: blue 01020304 Alice, green 11223344 Bob,\n"
        "       yellow 55667788 Carol (starts inside), red AABBCCDD unknown.\n"
        "Click a reader, then b/g/y/r to pick a card and t to tap.\n");
}

static void upper_copy(char *s)
{
    for (; *s != '\0'; s++) {
        *s = (char)toupper((unsigned char)*s);
    }
}

static void handle_line(char *line)
{
    char *arg;
    char uid[UID_STR_LEN];

    while (*line == ' ') {
        line++;
    }
    arg = strchr(line, ' ');
    if (arg != NULL) {
        *arg = '\0';
        arg++;
        while (*arg == ' ') {
            arg++;
        }
        if (*arg == '\0') {
            arg = NULL;
        }
    }
    upper_copy(line);

    if (line[0] == '\0') {
        return;
    }
    if (strcmp(line, "HELP") == 0) {
        print_help();
    } else if (strcmp(line, "STATUS") == 0) {
        console_printf("building=%s emergency=%s exit_allowed=%s inside=%d\n",
                       is_building_open() ? "OPEN" : "CLOSED", emergency_active() ? "ON" : "OFF",
                       exit_allowed() ? "YES" : "NO", get_building_count());
        storage_print_users();
        access_print_blocks();
    } else if (strcmp(line, "USERS") == 0) {
        storage_print_users();
    } else if (strcmp(line, "OPEN") == 0) {
        stub_set_building_open(true);
    } else if (strcmp(line, "CLOSED") == 0) {
        stub_set_building_open(false);
    } else if (strcmp(line, "EMERGENCY") == 0 && arg != NULL) {
        upper_copy(arg);
        if (strcmp(arg, "ON") == 0) {
            stub_set_emergency(true);
        } else if (strcmp(arg, "OFF") == 0) {
            stub_set_emergency(false);
        } else {
            console_printf("Use EMERGENCY ON or EMERGENCY OFF\n");
        }
    } else if ((strcmp(line, "IN") == 0 || strcmp(line, "OUT") == 0) && arg != NULL) {
        if (!uid_normalize(arg, uid, sizeof(uid))) {
            console_printf("UID must be 8 or 14 hex digits. Colons are optional.\n");
        } else {
            scan_queue_post(uid, strcmp(line, "OUT") == 0 ? DIR_EXIT : DIR_ENTRY);
        }
    } else if (strcmp(line, "RAPID") == 0) {
        int before = get_building_count();
        scan_queue_post("01020304", DIR_ENTRY);
        scan_queue_post("11223344", DIR_ENTRY);
        scan_queue_post("01020304", DIR_EXIT);
        scan_queue_post("11223344", DIR_EXIT);
        vTaskDelay(pdMS_TO_TICKS(200));
        console_printf("RAPID count %d -> %d (fresh boot stays the same)\n", before, get_building_count());
    } else if (strcmp(line, "LOCKOUT") == 0) {
        for (int i = 0; i < 4; i++) {
            scan_queue_post("AABBCCDD", DIR_ENTRY);
        }
    } else if (strcmp(line, "UNBLOCK") == 0) {
        access_clear_blocks();
    } else {
        console_printf("Unknown command. Type HELP.\n");
    }
}

void console_task(void *arg)
{
    char line[80];

    (void)arg;
    print_help();
    console_printf("> ");
    for (;;) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        line[strcspn(line, "\r\n")] = '\0';
        handle_line(line);
        console_printf("> ");
    }
}
