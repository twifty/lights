/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UAPI_DRIVER_SMBUS_ADAPTER_H
#define _UAPI_DRIVER_SMBUS_ADAPTER_H

#include <linux/i2c.h>
#include "../async.h"
#include "../hw_interface.h"

struct smbus_adapter {
    struct hw_device    hw_dev;
    struct i2c_adapter  *i2c_adapter;
    struct async_queue  *async_queue;
};

#define to_smbus_adapter(dev)( \
    container_of(dev, struct smbus_adapter, hw_dev) \
)

struct smbus_msg {
    u16     addr;
    u16     flags;
    u8      command;
    u8      type;
    u8      length;
    u8      swapped;
    union {
        u8  byte;
        u16 word;
        u8  block[I2C_SMBUS_BLOCK_MAX];
    } data;
};


int smbus_read_async (struct hw_device* const, void*, size_t, void*, hw_done_t);
int smbus_write_async (struct hw_device* const, void const*, size_t, void*, hw_done_t);

int smbus_read (struct hw_device *const, void*, size_t);
int smbus_write (struct hw_device *const, void const*, size_t);

#endif
