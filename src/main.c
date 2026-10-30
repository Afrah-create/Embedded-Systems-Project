/*
 * main.c
 * Entrance access point: two RFID readers, two doors, LEDs, and a buzzer.
 * Cards, the inside list, and the access log are stored by storage.c.
 * The keypad chooses menu items by number. * cancels. # confirms a number.
 */

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define PIN_SPI_SCK   18
#define PIN_SPI_MOSI  23
#define PIN_SPI_MISO  19

#define PIN_RFID_ENTRY_CS   5
#define PIN_RFID_ENTRY_RST  21
#define PIN_RFID_EXIT_CS    16
#define PIN_RFID_EXIT_RST   17

#define PIN_DOOR_ENTRY  13
#define PIN_DOOR_EXIT   25
#define PIN_LED_ENTRY_G  26
#define PIN_LED_ENTRY_R  32
#define PIN_LED_EXIT_G   33
#define PIN_LED_EXIT_R   14
#define PIN_BUZZER       27
#define PIN_SD_CS         4

/* Rows are outputs. Columns are inputs. GPIO 34, 35, 36 and 39 have no
   internal pull-up, so the diagram has a 10k resistor on each column. */
#define PIN_KEY_ROW1  22
#define PIN_KEY_ROW2  15
#define PIN_KEY_ROW3   2
#define PIN_KEY_ROW4   0
#define PIN_KEY_COL1  34
#define PIN_KEY_COL2  35
#define PIN_KEY_COL3  36
#define PIN_KEY_COL4  39

#define ADMIN_PASSWORD  "1234"

#define REG_COMMAND      0x01
#define REG_COM_IRQ      0x04
#define REG_DIV_IRQ      0x05
#define REG_ERROR        0x06
#define REG_FIFO_DATA    0x09
#define REG_FIFO_LEVEL   0x0A
#define REG_BIT_FRAMING  0x0D
#define REG_MODE         0x11
#define REG_TX_CONTROL   0x14
#define REG_TX_ASK       0x15
#define REG_CRC_HI       0x21
#define REG_CRC_LO       0x22
#define REG_TIMER_MODE   0x2A
#define REG_TIMER_PRE    0x2B
#define REG_TIMER_HI     0x2C
#define REG_TIMER_LO     0x2D
#define REG_VERSION      0x37

int storage_mount(void);
void storage_create_defaults(void);
void storage_lock(void);
void storage_unlock(void);
int storage_find_user(char *uid, char *name, int name_len, char *role, int role_len);
int storage_is_inside(char *uid);
void storage_set_inside(char *name, char *uid, int inside);
int storage_count_inside(void);
void storage_print_inside(void);
void storage_print_users(void);
void storage_print_log(void);
int storage_user_count(void);
int storage_get_user(int index, char *uid, int uid_len, char *name, int name_len, char *role, int role_len);
int storage_add_user(char *uid, char *name, char *role);
int storage_remove_user(char *uid);
int storage_building_open(void);
int storage_emergency_on(void);
void storage_set_building_open(int open);
void storage_set_emergency(int on);
void storage_log(int seconds, char *door, char *name, char *uid, char *result);

static spi_device_handle_t entry_reader;
static spi_device_handle_t exit_reader;
static SemaphoreHandle_t beep_lock;
static SemaphoreHandle_t capture_lock;
static int storage_ready = 0;
static int admin_logged_in = 0;
static int capture_mode = 0;
static int capture_ready = 0;
static char capture_uid[16];

static const int key_rows[4] = {PIN_KEY_ROW1, PIN_KEY_ROW2, PIN_KEY_ROW3, PIN_KEY_ROW4};
static const int key_cols[4] = {PIN_KEY_COL1, PIN_KEY_COL2, PIN_KEY_COL3, PIN_KEY_COL4};
static const char key_map[4][4] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}
};

