/*
 * storage.c
 * Saves cards, who is inside, the open/emergency flags, and the access log
 * on the SD card. storage_lock and storage_unlock also keep the SD card and
 * the RFID readers off the SPI bus at the same time.
 */

#include <stdio.h>
#include <string.h>

#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"

#define PIN_SD_CS  4

#define USERS_PATH     "/sdcard/users.txt"
#define INSIDE_PATH    "/sdcard/occupancy.txt"
#define LOG_PATH       "/sdcard/access.log"
#define SCHEDULE_PATH  "/sdcard/schedule.txt"

#define MAX_USERS   20
#define MAX_INSIDE  32

typedef struct {
    char uid[16];
    char name[20];
    char role[8];
} user_t;

static SemaphoreHandle_t file_lock;

void storage_set_inside(char *name, char *uid, int inside);

static void write_text_file(char *path, char *text)
{
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        printf("Could not write %s\n", path);
        return;
    }
    fputs(text, file);
    fclose(file);
}

static int file_exists(char *path)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return 0;
    }
    fclose(file);
    return 1;
}

int storage_mount(void)
{
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    sdmmc_card_t *card = NULL;
    esp_err_t err;

    file_lock = xSemaphoreCreateMutex();
    slot.gpio_cs = PIN_SD_CS;
    slot.host_id = host.slot;

    err = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mount_config, &card);
    if (err != ESP_OK) {
        printf("SD card mount failed: %s\n", esp_err_to_name(err));
        return 0;
    }

    printf("SD card ready\n");
    return 1;
}

void storage_create_defaults(void)
{
    if (file_exists(USERS_PATH) == 0) {
        write_text_file(USERS_PATH,
                        "01020304 Alice user\n"
                        "11223344 Bob user\n"
                        "55667788 Carol admin\n");
        printf("Created default cards\n");
    }
    if (file_exists(SCHEDULE_PATH) == 0) {
        write_text_file(SCHEDULE_PATH, "1 0\n");
        printf("Building starts open, emergency off\n");
    }
    if (file_exists(INSIDE_PATH) == 0) {
        write_text_file(INSIDE_PATH, "Carol 55667788\n");
        printf("Carol starts inside\n");
    }
}

void storage_lock(void)
{
    xSemaphoreTake(file_lock, portMAX_DELAY);
}

void storage_unlock(void)
{
    xSemaphoreGive(file_lock);
}

static int load_users(user_t *users)
{
    FILE *file;
    char line[64];
    int count = 0;

    file = fopen(USERS_PATH, "r");
    if (file == NULL) {
        return 0;
    }

    while (count < MAX_USERS && fgets(line, sizeof(line), file) != NULL) {
        if (sscanf(line, "%15s %19s %7s", users[count].uid, users[count].name, users[count].role) == 3) {
            count++;
        }
    }
    fclose(file);
    return count;
}

static void save_users(user_t *users, int count)
{
    FILE *file;
    int i;

    file = fopen(USERS_PATH, "w");
    if (file == NULL) {
        printf("Could not update users file\n");
        return;
    }
    for (i = 0; i < count; i++) {
        fprintf(file, "%s %s %s\n", users[i].uid, users[i].name, users[i].role);
    }
    fclose(file);
}

/* Returns 1 when the card is registered. Copies the person's name and role. */
int storage_find_user(char *uid, char *name, int name_len, char *role, int role_len)
{
    user_t users[MAX_USERS];
    int count;
    int i;

    count = load_users(users);
    for (i = 0; i < count; i++) {
        if (strcmp(users[i].uid, uid) == 0) {
            snprintf(name, name_len, "%s", users[i].name);
            snprintf(role, role_len, "%s", users[i].role);
            return 1;
        }
    }
    return 0;
}

int storage_user_count(void)
{
    user_t users[MAX_USERS];
    return load_users(users);
}

/* index starts at 0. Returns 1 when that card exists. */
int storage_get_user(int index, char *uid, int uid_len, char *name, int name_len, char *role, int role_len)
{
    user_t users[MAX_USERS];
    int count;

    count = load_users(users);
    if (index < 0 || index >= count) {
        return 0;
    }
    snprintf(uid, uid_len, "%s", users[index].uid);
    snprintf(name, name_len, "%s", users[index].name);
    snprintf(role, role_len, "%s", users[index].role);
    return 1;
}

int storage_add_user(char *uid, char *name, char *role)
{
    user_t users[MAX_USERS];
    int count;
    int i;

    count = load_users(users);
    for (i = 0; i < count; i++) {
        if (strcmp(users[i].uid, uid) == 0) {
            printf("Card %s is already registered\n", uid);
            return 0;
        }
    }
    if (count >= MAX_USERS) {
        printf("User list is full\n");
        return 0;
    }

    snprintf(users[count].uid, sizeof(users[count].uid), "%s", uid);
    snprintf(users[count].name, sizeof(users[count].name), "%s", name);
    snprintf(users[count].role, sizeof(users[count].role), "%s", role);
    save_users(users, count + 1);
    return 1;
}

