// SPDX-License-Identifier: GPL-2.0
#include <linux/types.h>
#include <linux/module.h>

#include <aura/debug.h>
#include <adapter/lights-interface.h>

#include "aura-module.h"

struct device *aura_dev;
static struct lights_state global_state;

static char *argv_color = NULL;
static char *argv_mode  = NULL;
static char *argv_speed = NULL;

module_param(argv_color, charp, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
module_param(argv_mode, charp, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
module_param(argv_speed, charp, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);

MODULE_PARM_DESC(argv_color, "A hexadecimal color code, eg. #00FF00");
MODULE_PARM_DESC(argv_mode, "The name of a color mode");
MODULE_PARM_DESC(argv_speed, "The speed of the color cycle, 1-5");

extern struct lights_mode aura_available_modes[];

static error_t aura_module_load_defaults (
    void
){
    char buffer[LIGHTS_MAX_MODENAME_LENGTH];
    error_t err;

    lights_get_state(&global_state);

    err = 0;
    if (argv_color) {
        err = lights_read_color(buffer, LIGHTS_MAX_MODENAME_LENGTH, &global_state.color);
        if (err)
            goto error;
    }

    if (argv_mode) {
        err = lights_read_mode(buffer, LIGHTS_MAX_MODENAME_LENGTH, aura_available_modes, &global_state.mode);
        if (err)
            goto error;
    }

    if (argv_speed) {
        err = lights_read_speed(buffer, LIGHTS_MAX_MODENAME_LENGTH, &global_state.speed);
        if (err)
            goto error;
    }

error:
    return err;
}

static error_t aura_module_probe_all (
    void
){
    probe_func_t *iter;
    probe_func_t funcs[] = {
        aura_motherboard_probe,
        aura_memory_probe,
        aura_gpu_probe,
        // aura_header_probe,
        NULL
    };
    error_t err = 0;

    for (iter = funcs; *iter != NULL && !err; iter++) {
        err = (*iter)(&global_state);
    }

    return err;
}

static void aura_module_release_all (
    void
){
    release_func_t *iter;
    release_func_t funcs[] = {
        aura_motherboard_release,
        aura_memory_release,
        aura_gpu_release,
        aura_header_release,
        NULL
    };

    for (iter = funcs; *iter != NULL; iter++)
        (*iter)();
}

static int __init aura_module_init (
    void
){
    int err;

    err = aura_module_load_defaults();
    if (err)
        return err;

    err = aura_module_probe_all();
    if (err)
        aura_module_release_all();

    return err;
}

static void __exit aura_module_exit (
    void
){
    aura_module_release_all();
}

module_init(aura_module_init);
module_exit(aura_module_exit);

#ifdef MODULE_IMPORT_NS
MODULE_IMPORT_NS(LIGHTS);
#endif

MODULE_AUTHOR("Owen Parry <waldermort@gmail.com>");
MODULE_DESCRIPTION("ASUS AURA SMBus driver");
MODULE_LICENSE("GPL");