static void spi_transfer(spi_device_handle_t dev, uint8_t *tx, uint8_t *rx, int bytes)
{
    uint8_t tx_copy[24] __attribute__((aligned(4)));
    uint8_t rx_copy[24] __attribute__((aligned(4)));
    spi_transaction_t transfer;
    int i;

    memset(&transfer, 0, sizeof(transfer));
    memset(tx_copy, 0, sizeof(tx_copy));
    memset(rx_copy, 0, sizeof(rx_copy));

    for (i = 0; i < bytes; i++) {
        tx_copy[i] = tx[i];
    }

    transfer.length = bytes * 8;
    transfer.rxlength = bytes * 8;

    if (bytes <= 4) {
        transfer.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
        for (i = 0; i < bytes; i++) {
            transfer.tx_data[i] = tx_copy[i];
        }
    } else {
        transfer.tx_buffer = tx_copy;
        transfer.rx_buffer = rx_copy;
    }

    spi_device_polling_transmit(dev, &transfer);

    if (rx == NULL) {
        return;
    }
    if (bytes <= 4) {
        for (i = 0; i < bytes; i++) {
            rx[i] = transfer.rx_data[i];
        }
    } else {
        for (i = 0; i < bytes; i++) {
            rx[i] = rx_copy[i];
        }
    }
}

static void write_reg(spi_device_handle_t dev, uint8_t reg, uint8_t value)
{
    uint8_t tx[2];
    tx[0] = (reg << 1) & 0x7E;
    tx[1] = value;
    spi_transfer(dev, tx, NULL, 2);
}

static uint8_t read_reg(spi_device_handle_t dev, uint8_t reg)
{
    uint8_t tx[2];
    uint8_t rx[2];
    tx[0] = ((reg << 1) & 0x7E) | 0x80;
    tx[1] = 0;
    spi_transfer(dev, tx, rx, 2);
    return rx[1];
}

static void write_fifo(spi_device_handle_t dev, uint8_t *data, int count)
{
    uint8_t tx[20];
    int i;

    tx[0] = (REG_FIFO_DATA << 1) & 0x7E;
    for (i = 0; i < count; i++) {
        tx[i + 1] = data[i];
    }
    spi_transfer(dev, tx, NULL, count + 1);
}

static void read_fifo(spi_device_handle_t dev, uint8_t *data, int count)
{
    uint8_t tx[20];
    uint8_t rx[20];
    uint8_t address;
    int i;

    address = ((REG_FIFO_DATA << 1) & 0x7E) | 0x80;
    memset(tx, 0, sizeof(tx));
    tx[0] = address;
    for (i = 1; i < count; i++) {
        tx[i] = address;
    }

    spi_transfer(dev, tx, rx, count + 1);
    for (i = 0; i < count; i++) {
        data[i] = rx[i + 1];
    }
}

static int talk_to_card(spi_device_handle_t dev, uint8_t *send, int send_len,
                        uint8_t *reply, int reply_max, uint8_t last_bits)
{
    int i;
    int count;
    uint8_t irq;
    uint8_t value;
    int got_reply = 0;

    write_reg(dev, REG_COMMAND, 0x00);
    write_reg(dev, REG_COM_IRQ, 0x7F);
    write_reg(dev, REG_FIFO_LEVEL, 0x80);
    write_fifo(dev, send, send_len);
    write_reg(dev, REG_BIT_FRAMING, last_bits);
    write_reg(dev, REG_COMMAND, 0x0C);

    value = read_reg(dev, REG_BIT_FRAMING);
    write_reg(dev, REG_BIT_FRAMING, value | 0x80);

    for (i = 0; i < 2000; i++) {
        irq = read_reg(dev, REG_COM_IRQ);
        if ((irq & 0x30) != 0) {
            got_reply = 1;
            break;
        }
        if ((irq & 0x01) != 0) {
            break;
        }
    }

    value = read_reg(dev, REG_BIT_FRAMING);
    write_reg(dev, REG_BIT_FRAMING, value & 0x7F);

    if (got_reply == 0) {
        return 0;
    }
    if ((read_reg(dev, REG_ERROR) & 0x13) != 0) {
        return 0;
    }

    count = read_reg(dev, REG_FIFO_LEVEL);
    if (count <= 0 || count > reply_max) {
        return 0;
    }
    read_fifo(dev, reply, count);
    return count;
}

