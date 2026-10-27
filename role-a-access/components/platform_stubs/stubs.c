/* STUB. Coordinator: replace this file with Role B (storage) and Role D
   (schedule, time, publish). Keep the header names and the struct layouts
   in components/interfaces. Do not link this file together with the real
   implementations or the build will fail with duplicate symbols. */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "console_api.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "schedule_api.h"
#include "storage_api.h"
#include "time_api.h"

static SemaphoreHandle_t out_mu;
static SemaphoreHandle_t user_mu;
static SemaphoreHandle_t config_mu;
static bool building_open = true;
static bool emergency_on;

static user_record_t users[] = {
    {.uid = "01020304", .name = "Alice", .role = USER_ROLE_STUDENT, .inside = false},
    {.uid = "11223344", .name = "Bob", .role = USER_ROLE_STUDENT, .inside = false},
    {.uid = "55667788", .name = "Carol", .role = USER_ROLE_STAFF, .inside = true},
};

void stubs_init(void)
{
    out_mu = xSemaphoreCreateMutex();
    user_mu = xSemaphoreCreateMutex();
    config_mu = xSemaphoreCreateMutex();
}

void console_printf(const char *fmt, ...)
{
    char buf[320];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (out_mu != NULL) {
        xSemaphoreTake(out_mu, portMAX_DELAY);
    }
    fputs(buf, stdout);
    fflush(stdout);
    if (out_mu != NULL) {
        xSemaphoreGive(out_mu);
    }
}

bool user_lookup(const char *uid, user_record_t *out)
{
    bool found = false;

    if (uid == NULL || out == NULL) {
        return false;
    }
    xSemaphoreTake(user_mu, portMAX_DELAY);
    for (size_t i = 0; i < sizeof(users) / sizeof(users[0]); i++) {
        if (strcmp(users[i].uid, uid) == 0) {
            *out = users[i];
            found = true;
            break;
        }
    }
    xSemaphoreGive(user_mu);
    return found;
}

bool set_inside(const char *uid, bool inside)
{
    bool found = false;

    xSemaphoreTake(user_mu, portMAX_DELAY);
    for (size_t i = 0; i < sizeof(users) / sizeof(users[0]); i++) {
        if (strcmp(users[i].uid, uid) == 0) {
            users[i].inside = inside;
            found = true;
            break;
        }
    }
    xSemaphoreGive(user_mu);
    return found;
}

void log_event(const log_event_t *event)
{
    if (event == NULL) {
        return;
    }
    console_printf("LOG time_s=%lld sync=%d uid=%s dir=%s result=%s reason=%s count=%d\n",
                   (long long)event->time_s, event->time_synced ? 1 : 0,
                   event->uid[0] ? event->uid : "-", direction_name(event->direction),
                   access_result_name(event->result), access_reason_name(event->reason), event->inside_count);
}

int storage_count_inside(void)
{
    int count = 0;

    xSemaphoreTake(user_mu, portMAX_DELAY);
    for (size_t i = 0; i < sizeof(users) / sizeof(users[0]); i++) {
        if (users[i].inside) {
            count++;
        }
    }
    xSemaphoreGive(user_mu);
    return count;
}

void storage_print_users(void)
{
    user_record_t copy[3];
    size_t n = sizeof(users) / sizeof(users[0]);

    xSemaphoreTake(user_mu, portMAX_DELAY);
    memcpy(copy, users, sizeof(users));
    xSemaphoreGive(user_mu);

    for (size_t i = 0; i < n; i++) {
        console_printf("USER uid=%s name=%s role=%s inside=%s\n", copy[i].uid, copy[i].name,
                       user_role_name(copy[i].role), copy[i].inside ? "YES" : "NO");
    }
}

bool is_building_open(void)
{
    bool open;

    xSemaphoreTake(config_mu, portMAX_DELAY);
    open = building_open;
    xSemaphoreGive(config_mu);
    return open;
}

bool emergency_active(void)
{
    bool on;

    xSemaphoreTake(config_mu, portMAX_DELAY);
    on = emergency_on;
    xSemaphoreGive(config_mu);
    return on;
}

bool exit_allowed(void)
{
    bool allowed;

    xSemaphoreTake(config_mu, portMAX_DELAY);
    allowed = building_open || emergency_on;
    xSemaphoreGive(config_mu);
    return allowed;
}

void publish(const char *topic, const char *json)
{
    console_printf("PUB topic=%s payload=%s\n", topic != NULL ? topic : "-", json != NULL ? json : "{}");
}

void stub_set_building_open(bool open)
{
    xSemaphoreTake(config_mu, portMAX_DELAY);
    building_open = open;
    xSemaphoreGive(config_mu);
    console_printf("SCHEDULE building=%s\n", open ? "OPEN" : "CLOSED");
    publish("cocis/entrance/schedule", open ? "{\"open\":true}" : "{\"open\":false}");
}

void stub_set_emergency(bool on)
{
    xSemaphoreTake(config_mu, portMAX_DELAY);
    emergency_on = on;
    xSemaphoreGive(config_mu);
    console_printf("EMERGENCY %s\n", on ? "ON" : "OFF");
    publish("cocis/entrance/emergency", on ? "{\"active\":true}" : "{\"active\":false}");
}

int64_t time_now_s(void)
{
    return esp_timer_get_time() / 1000000;
}

bool time_is_synced(void)
{
    return false;
}
