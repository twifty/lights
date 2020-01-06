// SPDX-License-Identifier: GPL-2.0
#include <linux/types.h>
#include <linux/module.h>

#include <aura/debug.h>
#include <adapter/lights-interface.h>

#include "aura-module.h"

static error_t aura_module_probe_all (
    struct lights_state const *state
){
    probe_func_t *iter;
    probe_func_t funcs[] = {
        aura_motherboard_probe,
        aura_memory_probe,
        aura_gpu_probe,
        aura_header_probe,
        NULL
    };
    error_t err = 0;

    for (iter = funcs; *iter != NULL && !err; iter++) {
        err = (*iter)(state);
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
    struct lights_state state;
    int err;

    lights_get_state(&state);

    err = aura_module_probe_all(&state);
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

MODULE_AUTHOR("Owen Parry <twifty@zoho.com>");
MODULE_DESCRIPTION("ASUS AURA module for lights");
MODULE_LICENSE("GPL");