static void turn_antenna_on(spi_device_handle_t dev)
{
    uint8_t value = read_reg(dev, REG_TX_CONTROL);
    if ((value & 0x03) != 0x03) {
        write_reg(dev, REG_TX_CONTROL, value | 0x03);
    }
}

static void sleep_card(spi_device_handle_t dev)
{
    uint8_t data[4];
    uint8_t reply[4];
    int i;

    data[0] = 0x50;
    data[1] = 0x00;

    write_reg(dev, REG_COMMAND, 0x00);
    write_reg(dev, REG_DIV_IRQ, 0x04);
    write_reg(dev, REG_FIFO_LEVEL, 0x80);
    write_fifo(dev, data, 2);
    write_reg(dev, REG_COMMAND, 0x03);

    for (i = 0; i < 2000; i++) {
        if ((read_reg(dev, REG_DIV_IRQ) & 0x04) != 0) {
            data[2] = read_reg(dev, REG_CRC_LO);
            data[3] = read_reg(dev, REG_CRC_HI);
            talk_to_card(dev, data, 4, reply, 4, 0);
            return;
        }
    }
}

/* Returns 1 when a card was read. uid looks like "01020304". */
static int read_one_reader(spi_device_handle_t dev, char *uid, int uid_len)
{
    uint8_t wake_up;
    uint8_t reply[8];
    uint8_t ask[2];
    int count;
    uint8_t check;

    if (dev == NULL) {
        return 0;
    }

    wake_up = 0x26;
    count = talk_to_card(dev, &wake_up, 1, reply, 4, 0x07);
    if (count == 0) {
        return 0;
    }

    ask[0] = 0x93;
    ask[1] = 0x20;
    count = talk_to_card(dev, ask, 2, reply, 8, 0);
    if (count < 5) {
        return 0;
    }

    check = reply[0] ^ reply[1] ^ reply[2] ^ reply[3];
    if (check != reply[4]) {
        return 0;
    }

    snprintf(uid, uid_len, "%02X%02X%02X%02X", reply[0], reply[1], reply[2], reply[3]);
    sleep_card(dev);
    return 1;
}

