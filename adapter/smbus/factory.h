/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UAPI_LIGHTS_ADAPTER_SMBUS_FACTORY_H
#define _UAPI_LIGHTS_ADAPTER_SMBUS_FACTORY_H

#include <linux/i2c.h>

/**
 * struct i2c_adapter - Creates an adapter for the 0xB20 bus
 *
 * @return: null if the bus wasn't detected
 */
struct i2c_adapter *piix4_adapter_create (void);

/**
 * piix4_adapter_destroy - Releases the adapter
 *
 * @adap: An adapter previously created with @piix4_adapter_create
 */
void piix4_adapter_destroy (struct i2c_adapter *adap);

/**
 * struct i2c_adapter - Creates an adapter for the nuvoton bus
 *
 * @return: null if the bus was't detected
 */
struct i2c_adapter *nuvoton_adapter_create (void);

/**
 * piix4_adapter_destroy - Releases the adapter
 *
 * @adap: An adapter previously created with @nuvoton_adapter_create
 */
void nuvoton_adapter_destroy (struct i2c_adapter *adap);

typedef struct i2c_adapter *(*i2c_create_t)(void);
typedef void (*i2c_destroy_t)(struct i2c_adapter*);

struct smbus_factory_entry {
    const char      *name;
    i2c_create_t    create;
    i2c_destroy_t   destroy;
};

/**
 * struct smbus_factory_entry
 *
 * A collection of known factory methods. This should enable new
 * factories to be added without having to change other files.
 */
struct smbus_factory_entry smbus_factory[] = {{
    .name = "piix4",
    .create = piix4_adapter_create,
    .destroy = piix4_adapter_destroy,
},{
    .name = "nuvoton",
    .create = nuvoton_adapter_create,
    .destroy = nuvoton_adapter_destroy
}};

#endif
