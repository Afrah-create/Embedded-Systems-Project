#include "occupancy.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "storage_api.h"

/* Lock order: this state mutex, then the user-table mutex inside Role B.
   Never take them in the other order, and never call log_event while
   holding either one. A mutex is used instead of a binary semaphore because
   it guards data and FreeRTOS mutexes inherit priority. That keeps a slow
   low-priority reader of the count from blocking access_task indefinitely
   (priority inversion). A binary semaphore does not inherit priority. */

static SemaphoreHandle_t state_mu;
static int inside_count;

void occupancy_init(void)
{
    state_mu = xSemaphoreCreateMutex();
    inside_count = storage_count_inside();
}

int get_building_count(void)
{
    int count;

    if (state_mu == NULL) {
        return storage_count_inside();
    }
    xSemaphoreTake(state_mu, portMAX_DELAY);
    count = inside_count;
    xSemaphoreGive(state_mu);
    return count;
}

occ_result_t occupancy_set_inside(const char *uid, bool inside)
{
    user_record_t user;

    xSemaphoreTake(state_mu, portMAX_DELAY);
    if (!user_lookup(uid, &user)) {
        xSemaphoreGive(state_mu);
        return OCC_STORAGE;
    }
    if (user.inside == inside) {
        xSemaphoreGive(state_mu);
        return inside ? OCC_ALREADY_INSIDE : OCC_NOT_INSIDE;
    }
    if (!set_inside(uid, inside)) {
        xSemaphoreGive(state_mu);
        return OCC_STORAGE;
    }
    if (inside) {
        inside_count++;
    } else if (inside_count > 0) {
        inside_count--;
    }
    xSemaphoreGive(state_mu);
    return OCC_OK;
}