static void start_one_reader(spi_device_handle_t *dev, int cs_pin, int rst_pin, char *name)
{
    spi_device_interface_config_t config;
    uint8_t version;

    gpio_set_direction(rst_pin, GPIO_MODE_OUTPUT);
    gpio_set_level(rst_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(rst_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    memset(&config, 0, sizeof(config));
    config.clock_speed_hz = 1000000;
    config.mode = 0;
    config.spics_io_num = cs_pin;
    config.queue_size = 1;
    spi_bus_add_device(SPI2_HOST, &config, dev);

    write_reg(*dev, REG_COMMAND, 0x0F);
    vTaskDelay(pdMS_TO_TICKS(50));
    write_reg(*dev, REG_TIMER_MODE, 0x80);
    write_reg(*dev, REG_TIMER_PRE, 0xA9);
    write_reg(*dev, REG_TIMER_HI, 0x03);
    write_reg(*dev, REG_TIMER_LO, 0xE8);
    write_reg(*dev, REG_TX_ASK, 0x40);
    write_reg(*dev, REG_MODE, 0x3D);
    turn_antenna_on(*dev);

    version = read_reg(*dev, REG_VERSION);
    printf("%s reader version 0x%02X\n", name, version);
}

/* Hold every chip-select high, then start the shared SPI bus. */
static void spi_bus_init(void)
{
    gpio_config_t cs_pins;
    spi_bus_config_t bus;

    memset(&cs_pins, 0, sizeof(cs_pins));
    cs_pins.pin_bit_mask = (1ULL << PIN_RFID_ENTRY_CS) | (1ULL << PIN_RFID_EXIT_CS) | (1ULL << PIN_SD_CS);
    cs_pins.mode = GPIO_MODE_OUTPUT;
    gpio_config(&cs_pins);
    gpio_set_level(PIN_RFID_ENTRY_CS, 1);
    gpio_set_level(PIN_RFID_EXIT_CS, 1);
    gpio_set_level(PIN_SD_CS, 1);

    memset(&bus, 0, sizeof(bus));
    bus.mosi_io_num = PIN_SPI_MOSI;
    bus.miso_io_num = PIN_SPI_MISO;
    bus.sclk_io_num = PIN_SPI_SCK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4000;
    spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    gpio_set_pull_mode(PIN_SPI_MISO, GPIO_PULLUP_ONLY);
}

static void rfid_start(void)
{
    start_one_reader(&entry_reader, PIN_RFID_ENTRY_CS, PIN_RFID_ENTRY_RST, "Entry");
    start_one_reader(&exit_reader, PIN_RFID_EXIT_CS, PIN_RFID_EXIT_RST, "Exit");
}

static int rfid_read_entry(char *uid, int uid_len)
{
    int ok;

    storage_lock();
    ok = read_one_reader(entry_reader, uid, uid_len);
    storage_unlock();
    return ok;
}

static int rfid_read_exit(char *uid, int uid_len)
{
    int ok;

    storage_lock();
    ok = read_one_reader(exit_reader, uid, uid_len);
    storage_unlock();
    return ok;
}

static uint32_t angle_to_duty(int angle)
{
    uint32_t pulse_us;

    if (angle < 0) {
        angle = 0;
    }
    if (angle > 180) {
        angle = 180;
    }
    pulse_us = 544 + ((uint32_t)angle * (2400 - 544)) / 180;
    return (pulse_us * ((1U << 14) - 1U)) / 20000U;
}

static void set_door_angle(ledc_channel_t channel, int angle)
{
    ledc_set_duty_and_update(LEDC_LOW_SPEED_MODE, channel, angle_to_duty(angle), 0);
}

static void beep(int times)
{
    int i;

    xSemaphoreTake(beep_lock, portMAX_DELAY);
    for (i = 0; i < times; i++) {
        ledc_set_duty_and_update(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, 512, 0);
        vTaskDelay(pdMS_TO_TICKS(120));
        ledc_set_duty_and_update(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    xSemaphoreGive(beep_lock);
}

static void start_doors_and_leds(void)
{
    ledc_timer_config_t servo_timer;
    ledc_timer_config_t beep_timer;
    ledc_channel_config_t entry_door;
    ledc_channel_config_t exit_door;
    ledc_channel_config_t buzzer;
    gpio_config_t leds;

    memset(&leds, 0, sizeof(leds));
    leds.pin_bit_mask = (1ULL << PIN_LED_ENTRY_G) | (1ULL << PIN_LED_ENTRY_R)
                      | (1ULL << PIN_LED_EXIT_G) | (1ULL << PIN_LED_EXIT_R);
    leds.mode = GPIO_MODE_OUTPUT;
    gpio_config(&leds);
    gpio_set_level(PIN_LED_ENTRY_G, 0);
    gpio_set_level(PIN_LED_ENTRY_R, 0);
    gpio_set_level(PIN_LED_EXIT_G, 0);
    gpio_set_level(PIN_LED_EXIT_R, 0);

    memset(&servo_timer, 0, sizeof(servo_timer));
    servo_timer.speed_mode = LEDC_LOW_SPEED_MODE;
    servo_timer.duty_resolution = LEDC_TIMER_14_BIT;
    servo_timer.timer_num = LEDC_TIMER_0;
    servo_timer.freq_hz = 50;
    servo_timer.clk_cfg = LEDC_AUTO_CLK;

    memset(&beep_timer, 0, sizeof(beep_timer));
    beep_timer.speed_mode = LEDC_LOW_SPEED_MODE;
    beep_timer.duty_resolution = LEDC_TIMER_10_BIT;
    beep_timer.timer_num = LEDC_TIMER_1;
    beep_timer.freq_hz = 2000;
    beep_timer.clk_cfg = LEDC_AUTO_CLK;

    ledc_timer_config(&servo_timer);
    ledc_timer_config(&beep_timer);

    memset(&entry_door, 0, sizeof(entry_door));
    entry_door.gpio_num = PIN_DOOR_ENTRY;
    entry_door.speed_mode = LEDC_LOW_SPEED_MODE;
    entry_door.channel = LEDC_CHANNEL_0;
    entry_door.timer_sel = LEDC_TIMER_0;
    entry_door.duty = angle_to_duty(0);

    memset(&exit_door, 0, sizeof(exit_door));
    exit_door.gpio_num = PIN_DOOR_EXIT;
    exit_door.speed_mode = LEDC_LOW_SPEED_MODE;
    exit_door.channel = LEDC_CHANNEL_1;
    exit_door.timer_sel = LEDC_TIMER_0;
    exit_door.duty = angle_to_duty(0);

    memset(&buzzer, 0, sizeof(buzzer));
    buzzer.gpio_num = PIN_BUZZER;
    buzzer.speed_mode = LEDC_LOW_SPEED_MODE;
    buzzer.channel = LEDC_CHANNEL_2;
    buzzer.timer_sel = LEDC_TIMER_1;
    buzzer.duty = 0;

    ledc_channel_config(&entry_door);
    ledc_channel_config(&exit_door);
    ledc_channel_config(&buzzer);
}

static void show_door_result(int is_entry, int granted)
{
    int green;
    int red;
    ledc_channel_t channel;

    if (is_entry == 1) {
        green = PIN_LED_ENTRY_G;
        red = PIN_LED_ENTRY_R;
        channel = LEDC_CHANNEL_0;
    } else {
        green = PIN_LED_EXIT_G;
        red = PIN_LED_EXIT_R;
        channel = LEDC_CHANNEL_1;
    }

    if (granted == 1) {
        gpio_set_level(red, 0);
        gpio_set_level(green, 1);
        set_door_angle(channel, 90);
        beep(1);
        vTaskDelay(pdMS_TO_TICKS(2800));
        set_door_angle(channel, 0);
        gpio_set_level(green, 0);
    } else {
        gpio_set_level(green, 0);
        gpio_set_level(red, 1);
        beep(3);
        gpio_set_level(red, 0);
    }
}

/*
 * is_entry is 1 at the entry door and 0 at the exit door.
 * Order of the checks:
 *   1. the card must be registered
 *   2. entry is refused when the building is closed
 *   3. entry is refused when the person is already inside
 *   4. exit is refused when the building is closed, unless emergency is on
 *   5. exit is refused when the person is not inside
 */
static void check_card(int is_entry, char *uid)
{
    char name[20];
    char role[8];
    char *door_name;
    char *result;
    int known;
    int open;
    int emergency;
    int already_inside;
    int granted;
    int count;
    int seconds;

    strcpy(name, "unknown");
    granted = 0;
    result = "UNKNOWN";

    if (storage_ready == 0) {
        printf("SD card not ready\n");
        show_door_result(is_entry, 0);
        return;
    }

    storage_lock();
    known = storage_find_user(uid, name, sizeof(name), role, sizeof(role));
    open = storage_building_open();
    emergency = storage_emergency_on();
    already_inside = storage_is_inside(uid);

    if (known == 0) {
        result = "UNKNOWN";
    } else if (is_entry == 1 && open == 0) {
        result = "CLOSED";
    } else if (is_entry == 1 && already_inside == 1) {
        result = "ALREADY_IN";
    } else if (is_entry == 0 && open == 0 && emergency == 0) {
        result = "CLOSED";
    } else if (is_entry == 0 && already_inside == 0) {
        result = "NOT_INSIDE";
    } else {
        result = "GRANTED";
        granted = 1;
        storage_set_inside(name, uid, is_entry);
    }

    if (is_entry == 1) {
        door_name = "ENTRY";
    } else {
        door_name = "EXIT";
    }
    seconds = (int)(esp_timer_get_time() / 1000000);
    storage_log(seconds, door_name, name, uid, result);
    count = storage_count_inside();
    storage_unlock();

    printf("%s %s %s -> %s (inside %d)\n", door_name, name, uid, result, count);
    show_door_result(is_entry, granted);
}

static void entry_task(void *arg)
{
    char uid[16];
    char last_uid[16];
    TickType_t last_time;

    (void)arg;
    last_uid[0] = '\0';
    last_time = 0;

    while (1) {
        if (rfid_read_entry(uid, sizeof(uid)) == 1) {
            TickType_t now = xTaskGetTickCount();
            int same_card = 0;
            int captured = 0;

            xSemaphoreTake(capture_lock, portMAX_DELAY);
            if (capture_mode == 1) {
                snprintf(capture_uid, sizeof(capture_uid), "%s", uid);
                capture_ready = 1;
                capture_mode = 0;
                captured = 1;
            }
            xSemaphoreGive(capture_lock);

            if (strcmp(uid, last_uid) == 0 && (now - last_time) < pdMS_TO_TICKS(3000)) {
                same_card = 1;
            }
            strcpy(last_uid, uid);
            last_time = now;
            if (captured == 0 && same_card == 0) {
                check_card(1, uid);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void exit_task(void *arg)
{
    char uid[16];
    char last_uid[16];
    TickType_t last_time;

    (void)arg;
    last_uid[0] = '\0';
    last_time = 0;

    while (1) {
        if (rfid_read_exit(uid, sizeof(uid)) == 1) {
            TickType_t now = xTaskGetTickCount();
            int same_card = 0;
            if (strcmp(uid, last_uid) == 0 && (now - last_time) < pdMS_TO_TICKS(3000)) {
                same_card = 1;
            }
            strcpy(last_uid, uid);
            last_time = now;
            if (same_card == 0) {
                check_card(0, uid);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void start_keypad(void)
{
    gpio_config_t rows;
    gpio_config_t cols;
    int i;

    memset(&rows, 0, sizeof(rows));
    rows.pin_bit_mask = (1ULL << PIN_KEY_ROW1) | (1ULL << PIN_KEY_ROW2)
                      | (1ULL << PIN_KEY_ROW3) | (1ULL << PIN_KEY_ROW4);
    rows.mode = GPIO_MODE_OUTPUT;
    gpio_config(&rows);
    for (i = 0; i < 4; i++) {
        gpio_set_level(key_rows[i], 1);
    }

    memset(&cols, 0, sizeof(cols));
    cols.pin_bit_mask = (1ULL << PIN_KEY_COL1) | (1ULL << PIN_KEY_COL2)
                      | (1ULL << PIN_KEY_COL3) | (1ULL << PIN_KEY_COL4);
    cols.mode = GPIO_MODE_INPUT;
    gpio_config(&cols);
}

static char scan_keypad(void)
{
    int row;
    int col;

    for (row = 0; row < 4; row++) {
        gpio_set_level(key_rows[row], 0);
        for (col = 0; col < 4; col++) {
            if (gpio_get_level(key_cols[col]) == 0) {
                vTaskDelay(pdMS_TO_TICKS(15));
                if (gpio_get_level(key_cols[col]) == 0) {
                    gpio_set_level(key_rows[row], 1);
                    return key_map[row][col];
                }
            }
        }
        gpio_set_level(key_rows[row], 1);
    }
    return '\0';
}

/* One press returns one character. The key must be released first. */
static char wait_key(void)
{
    char key;

    while (1) {
        key = scan_keypad();
        if (key != '\0') {
            while (scan_keypad() != '\0') {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            return key;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void show_main_menu(void)
{
    printf("\nCoCIS entrance\n");
    printf("  1  Status\n");
    printf("  2  Who is inside\n");
    printf("  3  Access log\n");
    printf("  4  Registered cards\n");
    printf("  5  Admin\n");
    printf("Press a number. * shows this menu again.\n");
    fflush(stdout);
}

static void show_admin_menu(void)
{
    printf("\nAdmin\n");
    printf("  1  Open the building\n");
    printf("  2  Close the building\n");
    printf("  3  Emergency on\n");
    printf("  4  Emergency off\n");
    printf("  5  Add a card\n");
    printf("  6  Remove a card\n");
    printf("  7  Log out\n");
    printf("Press a number. * goes back.\n");
    fflush(stdout);
}

static void show_status(void)
{
    int open;
    int emergency;
    int count;

    if (storage_ready == 0) {
        printf("SD card not ready\n");
        return;
    }

    storage_lock();
    open = storage_building_open();
    emergency = storage_emergency_on();
    count = storage_count_inside();
    storage_unlock();

    if (open == 1) {
        printf("Building: open\n");
    } else {
        printf("Building: closed\n");
    }
    if (emergency == 1) {
        printf("Emergency: on (exit allowed)\n");
    } else {
        printf("Emergency: off\n");
    }
    printf("People inside: %d\n", count);
    if (admin_logged_in == 1) {
        printf("Admin: logged in\n");
    } else {
        printf("Admin: logged out\n");
    }
}

static void menu_task(void *arg)
{
    int mode;
    char digits[8];
    int digit_count;
    char new_uid[16];
    char key;
    char last_key;

    (void)arg;
    mode = 0;
    digit_count = 0;
    new_uid[0] = '\0';
    last_key = '\0';
    show_main_menu();

    while (1) {
        if (mode == 4) {
            int ready = 0;

            xSemaphoreTake(capture_lock, portMAX_DELAY);
            if (capture_ready == 1) {
                snprintf(new_uid, sizeof(new_uid), "%s", capture_uid);
                capture_ready = 0;
                ready = 1;
            }
            xSemaphoreGive(capture_lock);

            if (ready == 1) {
                printf("Card %s. Press 1 for user or 2 for admin. * cancels.\n", new_uid);
                mode = 5;
                continue;
            }

            key = scan_keypad();
            if (key == '*' && last_key != '*') {
                xSemaphoreTake(capture_lock, portMAX_DELAY);
                capture_mode = 0;
                capture_ready = 0;
                xSemaphoreGive(capture_lock);
                printf("Cancelled\n");
                mode = 1;
                show_admin_menu();
            }
            last_key = key;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        key = wait_key();
        printf("Key %c\n", key);

        if (mode == 2) {
            if (key == '*') {
                printf("Cancelled\n");
                mode = 0;
                show_main_menu();
                continue;
            }
            if (key == '#') {
                digits[digit_count] = '\0';
                if (strcmp(digits, ADMIN_PASSWORD) == 0) {
                    admin_logged_in = 1;
                    printf("Admin logged in\n");
                    mode = 1;
                    show_admin_menu();
                } else {
                    printf("Wrong password\n");
                    mode = 0;
                    show_main_menu();
                }
                continue;
            }
            if (key >= '0' && key <= '9' && digit_count < 6) {
                digits[digit_count] = key;
                digit_count++;
                printf("*\n");
            }
            continue;
        }

        if (mode == 3) {
            int index;
            char uid[16];
            char name[20];
            char role[8];

            if (key == '*') {
                printf("Cancelled\n");
                mode = 1;
                show_admin_menu();
                continue;
            }
            if (key == '#') {
                digits[digit_count] = '\0';
                index = 0;
                if (digit_count > 0) {
                    sscanf(digits, "%d", &index);
                }
                storage_lock();
                if (storage_get_user(index - 1, uid, sizeof(uid), name, sizeof(name), role, sizeof(role)) == 1
                    && storage_remove_user(uid) == 1) {
                    printf("Removed %s %s\n", name, uid);
                } else {
                    printf("No card at that number\n");
                }
                storage_unlock();
                mode = 1;
                show_admin_menu();
                continue;
            }
            if (key >= '0' && key <= '9' && digit_count < 2) {
                digits[digit_count] = key;
                digit_count++;
                printf("%c\n", key);
            }
            continue;
        }

        if (mode == 5) {
            char name[20];
            char *role;
            int count;

            if (key == '*') {
                printf("Cancelled\n");
                mode = 1;
                show_admin_menu();
                continue;
            }
            if (key != '1' && key != '2') {
                printf("Press 1 for user or 2 for admin\n");
                continue;
            }
            if (key == '2') {
                role = "admin";
            } else {
                role = "user";
            }
            storage_lock();
            count = storage_user_count();
            snprintf(name, sizeof(name), "User%d", count + 1);
            if (storage_add_user(new_uid, name, role) == 1) {
                printf("Saved %s %s %s\n", new_uid, name, role);
            }
            storage_unlock();
            mode = 1;
            show_admin_menu();
            continue;
        }

        if (mode == 1) {
            if (key == '*') {
                mode = 0;
                show_main_menu();
                continue;
            }
            if (key == '1') {
                storage_lock();
                storage_set_building_open(1);
                storage_unlock();
                printf("Building open\n");
            } else if (key == '2') {
                storage_lock();
                storage_set_building_open(0);
                storage_unlock();
                printf("Building closed\n");
            } else if (key == '3') {
                storage_lock();
                storage_set_emergency(1);
                storage_unlock();
                printf("Emergency on. Registered people who are inside can leave.\n");
            } else if (key == '4') {
                storage_lock();
                storage_set_emergency(0);
                storage_unlock();
                printf("Emergency off\n");
            } else if (key == '5') {
                xSemaphoreTake(capture_lock, portMAX_DELAY);
                capture_uid[0] = '\0';
                capture_ready = 0;
                capture_mode = 1;
                xSemaphoreGive(capture_lock);
                printf("Tap the new card on the ENTRY reader. * cancels.\n");
                mode = 4;
                last_key = '\0';
            } else if (key == '6') {
                storage_lock();
                storage_print_users();
                storage_unlock();
                printf("Card number, then #. * cancels.\n");
                digit_count = 0;
                mode = 3;
            } else if (key == '7') {
                admin_logged_in = 0;
                printf("Logged out\n");
                mode = 0;
                show_main_menu();
            } else {
                printf("Press 1 to 7\n");
            }
            continue;
        }

        if (key == '*') {
            show_main_menu();
            continue;
        }
        if (key == '1') {
            show_status();
        } else if (key == '2') {
            storage_lock();
            storage_print_inside();
            storage_unlock();
        } else if (key == '3') {
            storage_lock();
            storage_print_log();
            storage_unlock();
        } else if (key == '4') {
            storage_lock();
            storage_print_users();
            storage_unlock();
        } else if (key == '5') {
            printf("Password, then #. Password is %s. * cancels.\n", ADMIN_PASSWORD);
            digit_count = 0;
            mode = 2;
        } else {
            printf("Press 1 to 5\n");
        }
    }
}

void app_main(void)
{
    beep_lock = xSemaphoreCreateMutex();
    capture_lock = xSemaphoreCreateMutex();
    capture_uid[0] = '\0';

    start_keypad();
    start_doors_and_leds();
    spi_bus_init();

    if (storage_mount() == 1) {
        storage_ready = 1;
        storage_create_defaults();
    }

    rfid_start();
    printf("Entrance ready. Use the keypad.\n");
    printf("Cards: blue 01020304 Alice, green 11223344 Bob,\n");
    printf("       yellow 55667788 Carol (starts inside), red AABBCCDD unknown.\n");
    printf("Click a reader, then b/g/y/r to pick a card and t to tap.\n");

    xTaskCreate(entry_task, "entry", 8192, NULL, 5, NULL);
    xTaskCreate(exit_task, "exit", 8192, NULL, 5, NULL);
    xTaskCreate(menu_task, "menu", 8192, NULL, 4, NULL);
}
