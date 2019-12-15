/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UAPI_LIGHTS_ADAPTER_THUNK_H
#define _UAPI_LIGHTS_ADAPTER_THUNK_H

#include <linux/types.h>

/**
 * struct lights_thunk - Avoid using void pointers
 *
 * @ptr:   Any user value
 * @value: Any user value
 *
 * This object is inteded to be embedded within a users object which
 * can be referenced using container_of. The @value member can be used
 * as a type id allowing the thunk to be used within multiple objects
 * and then derefenced with some safety.
 */
struct lights_thunk {
    union {
        void        *ptr;
        uint32_t    value;
    };
};

/**
 * container_of_thunk()
 *
 * Wrapper around container_of(). The @_hash argument is expected
 * to be a multi-char constant (ie. 'HASH') and the same value
 * should have been configured upon instance creation.
 *
 * If the hash doesn't match, a debug message will be logged and
 * NULL will be returned.
 */
#define lights_thunk_container(_thunk, _type, _member, _hash) ({ \
    _type *__p = container_of((_thunk), _type, _member); \
    _Pragma("GCC diagnostic push"); \
    _Pragma("GCC diagnostic ignored \"-Wmultichar\""); \
    if (IS_NULL(_thunk) || IS_FALSE(__p->_member.value == (_hash))) \
        __p = NULL; \
    _Pragma("GCC diagnostic pop"); \
    __p; \
})

#define lights_thunk_init(_thunk, _hash) ({ \
    _Pragma("GCC diagnostic push"); \
    _Pragma("GCC diagnostic ignored \"-Wmultichar\""); \
    (_thunk)->value = (_hash); \
    _Pragma("GCC diagnostic pop"); \
})

#endif
