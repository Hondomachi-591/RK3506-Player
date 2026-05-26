/**
 * @file lv_port_init.c
 *
 */

#include <lvgl/lvgl.h>
#include <lvgl/lv_conf.h>

#include "lv_port_disp.h"
#include "lv_port_indev.h"

void lv_port_init(int width, int height, int rotation)
{
    lv_init();

    lv_port_disp_init(width, height, rotation);
    lv_port_indev_init(rotation);
}
