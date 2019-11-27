// SPDX-License-Identifier: GPL-2.0
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>
#include <include/quirks.h>

#include <adapter/debug.h>
#include "reserve.h"

#define GUARD_USED  0x5A5A5A5A
#define GUARD_FREE  0x6B6B6B6B
#define GUARD_END   0xA5A5A5A5

/* Mark nodes for deletion after 60 seconds */
#define RESERVE_NODE_TTL 60
/* Purge expired nodes every 60 seconds */
#define RESERVE_NODE_PURGE 60

static LIST_HEAD(reserve_context_list);
static DEFINE_SPINLOCK(reserve_context_lock);

enum reserve_state {
    STATE_IDLE    = 0,
    STATE_PURGING = 1,
    STATE_EXITING = 2,
};

/**
 * struct reserve_context - Storage for reserved memory
 *
 * @siblings:     Next and prev pointers
 * @allocated:    List of nodes currently in-use
 * @available:    List of node available for use
 * @lock:         Access spin lock
 * @worker:       Purge worker thread
 * @state:        Atomic state of this object
 * @min_nr:       Minimum number of pre-allocated nodes
 * @alloc_nr:     Actual number of allocated nodes
 * @node_size:    Byte size of each node
 * @guard_offset: Offset into node data of ending write guard
 * @gfp_mask:     GFP mask to use when creating new nodes
 * @cache:        Kernel memory cache instance
 * @ref:          Reference counter
 * @name:         Unique name, also visible in /proc/slabinfo
 */
struct reserve_context {
    struct list_head    siblings;
    struct list_head    allocated;
    struct list_head    available;

    spinlock_t          lock;

    struct delayed_work worker;
    atomic_t            state;

    size_t              min_nr;
    size_t              alloc_nr;

    size_t              node_size;
    size_t              guard_offset;

    gfp_t               gfp_mask;
    struct kmem_cache   *cache;
    struct kref         ref;
    const char          *name;
};

/**
 * struct reserve_node - Wrapper for callers memory block
 *
 * @siblings: Next and prev pointers
 * @guard:    Overwrite detection and state bytes
 * @data:     Callers memory block
 *
 * When a node is unused, the @data member is a uint32_t containing
 * the time the node was freed. The last array entry in @data is a
 * guard type to detect for writing past end.
 */
struct reserve_node {
    struct list_head    siblings;
    uint32_t            guard;
    uint32_t            data[];
};
#define list_first_node(head) ( \
    list_first_entry_or_null((head), struct reserve_node, siblings) \
)
#define list_last_node(head) ({ \
    struct reserve_node * node = NULL; \
    if (!list_empty((head))) \
        node = list_last_entry((head), struct reserve_node, siblings); \
    node; \
})

/**
 * reserve_context_find() - Finds a named context
 *
 * @name: Unique name to search for
 *
 * @return: NULL or the named context
 */
static struct reserve_context *reserve_context_find(
   const char *name
){
    struct reserve_context *iter;
    unsigned long flags;

    spin_lock_irqsave(&reserve_context_lock, flags);

    list_for_each_entry(iter, &reserve_context_list, siblings) {
        if (0 == strcmp(name, iter->name)) {
            kref_get(&iter->ref);
            goto found;
        }
    }

    iter = NULL;

found:
    spin_unlock_irqrestore(&reserve_context_lock, flags);

    return iter;
}

/**
 * reserve_context_insert() - Adds a context to global list
 *
 * @context: Context to add
 *
 * @return: Zero or negative error number if already exists
 */
static error_t reserve_context_insert (
    struct reserve_context * context
){
    struct reserve_context *iter;
    unsigned long flags;
    error_t err = 0;

    spin_lock_irqsave(&reserve_context_lock, flags);

    list_for_each_entry(iter, &reserve_context_list, siblings) {
        if (0 == strcmp(context->name, iter->name)) {
            err = -EEXIST;
            goto found;
        }
    }

    list_add_tail(&context->siblings, &reserve_context_list);

found:
    spin_unlock_irqrestore(&reserve_context_lock, flags);

    return err;
}

/**
 * reserve_context_destroy() - Destroys a context
 *
 * @ref: Reference counter instance
 */
