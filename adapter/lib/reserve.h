/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UAPI_DRIVER_RESERVE_H
#define _UAPI_DRIVER_RESERVE_H

struct reserve_context;

typedef struct reserve_context * reserve_t;

/**
 * reserve_create() - Creates a referenced counted memory pool
 *
 * @name:     A unique name for entry into /proc/slabinfo
 * @min_nr:   Initial and minimum count of objects
 * @el_size:  Size of each object
 * @flags:    Slab creation flags
 * @gfp:      Memory allocation flags
 *
 * @return: The memory pool or a negative error code
 *
 * It is preferred to use the @reserve_get macro.
 */
reserve_t reserve_create (
    const char *name,
    int min_nr,
    size_t el_size,
    slab_flags_t flags,
    gfp_t gfp
);

/**
 * reserve_get() - Returns a reference counted pool by name
 *
 * @__struct: The name of the struct managed by the pool
 * @__min:    Minimum number of objects in the pool
 * @__flags:  Slab creation flags
 *
 * @return: A standard memory pool
 */
#define reserve_get(__struct, __min, __flags, __gfp)(   \
    reserve_create(                                     \
        #__struct,                                      \
        (__min),                                        \
        sizeof(struct __struct),                        \
        (__flags),                                      \
        (__gfp)                                         \
    )                                                   \
)

/**
 * reserve_put() - Releases a reference to a named memory pool
 *
 * @reserve: Previously allocated with reserve_get()
 *
 * Once this function has been called, it is no longer safe
 * to use @pool.
 */
void reserve_put (reserve_t reserve);

/**
 * reserve_resize() - Resizes an existing memory pool
 *
 * @reserve: Previously allocated with reserve_get()
 * @min_nr:  A new minimum number of elements
 *
 * @return: Zero or negative error code
 */
int reserve_resize (reserve_t reserve, int new_min_nr);

/**
 * reserve_purge() - Releases Unused memory allocations
 *
 * @reserve: Previously created with reserve_get()
 *
 * Calling this will remove all unused memory blocks allocated
 * beyond the predefined min_nr. It will also cancel any running
 * purge thread.
 */
void reserve_purge (reserve_t reserve);

/**
 * reserve_alloc() - Allocates an element from the pool
 *
 * @reserve:  Previously allocated with reserve_get()
 *
 * @return: The allocated element or a negative error code
 *
 * This function may sleep. It will never fail when called
 * from process context, but may fail when called from interupt.
 */
void *reserve_alloc (reserve_t reserve) __malloc;

/**
 * reserve_free() - Returns a previously allocated element
 *
 * @reserve: Previously allocated with reserve_get()
 * @element: Previously allocated with reserve_alloc()
 */
void reserve_free (reserve_t reserve, const void *element);

#endif
