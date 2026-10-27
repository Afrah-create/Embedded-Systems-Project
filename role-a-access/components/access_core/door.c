#include "door.h"

#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "console_api.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "occupancy.h"
#include "schedule_api.h"
#include "storage_api.h"
#include "system_bus.h"
#include "time_api.h"

static SemaphoreHandle_t door_mu;
static int64_t grant_until_us[2];
static int64_t close_at_us[2];
static int64_t red_off_at_us[2];
static int64_t beep_off_at_us;
static int64_t tail_suppress_us[2];

static const gpio_num_t green_pins[2] = {PIN_LED_IN_G, PIN_LED_OUT_G};
static const gpio_num_t red_pins[2] = {PIN_LED_IN_R, PIN_LED_OUT_R};
static const ledc_channel_t servo_channels[2] = {LEDC_CHANNEL_0, LEDC_CHANNEL_1};

static uint32_t angle_to_duty(int angle)
{
    uint32_t pulse_us;

    if (angle < 0) {
        angle = 0;
    }
    if (angle > 180) {
        angle = 180;
    }
    /* Same pulse range as the Arduino Servo library, which Wokwi's servo uses. */
    pulse_us = 544 + ((uint32_t)angle * (2400 - 544)) / 180;
    return (pulse_us * ((1U << 14) - 1U)) / 20000U;
}

static void servo_write(door_id_t door, int angle)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, servo_channels[door], angle_to_duty(angle));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, servo_channels[door]);
}

static void set_green(door_id_t door, int on)
{
    gpio_set_level(green_pins[door], on ? 1 : 0);
}

static void set_red(door_id_t door, int on)
{
    gpio_set_level(red_pins[door], on ? 1 : 0);
}

static void beep_start(int ms)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, 512);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
    beep_off_at_us = esp_timer_get_time() + (int64_t)ms * 1000;
}

static void note_grant(door_id_t door)
{
    xSemaphoreTake(door_mu, portMAX_DELAY);
    grant_until_us[door] = esp_timer_get_time() + ((int64_t)DOOR_OPEN_MS + 1000) * 1000;
    xSemaphoreGive(door_mu);
}

static void publish_door(door_id_t door, const char *state)
{
    char json[64];
    snprintf(json, sizeof(json), "{\"door\":\"%s\",\"state\":\"%s\"}", door == DOOR_EXIT ? "OUT" : "IN", state);
    publish("cocis/entrance/door", json);
}

static const char *door_name(door_id_t door)
{
    return door == DOOR_EXIT ? "OUT" : "IN";
}

static void apply_command(const door_command_t *cmd)
{
    int64_t now = esp_timer_get_time();

    if (cmd->door != DOOR_ENTRY && cmd->door != DOOR_EXIT) {
        return;
    }

    if (cmd->action == DOOR_CMD_GRANT) {
        set_green(cmd->door, 1);
        set_red(cmd->door, 0);
        red_off_at_us[cmd->door] = 0;
        servo_write(cmd->door, 90);
        close_at_us[cmd->door] = now + (int64_t)DOOR_OPEN_MS * 1000;
        note_grant(cmd->door);
        publish_door(cmd->door, "OPEN");
        return;
    }

    set_red(cmd->door, 1);
    if (cmd->action == DOOR_CMD_ALERT) {
        red_off_at_us[cmd->door] = now + 800000;
        beep_start(500);
    } else {
        red_off_at_us[cmd->door] = now + 400000;
        beep_start(150);
    }
}

static void service_timers(void)
{
    int64_t now = esp_timer_get_time();

    if (beep_off_at_us != 0 && now >= beep_off_at_us) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
        beep_off_at_us = 0;
    }

    for (int door = 0; door < 2; door++) {
        if (red_off_at_us[door] != 0 && now >= red_off_at_us[door]) {
            set_red((door_id_t)door, 0);
            red_off_at_us[door] = 0;
        }
        if (close_at_us[door] != 0 && now >= close_at_us[door]) {
            servo_write((door_id_t)door, 0);
            set_green((door_id_t)door, 0);
            close_at_us[door] = 0;
            publish_door((door_id_t)door, "CLOSED");
        }
    }
}

bool door_grant_is_fresh(door_id_t door)
{
    bool fresh = false;

    if ((door != DOOR_ENTRY && door != DOOR_EXIT) || door_mu == NULL) {
        return false;
    }
    xSemaphoreTake(door_mu, portMAX_DELAY);
    fresh = esp_timer_get_time() < grant_until_us[door];
    xSemaphoreGive(door_mu);
    return fresh;
}