static void reserve_context_destroy (
    struct kref * ref
){
    struct reserve_context * context = container_of(ref, struct reserve_context, ref);
    struct reserve_node * iter, * safe;
    unsigned long flags;

    if (context->siblings.next) {
        spin_lock_irqsave(&reserve_context_lock, flags);
        list_del(&context->siblings);
        spin_unlock_irqrestore(&reserve_context_lock, flags);
    }

    if (STATE_PURGING == atomic_xchg(&context->state, STATE_EXITING)) {
        cancel_delayed_work_sync(&context->worker);
    }

    if (!list_empty(&context->allocated)) {
        LIGHTS_ERR("Reserve still contains allocated objects");

        list_for_each_entry_safe(iter, safe, &context->allocated, siblings) {
            LIGHTS_DBG("Removing from list");
            list_del(&iter->siblings);
            kmem_cache_free(context->cache, iter);
        }

        LIGHTS_DBG("Removed dangling objects");
    }

    list_for_each_entry_safe(iter, safe, &context->available, siblings) {
        list_del(&iter->siblings);
        kmem_cache_free(context->cache, iter);
    }

    if (context->cache)
        kmem_cache_destroy(context->cache);

    LIGHTS_DBG("Destroyed reserve '%s'", context->name);

    if (context->name)
        kfree_const(context->name);

    kfree(context);
}

/**
 * reserve_context_get_node() - Allocates a node
 *
 * @context: Owning context
 *
 * @return: Node or a negative error number
 *
 * If the context doesn't contain any unused nodes, a new node
 * will be created. All returned nodes have the guard bytes set.
 *
 * Old nodes are pushed to the end of the list and new nodes are
 * also taken from the end. This means the list is ordered by age.
 * The purge thread will delete nodes from the front and stop when
 * it encouters a node younger than RESERVE_NODE_TTL.
 */
static struct reserve_node * reserve_context_get_node (
    struct reserve_context * context
){
    unsigned long flags;
    struct reserve_node * node;

    if (STATE_EXITING == atomic_read(&context->state))
        return ERR_PTR(-ECANCELED);

    spin_lock_irqsave(&context->lock, flags);
    node = list_last_node(&context->available);
    if (node) {
        node->guard = GUARD_USED;
        list_move_tail(&node->siblings, &context->allocated);
    }
    spin_unlock_irqrestore(&context->lock, flags);

    if (!node) {
        node = kmem_cache_alloc(context->cache, context->gfp_mask);
        if (!node) {
            LIGHTS_ERR("kmem_cache_alloc failure");
            return ERR_PTR(-ENOMEM);
        }

        node->guard = GUARD_USED;

        spin_lock_irqsave(&context->lock, flags);
        list_add_tail(&node->siblings, &context->allocated);
        context->alloc_nr++;
        spin_unlock_irqrestore(&context->lock, flags);

        /* Allocating extra nodes requires future purging */
        if (STATE_IDLE == atomic_cmpxchg(&context->state, STATE_IDLE, STATE_PURGING))
            schedule_delayed_work(&context->worker, HZ * RESERVE_NODE_PURGE);
    }

    node->data[context->guard_offset] = GUARD_END;

    return node;
}

/**
 * reserve_context_put_node() - Unallocates a node
 *
 * @context: Owning context
 * @node:    Node to remove
 *
 * @return: Zero or a negative error number
 *
 * This will only fail if caller wrote outside of block or
 * the node has already been released. The node is added to
 * a list for later usage. This lisy is routinely purged.
 */
static error_t reserve_context_put_node (
    struct reserve_context * context,
    struct reserve_node * node
){
    unsigned long flags;
    struct reserve_node * iter;

    if (IS_NULL(context, node))
        return -EINVAL;

    if (node->guard != GUARD_USED) {
        if (node->guard == GUARD_FREE) {
            LIGHTS_WARN("Object has already been freed");
            return -EFAULT;
        } else {
            LIGHTS_ERR("Leading guard bytes do not match");
        }
    }

    if (node->data[context->guard_offset] != GUARD_END)
        LIGHTS_ERR("Trailing guard bytes do not match");

    list_for_each_entry(iter, &context->allocated, siblings) {
        if (node == iter)
            goto found;
    }

    LIGHTS_ERR("Object not found in reserve context");
    return -EFAULT;

found:
    node->data[0] = (uint32_t)ktime_get_seconds();

    spin_lock_irqsave(&context->lock, flags);

    node->guard = GUARD_FREE;
    list_move_tail(&node->siblings, &context->available);

    spin_unlock_irqrestore(&context->lock, flags);

    return 0;
}

