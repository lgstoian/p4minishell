/**
 * @file lv_async.c
 *
 */

/*********************
 *      INCLUDES
 *********************/

#include "lv_async.h"
#include "lv_timer_private.h"
#include "../osal/lv_os.h"
#include "../stdlib/lv_mem.h"

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#endif

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

typedef struct _lv_async_info_t {
    lv_async_cb_t cb;
    void * user_data;
} lv_async_info_t;

/**********************
 *  STATIC PROTOTYPES
 **********************/

static void lv_async_timer_cb(lv_timer_t * timer);

#if defined(ESP_PLATFORM)
static lv_async_info_t * async_info_alloc(void);
static void async_info_free(lv_async_info_t * info);
#endif

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_result_t lv_async_call(lv_async_cb_t async_xcb, void * user_data)
{
    lv_result_t result = LV_RESULT_OK;
    lv_async_info_t * info;
    lv_timer_t * timer;

    /*Allocate an info structure. On ESP32-P4 with esp_hosted 2.12.1 the Wi-Fi
     * bring-up path corrupts internal-RAM heap metadata; keeping these small
     * async descriptors in PSRAM isolates them from that corruption.*/

    /* lv_async_call may be invoked from any task (Wi-Fi events, worker,
     * USB). Creating/deleting timers mutates the global timer list, which the
     * LVGL task iterates in lv_timer_handler() under lv_lock(). Take the
     * recursive lock here so a foreign task cannot corrupt the list. */
    lv_lock();

#if defined(ESP_PLATFORM)
    info = async_info_alloc();
#else
    info = (lv_async_info_t *)lv_malloc(sizeof(lv_async_info_t));
#endif

    if(info == NULL) {
        result = LV_RESULT_INVALID;
        goto done;
    }

    /*Create a new timer*/
    timer = lv_timer_create(lv_async_timer_cb, 0, info);

    if(timer == NULL) {
#if defined(ESP_PLATFORM)
        async_info_free(info);
#else
        lv_free(info);
#endif
        result = LV_RESULT_INVALID;
        goto done;
    }

    info->cb = async_xcb;
    info->user_data = user_data;

    lv_timer_set_repeat_count(timer, 1);

done:
    lv_unlock();
    return result;
}

lv_result_t lv_async_call_cancel(lv_async_cb_t async_xcb, void * user_data)
{
    lv_timer_t * timer;
    lv_result_t res = LV_RESULT_INVALID;

    /* Serialise against lv_timer_handler()/lv_async_call() on other tasks. */
    lv_lock();

    timer = lv_timer_get_next(NULL);
    while(timer != NULL) {
        /*Find the next timer node*/
        lv_timer_t * timer_next = lv_timer_get_next(timer);

        /*Find async timer callback*/
        if(timer->timer_cb == lv_async_timer_cb) {
            lv_async_info_t * info = (lv_async_info_t *)timer->user_data;

            /*Match user function callback and user data*/
            if(info->cb == async_xcb && info->user_data == user_data) {
                lv_timer_delete(timer);
#if defined(ESP_PLATFORM)
                async_info_free(info);
#else
                lv_free(info);
#endif
                res = LV_RESULT_OK;
            }
        }

        timer = timer_next;
    }

    lv_unlock();
    return res;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void lv_async_timer_cb(lv_timer_t * timer)
{
    /*Save the info because an lv_async_call_cancel might delete it in the callback*/
    lv_async_info_t * info = (lv_async_info_t *)timer->user_data;
    lv_async_info_t info_save = *info;
    lv_timer_delete(timer);
#if defined(ESP_PLATFORM)
    async_info_free(info);
#else
    lv_free(info);
#endif

    info_save.cb(info_save.user_data);
}

#if defined(ESP_PLATFORM)
static lv_async_info_t * async_info_alloc(void)
{
    lv_async_info_t * info = (lv_async_info_t *)heap_caps_malloc(sizeof(lv_async_info_t),
                                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(info == NULL) {
        info = (lv_async_info_t *)heap_caps_malloc(sizeof(lv_async_info_t),
                                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return info;
}

static void async_info_free(lv_async_info_t * info)
{
    heap_caps_free(info);
}
#endif
