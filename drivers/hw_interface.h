/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UAPI_LIGHTS_DRIVER_INTF_H
#define _UAPI_LIGHTS_DRIVER_INTF_H

#include <linux/types.h>
#include "drivers.h"

struct hw_device;
typedef void (*hw_done_t) (void*);
typedef void (*hw_release_t) (struct hw_device*);

typedef int (*hw_read_t) (struct hw_device* const, void*, size_t);
typedef int (*hw_write_t) (struct hw_device* const, void const*, size_t);
typedef int (*hw_read_async_t) (struct hw_device* const, void*, size_t, void*, hw_done_t);
typedef int (*hw_write_async_t) (struct hw_device* const, void const*, size_t, void*, hw_done_t);

struct hw_device_vtable {
    hw_release_t        release;
    hw_read_t           read;
    hw_write_t          write;
    hw_read_async_t     read_async;
    hw_write_async_t    write_async;
};

struct hw_device {
    struct hw_device_vtable const *vtable;
};

/**
 * hw_read()
 * @param  dev [description]
 * @param  buf [description]
 * @param  len [description]
 * @return     [description]
 */
#define hw_read(dev, buf, len) \
    (dev)->vtable->read((dev), (buf), (len))

#define hw_write(dev, buf, len) \
    (dev)->vtable->write((dev), (buf), (len))

#define hw_read_async(dev, buf, len, data, cb) \
    (dev)->vtable->read_async((dev), (buf), (len), (data), (cb))

#define hw_write_async(dev, buf, len, data, cb) \
    (dev)->vtable->write_async((dev), (buf), (len), (data), (cb))

#define hw_release(dev) \
    (dev)->vtable->release((dev))

#endif