int storage_remove_user(char *uid)
{
    user_t users[MAX_USERS];
    user_t kept[MAX_USERS];
    int count;
    int keep;
    int i;
    int found = 0;

    count = load_users(users);
    keep = 0;
    for (i = 0; i < count; i++) {
        if (strcmp(users[i].uid, uid) == 0) {
            found = 1;
        } else {
            kept[keep] = users[i];
            keep++;
        }
    }
    if (found == 0) {
        printf("Card %s was not found\n", uid);
        return 0;
    }
    save_users(kept, keep);
    storage_set_inside("", uid, 0);
    return 1;
}

static int load_inside(user_t *people)
{
    FILE *file;
    char line[64];
    int count = 0;

    file = fopen(INSIDE_PATH, "r");
    if (file == NULL) {
        return 0;
    }

    while (count < MAX_INSIDE && fgets(line, sizeof(line), file) != NULL) {
        if (sscanf(line, "%19s %15s", people[count].name, people[count].uid) == 2) {
            count++;
        }
    }
    fclose(file);
    return count;
}

static void save_inside(user_t *people, int count)
{
    FILE *file;
    int i;

    file = fopen(INSIDE_PATH, "w");
    if (file == NULL) {
        printf("Could not update occupancy file\n");
        return;
    }
    for (i = 0; i < count; i++) {
        fprintf(file, "%s %s\n", people[i].name, people[i].uid);
    }
    fclose(file);
}

int storage_is_inside(char *uid)
{
    user_t people[MAX_INSIDE];
    int count;
    int i;

    count = load_inside(people);
    for (i = 0; i < count; i++) {
        if (strcmp(people[i].uid, uid) == 0) {
            return 1;
        }
    }
    return 0;
}

/* inside = 1 adds the person. inside = 0 removes the person. */
void storage_set_inside(char *name, char *uid, int inside)
{
    user_t people[MAX_INSIDE];
    user_t kept[MAX_INSIDE];
    int count;
    int keep;
    int i;

    count = load_inside(people);
    keep = 0;
    for (i = 0; i < count; i++) {
        if (strcmp(people[i].uid, uid) != 0) {
            kept[keep] = people[i];
            keep++;
        }
    }

    if (inside == 1 && keep < MAX_INSIDE) {
        snprintf(kept[keep].name, sizeof(kept[keep].name), "%s", name);
        snprintf(kept[keep].uid, sizeof(kept[keep].uid), "%s", uid);
        keep++;
    }
    save_inside(kept, keep);
}

int storage_count_inside(void)
{
    user_t people[MAX_INSIDE];
    return load_inside(people);
}

void storage_print_inside(void)
{
    user_t people[MAX_INSIDE];
    int count;
    int i;

    count = load_inside(people);
    if (count == 0) {
        printf("Nobody is inside.\n");
        return;
    }
    for (i = 0; i < count; i++) {
        printf("  %s  %s\n", people[i].name, people[i].uid);
    }
    printf("%d inside.\n", count);
}

void storage_print_users(void)
{
    user_t users[MAX_USERS];
    int count;
    int i;

    count = load_users(users);
    if (count == 0) {
        printf("No cards registered.\n");
        return;
    }
    for (i = 0; i < count; i++) {
        printf("  %d  %s  %s  %s\n", i + 1, users[i].uid, users[i].name, users[i].role);
    }
}

void storage_print_log(void)
{
    FILE *file;
    char line[96];
    int any = 0;

    file = fopen(LOG_PATH, "r");
    if (file == NULL) {
        printf("No log yet.\n");
        return;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        printf("%s", line);
        any = 1;
    }
    fclose(file);
    if (any == 0) {
        printf("No log yet.\n");
    }
}

/* schedule.txt is two numbers: building open (1/0), then emergency (1/0). */
static void read_flags(int *open, int *emergency)
{
    FILE *file;
    int got;

    *open = 1;
    *emergency = 0;
    file = fopen(SCHEDULE_PATH, "r");
    if (file == NULL) {
        return;
    }
    got = fscanf(file, "%d %d", open, emergency);
    fclose(file);
    if (got != 2) {
        *open = 1;
        *emergency = 0;
    }
}

static void write_flags(int open, int emergency)
{
    FILE *file = fopen(SCHEDULE_PATH, "w");
    if (file == NULL) {
        printf("Could not save schedule\n");
        return;
    }
    fprintf(file, "%d %d\n", open, emergency);
    fclose(file);
}

int storage_building_open(void)
{
    int open;
    int emergency;

    read_flags(&open, &emergency);
    if (open == 1) {
        return 1;
    }
    return 0;
}

int storage_emergency_on(void)
{
    int open;
    int emergency;

    read_flags(&open, &emergency);
    if (emergency == 1) {
        return 1;
    }
    return 0;
}

void storage_set_building_open(int open)
{
    int current_open;
    int emergency;

    read_flags(&current_open, &emergency);
    if (open == 1) {
        write_flags(1, emergency);
    } else {
        write_flags(0, emergency);
    }
}

void storage_set_emergency(int on)
{
    int open;
    int emergency;

    read_flags(&open, &emergency);
    if (on == 1) {
        write_flags(open, 1);
    } else {
        write_flags(open, 0);
    }
}

void storage_log(int seconds, char *door, char *name, char *uid, char *result)
{
    FILE *file = fopen(LOG_PATH, "a");
    if (file == NULL) {
        printf("Could not write log\n");
        return;
    }
    fprintf(file, "t=%05d %s %s %s %s\n", seconds, door, name, uid, result);
    fclose(file);
}
