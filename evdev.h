/**
 * @file evdev.h
 *
 */

#ifndef EVDEV_H
#define EVDEV_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef USE_EVDEV
#  define USE_EVDEV           0
#endif

#ifndef USE_BSD_EVDEV
#  define USE_BSD_EVDEV       0
#endif

#ifndef USE_SENSOR
#  define USE_SENSOR          0
#endif

#ifndef USE_PSENSOR
#  define USE_PSENSOR         0
#endif

#if USE_EVDEV || USE_BSD_EVDEV
#  undef EVDEV_NAME
#  define EVDEV_NAME   "/dev/input/event2"
#  define EVDEV_SWAP_AXES         0

#  define DEFAULT_EVDEV_HOR_MIN   0
#  define DEFAULT_EVDEV_HOR_MAX   720
#  define DEFAULT_EVDEV_VER_MIN   0
#  define DEFAULT_EVDEV_VER_MAX   1280
#endif

#if USE_EVDEV || USE_BSD_EVDEV

#include <lvgl.h>

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 * GLOBAL PROTOTYPES
 **********************/

int evdev_init(lv_disp_drv_t *disp_drv, int rot);
int evdev_set_file(lv_disp_drv_t *disp_drv, char *dev_name);
void evdev_read(lv_indev_drv_t *drv, lv_indev_data_t *data);

#if USE_SENSOR
int evdev_init_psensor(void);

void *evdev_get_psensor(void);

int evdev_init_lsensor(void);

void *evdev_get_lsensor(void);

void evdev_sensor_read(lv_indev_drv_t *drv, lv_indev_data_t *data);
#endif

#endif

extern int g_evdev_raw_x;
extern int g_evdev_raw_y;
extern int g_touch_offset_x;
extern int g_touch_offset_y;

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*EVDEV_H*/
