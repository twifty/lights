// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <adapter/debug.h>
#include "lights-interface.h"

static char *default_color      = "#FF0000";
static char *default_effect     = "static";
static char *default_speed      = "2";
static char *default_direction  = "0";


extern void lights_destroy (
    void
);

extern error_t lights_init (
    struct lights_state *state
);

/**
 * @lights_module_exit() - Module exit
 */
static void __exit lights_module_exit (void)
{
    lights_destroy();
}

/**
 * @lights_module_init() - Module entry
 */
static int __init lights_module_init (void)
{
    struct lights_state state = {0};
    int err;

    err = lights_read_effect(default_effect, strlen(default_effect), NULL, &state.effect);
    if (err < 0) {
        LIGHTS_ERR("Invalid effect");
        return err;
    }

    err = lights_read_color(default_color, strlen(default_color), &state.color);
    if (err < 0) {
        LIGHTS_ERR("Invalid color");
        return err;
    }

    err = lights_read_speed(default_speed, strlen(default_speed), &state.speed);
    if (err < 0) {
        LIGHTS_ERR("Invalid speed");
        return err;
    }

    err = lights_read_direction(default_direction, strlen(default_direction), &state.direction);
    if (err < 0) {
        LIGHTS_ERR("Invalid direction");
        return err;
    }

    return lights_init(&state);
}

module_param(default_color,     charp, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
module_param(default_effect,    charp, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
module_param(default_speed,     charp, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
module_param(default_direction, charp, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);

module_init(lights_module_init);
module_exit(lights_module_exit);

MODULE_PARM_DESC(default_color,     "A hexadecimal color code, eg. #00FF00");
MODULE_PARM_DESC(default_effect,    "The name of a color effect");
MODULE_PARM_DESC(default_speed,     "The speed of the color cycle, 1-5");
MODULE_PARM_DESC(default_direction, "The direction of rotation, 0 or 1");

MODULE_AUTHOR("Owen Parry <twifty@zoho.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("RGB Lighting Class Interface");
