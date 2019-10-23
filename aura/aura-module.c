// SPDX-License-Identifier: GPL-2.0
#include "aura.h"

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

static const struct file_operations aura_fops = {
    .owner = THIS_MODULE,
};

static struct miscdevice aura_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "aura",
    .fops  = &aura_fops,
};

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

static void aura_module_probe_all (
    void
){
    aura_motherboard_probe(&global_state);
    aura_memory_probe(&global_state);
    aura_gpu_probe(&global_state);
    aura_header_probe(&global_state);
}

static void aura_module_release_all (
    void
){
    aura_motherboard_release();
    aura_memory_release();
    aura_gpu_release();
    aura_header_release();
}

static int __init aura_module_init (
    void
){
    int err;

    err = misc_register(&aura_misc);
    if (err)
        return err;

    aura_dev = aura_misc.this_device;

    err = aura_module_load_defaults();
    if (err)
        return err;

    aura_module_probe_all();

    return 0;
}

static void __exit aura_module_exit (
    void
){
    aura_module_release_all();
    misc_deregister(&aura_misc);
}

module_init(aura_module_init);
module_exit(aura_module_exit);

#ifdef MODULE_IMPORT_NS
MODULE_IMPORT_NS(LIGHTS);
#endif

MODULE_AUTHOR("Owen Parry <waldermort@gmail.com>");
MODULE_DESCRIPTION("ASUS AURA SMBus driver");
MODULE_LICENSE("GPL");