/**
 * reserve_context_purge() - Removes unused nodes
 *
 * @context: Owning context
 * @ttl:     Age of nodes to remove
 *
 * Removes nodes starting at the front of the list until
 * it encouters one younger than @ttl.
 */
static void reserve_context_purge (
    struct reserve_context * context,
    uint32_t ttl
){
    struct reserve_node * node;
    uint32_t curr_secs = (uint32_t)ktime_get_seconds();
    unsigned long flags;
    int count = 0;

    node = list_first_node(&context->available);
    while (node) {
        spin_lock_irqsave(&context->lock, flags);

        if (node->guard == GUARD_FREE && curr_secs - node->data[0] >= ttl) {
            list_del(&node->siblings);
            kmem_cache_free(context->cache, node);
            context->alloc_nr--;
            count++;
            node = list_first_node(&context->available);
        } else {
            node = NULL;
        }

        spin_unlock_irqrestore(&context->lock, flags);
    }

    if (count)
        LIGHTS_DBG("Purged %d nodes", count);
}

/**
 * reserve_context_purge_callback() - Removes stale nodes
 *
 * @work: Work instance
 */
static void reserve_context_purge_callback (
    struct work_struct * work
){
    struct reserve_context * context = container_of(work, struct reserve_context, worker.work);

    reserve_context_purge(context, RESERVE_NODE_TTL);

    if (STATE_PURGING == atomic_read(&context->state))
        schedule_delayed_work(&context->worker, HZ * RESERVE_NODE_PURGE);
}

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
){
    struct reserve_context * context;
    struct reserve_node * node;
    size_t guard_offset;
    size_t node_size;
    error_t err;

    if (el_size < sizeof(uint32_t))
        el_size = sizeof(uint32_t);

    el_size      = (el_size + 3) & ~0x3;
    guard_offset = (el_size / sizeof(uint32_t));
    node_size    = el_size + sizeof(struct reserve_node) + sizeof(uint32_t);

    if (IS_NULL(name) || IS_TRUE(name[0] == 0))
        return ERR_PTR(-EINVAL);

    context = reserve_context_find(name);
    if (context) {
        if (context->node_size != node_size) {
            LIGHTS_ERR("Conflicting sizes for reserve '%s'", name);
            return ERR_PTR(-EEXIST);
        }
        return context;
    }

    context = kzalloc(sizeof(*context), GFP_KERNEL);
    if (!context)
        return ERR_PTR(-ENOMEM);

    INIT_LIST_HEAD(&context->allocated);
    INIT_LIST_HEAD(&context->available);
    INIT_DELAYED_WORK(&context->worker, reserve_context_purge_callback);
    kref_init(&context->ref);
    atomic_set(&context->state, STATE_IDLE);

    context->gfp_mask     = gfp;
    context->min_nr       = min_nr;
    context->guard_offset = guard_offset;
    context->node_size    = node_size;

    context->name = kstrdup_const(name, GFP_KERNEL);
    if (!context->name) {
        err = -ENOMEM;
        goto error_free;
    }

    context->cache = kmem_cache_create(context->name, node_size, __alignof(struct reserve_node), flags, NULL);
    if (IS_ERR_OR_NULL(context->cache)) {
        err = CLEAR_ERR(context->cache);
        goto error_free;
    }

    for (context->alloc_nr = 0; context->alloc_nr < min_nr; context->alloc_nr++) {
        node = kmem_cache_alloc(context->cache, context->gfp_mask);
        if (!node) {
            err = -ENOMEM;
            goto error_free;
        }
        node->guard = GUARD_FREE;
        list_add_tail(&node->siblings, &context->available);
    }

    err = reserve_context_insert(context);
    if (err)
        goto error_free;

    LIGHTS_DBG("Created reserve '%s'", context->name);

    return context;

error_free:
    reserve_context_destroy(&context->ref);

    return ERR_PTR(err);
}
EXPORT_SYMBOL_NS_GPL(reserve_create, LIGHTS);

