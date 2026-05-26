/**
 * @file lv_port_indev.c
 *
 */

#include <lvgl.h>
#include <lvgl/lv_conf.h>
#if USE_SDL_GPU
#include <lv_drivers/sdl/sdl_gpu.h>
#endif

#include "lv_port_indev.h"
#include "evdev.h"

static int rot_indev;
static lv_indev_t *indev_touchpad;
static lv_indev_t *indev_sdl;

#if USE_SENSOR
static lv_indev_drv_t lsensor_drv;
static lv_indev_drv_t psensor_drv;

lv_indev_drv_t *lv_port_indev_get_lsensor_drv(void)
{
    return &lsensor_drv;
}

lv_indev_drv_t *lv_port_indev_get_psensor_drv(void)
{
    return &psensor_drv;
}
#endif

void lv_port_indev_init(int rot)
{
    static lv_indev_drv_t indev_drv;
    static lv_indev_drv_t sdl_drv;
    lv_disp_t *disp;

    rot_indev = rot;

#if USE_EVDEV != 0 || USE_BSD_EVDEV
    disp = lv_disp_get_default();
    if (evdev_init(disp->driver, rot) == 0)
    {
        lv_indev_drv_init(&indev_drv);
        indev_drv.type = LV_INDEV_TYPE_POINTER;
        indev_drv.read_cb = evdev_read;
        indev_touchpad = lv_indev_drv_register(&indev_drv);
    }
#endif

#if USE_SDL_GPU
    lv_indev_drv_init(&sdl_drv);
    sdl_drv.type = LV_INDEV_TYPE_POINTER;
    sdl_drv.read_cb = sdl_mouse_read;
    indev_sdl = lv_indev_drv_register(&sdl_drv);
#endif

#if USE_SENSOR
    lv_indev_drv_init(&lsensor_drv);
    if (evdev_init_lsensor() >= 0)
    {
        lsensor_drv.type = LV_INDEV_TYPE_NONE;
        lsensor_drv.user_data = evdev_get_lsensor();
        lsensor_drv.read_cb = evdev_sensor_read;
    }

    lv_indev_drv_init(&psensor_drv);
    if (evdev_init_psensor() >= 0)
    {
        psensor_drv.type = LV_INDEV_TYPE_NONE;
        psensor_drv.user_data = evdev_get_psensor();
        psensor_drv.read_cb = evdev_sensor_read;
    }
#endif
}
