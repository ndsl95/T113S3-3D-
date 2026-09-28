/* Minimal evdev touchscreen driver for LVGL 9.3.
 *
 * The board rootfs has no libevdev (and its shared libraries are damaged),
 * so the input device is read directly.  Handles Linux MT protocol type B
 * (edt-ft5x06 on /dev/input/event1) and falls back to the single touch
 * ABS_X / ABS_Y axes.
 *
 * The ft5x06 driver on this board declares ABS_MT_POSITION_X/Y as 0..65535
 * but actually reports values already in panel pixels (measured X 0..719,
 * Y 0..1279 on a 720x1280 panel), so the raw value is used as the pixel
 * coordinate.  LV_TOUCH_SWAP_XY / LV_TOUCH_INVERT_X / LV_TOUCH_INVERT_Y
 * are available if a panel ever needs it.
 */
#include "lvgl/lvgl.h"
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

typedef struct {
    int fd;
    lv_indev_t * indev;
    int32_t raw_x;
    int32_t raw_y;
    int pressed;
    int debug;
    int swap_xy;
    int invert_x;
    int invert_y;
} touch_ctx_t;

static touch_ctx_t g_touch;

static int env_flag(const char * name)
{
    const char * v = getenv(name);
    return v && v[0] != '0';
}

static void touch_read_cb(lv_indev_t * indev, lv_indev_data_t * data)
{
    touch_ctx_t * ctx = &g_touch;
    struct input_event ev;

    while(1) {
        ssize_t n = read(ctx->fd, &ev, sizeof(ev));
        if(n != (ssize_t)sizeof(ev)) break;

        if(ev.type == EV_ABS) {
            switch(ev.code) {
                case ABS_MT_POSITION_X:
                case ABS_X:
                    ctx->raw_x = ev.value;
                    break;
                case ABS_MT_POSITION_Y:
                case ABS_Y:
                    ctx->raw_y = ev.value;
                    break;
                case ABS_MT_TRACKING_ID:
                    ctx->pressed = (ev.value >= 0);
                    break;
                default:
                    break;
            }
        }
        else if(ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            ctx->pressed = (ev.value != 0);
        }

        if(ctx->debug) {
            printf("touch ev: type=%u code=%u value=%d\n", ev.type, ev.code, ev.value);
        }
    }

    lv_display_t * disp = lv_indev_get_display(indev);
    if(disp == NULL) disp = lv_display_get_default();

    int32_t range_x = (int32_t)lv_display_get_horizontal_resolution(disp);
    int32_t range_y = (int32_t)lv_display_get_vertical_resolution(disp);

    int32_t x = ctx->raw_x;
    int32_t y = ctx->raw_y;

    if(ctx->swap_xy) {
        int32_t t = x; x = y; y = t;
        t = range_x; range_x = range_y; range_y = t;
    }
    if(ctx->invert_x) x = (range_x - 1) - x;
    if(ctx->invert_y) y = (range_y - 1) - y;

    if(x < 0) x = 0;
    if(y < 0) y = 0;
    if(x >= range_x) x = range_x - 1;
    if(y >= range_y) y = range_y - 1;

    data->point.x = x;
    data->point.y = y;
    data->state = ctx->pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

lv_indev_t * touch_input_create(const char * device)
{
    touch_ctx_t * ctx = &g_touch;
    memset(ctx, 0, sizeof(*ctx));

    ctx->fd = open(device, O_RDONLY | O_NONBLOCK);
    if(ctx->fd < 0) {
        printf("touch: cannot open %s (%s)\n", device, strerror(errno));
        return NULL;
    }

    ctx->debug    = env_flag("LV_TOUCH_DEBUG");
    ctx->swap_xy  = env_flag("LV_TOUCH_SWAP_XY");
    ctx->invert_x = env_flag("LV_TOUCH_INVERT_X");
    ctx->invert_y = env_flag("LV_TOUCH_INVERT_Y");

    printf("touch: %s opened (raw values used as panel pixels), swap_xy=%d invert_x=%d invert_y=%d\n",
           device, ctx->swap_xy, ctx->invert_x, ctx->invert_y);

    ctx->indev = lv_indev_create();
    lv_indev_set_type(ctx->indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(ctx->indev, touch_read_cb);
    return ctx->indev;
}