bool door_post(const door_command_t *cmd)
{
    if (cmd == NULL || door_queue == NULL || xQueueSend(door_queue, cmd, 0) != pdTRUE) {
        console_printf("DOOR_CMD_DROPPED\n");
        return false;
    }
    return true;
}

static void IRAM_ATTR tailgate_isr(void *arg)
{
    tailgate_event_t ev = {.door = (door_id_t)(uintptr_t)arg};
    BaseType_t wake = pdFALSE;

    if (tailgate_queue != NULL) {
        xQueueSendFromISR(tailgate_queue, &ev, &wake);
    }
    if (wake == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void configure_output(gpio_num_t pin)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    gpio_set_level(pin, 0);
}

static void configure_servo_timer(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_14_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 50,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config_t buzz = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = 2000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const gpio_num_t servo_pins[2] = {PIN_SERVO_IN, PIN_SERVO_OUT};

    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ESP_ERROR_CHECK(ledc_timer_config(&buzz));

    for (int door = 0; door < 2; door++) {
        ledc_channel_config_t channel = {
            .gpio_num = servo_pins[door],
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = servo_channels[door],
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER_0,
            .duty = angle_to_duty(0),
            .hpoint = 0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&channel));
    }

    ledc_channel_config_t buzzer = {
        .gpio_num = PIN_BUZZER,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_2,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_1,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&buzzer));
}

void door_init(void)
{
    esp_err_t err;
    gpio_config_t tails = {
        .pin_bit_mask = (1ULL << PIN_TAIL_IN) | (1ULL << PIN_TAIL_OUT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };

    door_mu = xSemaphoreCreateMutex();
    configure_output(PIN_LED_IN_G);
    configure_output(PIN_LED_IN_R);
    configure_output(PIN_LED_OUT_G);
    configure_output(PIN_LED_OUT_R);
    configure_servo_timer();

    ESP_ERROR_CHECK(gpio_config(&tails));
    err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_TAIL_IN, tailgate_isr, (void *)(uintptr_t)DOOR_ENTRY));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_TAIL_OUT, tailgate_isr, (void *)(uintptr_t)DOOR_EXIT));
}

void door_task(void *arg)
{
    (void)arg;
    console_printf("door_task started\n");
    for (;;) {
        door_command_t cmd;
        if (xQueueReceive(door_queue, &cmd, pdMS_TO_TICKS(20)) == pdTRUE) {
            apply_command(&cmd);
        }
        service_timers();
    }
}

void tailgate_task(void *arg)
{
    (void)arg;
    console_printf("tailgate_task started\n");
    for (;;) {
        tailgate_event_t ev;
        tailgate_event_t junk;
        log_event_t log;
        char json[80];

        if (xQueueReceive(tailgate_queue, &ev, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        /* The interrupt only enqueues. This task owns the decision so the
           ISR stays short. The delay groups contact bounce into one event. */
        vTaskDelay(pdMS_TO_TICKS(30));
        while (xQueueReceive(tailgate_queue, &junk, 0) == pdTRUE) {
        }

        if (ev.door != DOOR_ENTRY && ev.door != DOOR_EXIT) {
            continue;
        }
        if (esp_timer_get_time() < tail_suppress_us[ev.door]) {
            continue;
        }
        tail_suppress_us[ev.door] = esp_timer_get_time() + 500000;

        if (door_grant_is_fresh(ev.door)) {
            console_printf("TAIL door=%s ignored (valid grant still open)\n", door_name(ev.door));
            continue;
        }

        memset(&log, 0, sizeof(log));
        log.time_s = time_now_s();
        log.time_synced = time_is_synced();
        strncpy(log.uid, "-", UID_STR_LEN - 1);
        log.direction = ev.door == DOOR_EXIT ? DIR_EXIT : DIR_ENTRY;
        log.result = ACCESS_RESULT_ALERT;
        log.reason = REASON_TAILGATE;
        log.inside_count = get_building_count();
        log_event(&log);

        snprintf(json, sizeof(json), "{\"event\":\"tailgate\",\"door\":\"%s\"}", door_name(ev.door));
        publish("cocis/entrance/alert", json);

        door_command_t cmd = {.door = ev.door, .action = DOOR_CMD_ALERT};
        door_post(&cmd);
    }
}