/**
 * reserve_put - Releases a reference to a named memory pool
 *
 * @reserve: Previously allocated with reserve_get()
 *
 * Once this function has been called, it is no longer safe
 * to use @pool.
 */
void reserve_put (
    reserve_t reserve
){
    if (IS_NULL(reserve))
        return;

    kref_put(&reserve->ref, reserve_context_destroy);
}
EXPORT_SYMBOL_NS_GPL(reserve_put, LIGHTS);

/**
 * reserve_resize - Resizes an existing memory pool
 *
 * @reserve: Previously allocated with reserve_get()
 * @min_nr:  A new minimum number of elements
 *
 * @return: Zero or negative error code
 */
error_t reserve_resize (
    reserve_t context,
    int new_min_nr
){
    unsigned long flags;
    struct reserve_node * node, * safe;
    LIST_HEAD(temp);
    int adjust;
    int count = 0;
    error_t err = 0;

    if (IS_NULL(context))
        return -EINVAL;

    if (STATE_EXITING == atomic_read(&context->state))
        return -ECANCELED;

    adjust = new_min_nr - context->min_nr;

    if (adjust > 0) {
        while (adjust-- > 0) {
            node = kmem_cache_alloc(context->cache, context->gfp_mask);
            if (!node) {
                err = -ENOMEM;
                goto free_temp;
            }
            node->guard = GUARD_FREE;
            list_add_tail(&node->siblings, &temp);
            count++;
        };

        spin_lock_irqsave(&context->lock, flags);
        list_splice(&temp, &context->available);
        context->alloc_nr += count;
        spin_unlock_irqrestore(&context->lock, flags);

        return 0;
    } else if (adjust < 0) {
        spin_lock_irqsave(&context->lock, flags);
        while (adjust++ < 0 && !list_empty(&context->available)) {
            list_move_tail(context->available.next, &temp);
            context->alloc_nr--;
        }
        spin_unlock_irqrestore(&context->lock, flags);
    }

free_temp:
    list_for_each_entry_safe(node, safe, &temp, siblings) {
        list_del(&node->siblings);
        kmem_cache_free(context->cache, node);
    }

    return err;
}
EXPORT_SYMBOL_NS_GPL(reserve_resize, LIGHTS);

/**
 * reserve_purge() - Releases Unused memory allocations
 *
 * @reserve: Previously created with reserve_get()
 *
 * Calling this will remove all unused memory blocks allocated
 * beyond the predefined min_nr. It will also cancel any running
 * purge thread.
 */
void reserve_purge (
    reserve_t reserve
){
    if (IS_NULL(reserve))
        return;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough="

    switch (atomic_read(&reserve->state)) {
        case STATE_PURGING:
            atomic_cmpxchg(&reserve->state, STATE_PURGING, STATE_IDLE);
        case STATE_IDLE:
            reserve_context_purge(reserve, 0);
        case STATE_EXITING:
            return;
    }

#pragma GCC diagnostic pop
}
EXPORT_SYMBOL_NS_GPL(reserve_purge, LIGHTS);

/**
 * reserve_alloc - Allocates an element from the pool
 *
 * @reserve:  Previously allocated with @mempool_get
 *
 * @return: The allocated element
 *
 * This function may sleep. It will never fail when called
 * from process context, but may fail when called from interupt.
 */
__malloc
void *reserve_alloc (
    reserve_t reserve
){
    struct reserve_node * node;

    if (IS_NULL(reserve))
        return ERR_PTR(-EINVAL);

    node = reserve_context_get_node(reserve);
    if (IS_ERR(node))
        return ERR_CAST(node);

    return node->data;
};
EXPORT_SYMBOL_NS_GPL(reserve_alloc, LIGHTS);

/**
 * reserve_free - Returns a previously allocated element
 *
 * @element: Previously allocated with @mempool_alloc
 * @reserve: Previously allocated with @mempool_get
 */
void reserve_free (
    reserve_t reserve,
    const void *element
){
    struct reserve_node * node;

    if (IS_NULL(reserve, element))
        return;

    node = container_of(element, struct reserve_node, data);

    reserve_context_put_node(reserve, node);
}
EXPORT_SYMBOL_NS_GPL(reserve_free, LIGHTS);
