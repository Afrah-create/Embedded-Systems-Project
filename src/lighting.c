#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define ROOM_COUNT 2
#define ROOM_TIMEOUT_SECONDS 10
#define ROOM_1_LIGHT_PIN 12
#define ROOM_2_LIGHT_PIN 3

typedef struct {
    int occupied;
    int light_on;
    int64_t last_motion_us;
    int64_t light_started_us;
    int64_t total_light_us;
} room_t;

static room_t rooms[ROOM_COUNT];
static const int light_pins[ROOM_COUNT] = {
    ROOM_1_LIGHT_PIN,
    ROOM_2_LIGHT_PIN
};
static SemaphoreHandle_t room_lock;

static void lighting_task(void *arg)
{
    int room;

    (void)arg;

    while (1) {
        int64_t now = esp_timer_get_time();

        xSemaphoreTake(room_lock, portMAX_DELAY);
        for (room = 0; room < ROOM_COUNT; room++) {
            int64_t idle_us = now - rooms[room].last_motion_us;

            if (rooms[room].occupied == 1
                && idle_us >= ROOM_TIMEOUT_SECONDS * 1000000LL) {
                rooms[room].occupied = 0;
                rooms[room].light_on = 0;
                rooms[room].total_light_us += now - rooms[room].light_started_us;
                gpio_set_level(light_pins[room], 0);
                printf("Room %d vacant. Light off.\n", room + 1);
            }
        }
        xSemaphoreGive(room_lock);

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void lighting_start(void)
{
    gpio_config_t lights;

    memset(rooms, 0, sizeof(rooms));
    room_lock = xSemaphoreCreateMutex();

    memset(&lights, 0, sizeof(lights));
    lights.pin_bit_mask = (1ULL << ROOM_1_LIGHT_PIN)
                        | (1ULL << ROOM_2_LIGHT_PIN);
    lights.mode = GPIO_MODE_OUTPUT;
    gpio_config(&lights);
    gpio_set_level(ROOM_1_LIGHT_PIN, 0);
    gpio_set_level(ROOM_2_LIGHT_PIN, 0);

    xTaskCreate(lighting_task, "lighting", 3072, NULL, 3, NULL);
    printf("Room lighting ready. A is Room 1, B is Room 2, C shows status.\n");
}

void lighting_motion(int room)
{
    int64_t now;

    if (room < 0 || room >= ROOM_COUNT) {
        return;
    }

    now = esp_timer_get_time();
    xSemaphoreTake(room_lock, portMAX_DELAY);
    rooms[room].occupied = 1;
    rooms[room].last_motion_us = now;

    if (rooms[room].light_on == 0) {
        rooms[room].light_on = 1;
        rooms[room].light_started_us = now;
        gpio_set_level(light_pins[room], 1);
        printf("Room %d occupied. Light on.\n", room + 1);
    } else {
        printf("Room %d motion detected. Timeout refreshed.\n", room + 1);
    }
    xSemaphoreGive(room_lock);
}

void lighting_print_status(void)
{
    int room;
    int64_t now = esp_timer_get_time();

    xSemaphoreTake(room_lock, portMAX_DELAY);
    printf("\nRoom lighting status\n");
    for (room = 0; room < ROOM_COUNT; room++) {
        int64_t total_us = rooms[room].total_light_us;

        if (rooms[room].light_on == 1) {
            total_us += now - rooms[room].light_started_us;
        }

        printf("  Room %d: %s, light %s, on for %lld seconds\n",
               room + 1,
               rooms[room].occupied == 1 ? "occupied" : "vacant",
               rooms[room].light_on == 1 ? "on" : "off",
               (long long)(total_us / 1000000LL));
    }
    xSemaphoreGive(room_lock);
}
