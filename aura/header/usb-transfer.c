// SPDX-License-Identifier: GPL-2.0
/*
 * Memory managment for usb packets.
 *
 * Copyright (C) 2019 Owen Parry
 *
 * Authors:
 * Owen Parry <twifty@zoho.com>
 */

#include <linux/spinlock.h>
#include <linux/kref.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include "usb-transfer.h"

#define MAX_PACKETS (MAX_TRANSFERS * MAX_TRANSFER_PACKETS)

struct usb_cache_private {
    spinlock_t                  lock;
    struct kref                 refs;

    wait_queue_head_t           transfer_wait;
    struct usb_transfer_private *transfers;
    unsigned long               *transfer_usage;

    wait_queue_head_t           packet_wait;
    struct usb_packet           *packets;
    unsigned long               *packet_usage;
};

#define INDEX_OF(ptr, array) \
    (((void*)ptr - (void*)array) / sizeof(*ptr))

#define cache_from_kref(ptr) \
    container_of(ptr, struct usb_cache_private, refs)

struct usb_transfer_private {
    struct usb_transfer         public;

    spinlock_t                  lock;
    struct usb_cache_private    *cache;
    size_t                      count;
    size_t                      index;
    struct kref                 refs;
    struct usb_packet           *packets[MAX_TRANSFER_PACKETS];
};

#define xfer_from_kref(ptr) \
    container_of(ptr, struct usb_transfer_private, refs)

#define xfer_from_public(ptr) \
    container_of(ptr, struct usb_transfer_private, public)

struct usb_transfer_queue_private {
    struct kref                 refs;
    spinlock_t                  lock;
    size_t                      count;
    size_t                      read_index;
    size_t                      write_index;
    struct usb_transfer         *data[MAX_TRANSFERS];
};

#define queue_next_index(val) \
    ((val) + 1 >= MAX_TRANSFERS ? 0 : ((val) + 1))

#define queue_from_kref(ptr) \
    container_of(ptr, struct usb_transfer_queue_private, refs)

static void __usb_cache_release (
    struct kref *kref
){
    struct usb_cache_private *cache = cache_from_kref(kref);

    kfree(cache->transfers);
    kfree(cache->transfer_usage);
    kfree(cache->packets);
    kfree(cache->packet_usage);
    kfree(cache);
}

static void __usb_packet_release (
    struct usb_cache_private *cache,
    struct usb_packet *packet
){
    size_t index = INDEX_OF(packet, cache->packets);
    unsigned long cache_flags;

    if (index >= MAX_PACKETS) {
        pr_err("The packet array index (%ld) is out of range", index);
        return;
    }

    /* Mark the transfer as free to use */
    spin_lock_irqsave(&cache->lock, cache_flags);
    clear_bit(index, cache->packet_usage);
    spin_unlock_irqrestore(&cache->lock, cache_flags);

    /* Wake any waiters */
    wake_up(&cache->packet_wait);

    kref_put(&cache->refs, __usb_cache_release);
}

static void __usb_transfer_release (
    struct kref *kref
){
    struct usb_cache_private *cache;
    struct usb_transfer_private *xfer = xfer_from_kref(kref);
    unsigned long cache_flags;
    size_t index, i;

    cache = xfer->cache;
    index = INDEX_OF(xfer, cache->transfers);

    if (index >= MAX_TRANSFERS) {
        pr_err("The transfer array index (%ld) is out of range", index);
        return;
    }

    /* Release allocated packets */
    for (i = 0; i < xfer->count; i++)
        __usb_packet_release(cache, xfer->packets[i]);

    /* Mark the transfer as free to use */
    spin_lock_irqsave(&cache->lock, cache_flags);
    clear_bit(index, cache->transfer_usage);
    spin_unlock_irqrestore(&cache->lock, cache_flags);

    /* Wake any waiters */
    wake_up(&cache->transfer_wait);
}


int usb_transfer_cache_init (
    struct usb_transfer_cache *_cache
){
    struct usb_cache_private *cache;

    if (WARN_ON(_cache == NULL))
        return -EINVAL;

    cache = kzalloc(sizeof(*cache), GFP_KERNEL);
    if (!cache)
        return -ENOMEM;

    spin_lock_init(&cache->lock);
    init_waitqueue_head(&cache->transfer_wait);
    init_waitqueue_head(&cache->packet_wait);
    kref_init(&cache->refs);

    cache->transfers = kcalloc(sizeof(*cache->transfers), MAX_TRANSFER_PACKETS, GFP_KERNEL);
    cache->transfer_usage = kcalloc(sizeof(*cache->transfer_usage), BITS_TO_LONGS(MAX_TRANSFER_PACKETS), GFP_KERNEL);

    cache->packets = kcalloc(sizeof(*cache->packets), MAX_PACKETS, GFP_KERNEL);
    cache->packet_usage = kcalloc(sizeof(*cache->packet_usage), BITS_TO_LONGS(MAX_PACKETS), GFP_KERNEL);

    if (!cache->transfers || !cache->transfer_usage || !cache->packets || !cache->packet_usage) {
        kref_put(&cache->refs, __usb_cache_release);
        return -ENOMEM;
    }

    _cache->p = cache;

    return 0;
}

void usb_transfer_cache_release (
    struct usb_transfer_cache *_cache
){
    struct usb_cache_private *cache;

    if (WARN_ON(_cache == NULL || _cache->p == NULL))
        return;

    cache = _cache->p;
    _cache->p = NULL;

    kref_put(&cache->refs, __usb_cache_release);
}

struct usb_transfer *usb_transfer_create (
    struct usb_transfer_cache *_cache
){
    struct usb_cache_private *cache;
    struct usb_transfer_private *xfer = NULL;
    wait_queue_entry_t wait;
    unsigned long flags;
    size_t index;

    if (WARN_ON(_cache == NULL || _cache->p == NULL))
        return ERR_PTR(-EINVAL);

    cache = _cache->p;

repeat:
    spin_lock_irqsave(&cache->lock, flags);
    index = find_first_zero_bit(cache->transfer_usage, MAX_TRANSFERS);
    if (likely(index < MAX_TRANSFERS)) {
        xfer = &cache->transfers[index];
        set_bit(index, cache->transfer_usage);
    }

    if (likely(xfer != NULL)) {
        spin_unlock_irqrestore(&cache->lock, flags);

        xfer->cache = cache;
        xfer->count = 0;
        xfer->index = 0;

        spin_lock_init(&xfer->lock);
        kref_init(&xfer->refs);
        kref_get(&cache->refs);

        return &xfer->public;
    }

    init_wait(&wait);
    prepare_to_wait(&cache->transfer_wait, &wait, TASK_UNINTERRUPTIBLE);

    spin_unlock_irqrestore(&cache->lock, flags);
    schedule();

    finish_wait(&cache->transfer_wait, &wait);
    goto repeat;
}

void usb_transfer_get (
    struct usb_transfer *_xfer
){
    struct usb_transfer_private *xfer = xfer_from_public(_xfer);

    if (WARN_ON(_xfer == NULL))
        return;

    kref_get(&xfer->refs);
}

void usb_transfer_put (
    struct usb_transfer *_xfer
){
    struct usb_transfer_private *xfer = xfer_from_public(_xfer);

    if (WARN_ON(_xfer == NULL))
        return;

    kref_put(&xfer->refs, __usb_transfer_release);
}

void *usb_transfer_packet_alloc (
    struct usb_transfer *_xfer
){
    struct usb_cache_private *cache;
    struct usb_transfer_private *xfer = xfer_from_public(_xfer);
    struct usb_packet *packet;
    wait_queue_entry_t wait;
    unsigned long xfer_flags, cache_flags;
    size_t index;

    if (WARN_ON(_xfer == NULL))
        return NULL;

    cache = xfer->cache;

    spin_lock_irqsave(&xfer->lock, xfer_flags);
    if (xfer->count >= MAX_TRANSFER_PACKETS) {
        spin_unlock_irqrestore(&xfer->lock, xfer_flags);
        return NULL;
    }

repeat:
    spin_lock_irqsave(&cache->lock, cache_flags);
    index = find_first_zero_bit(cache->packet_usage, MAX_PACKETS);
    if (likely(index < MAX_PACKETS)) {
        packet = &cache->packets[index];
        set_bit(index, cache->packet_usage);
    }

    if (likely(packet != NULL)) {
        spin_unlock_irqrestore(&cache->lock, cache_flags);

        xfer->packets[xfer->count] = packet;
        xfer->count++;
        spin_unlock_irqrestore(&xfer->lock, xfer_flags);

        return packet;
    }

    init_wait(&wait);
    prepare_to_wait(&cache->packet_wait, &wait, TASK_UNINTERRUPTIBLE);

    spin_unlock_irqrestore(&cache->lock, cache_flags);
    schedule();

    finish_wait(&cache->packet_wait, &wait);
    goto repeat;
}

struct usb_packet *usb_transfer_packet_get (
    struct usb_transfer *_xfer
){
    struct usb_transfer_private *xfer = xfer_from_public(_xfer);
    struct usb_packet *packet = NULL;
    unsigned long xfer_flags;

    if (WARN_ON(_xfer == NULL))
        return NULL;

    spin_lock_irqsave(&xfer->lock, xfer_flags);
    if (xfer->index < xfer->count) {
        packet = xfer->packets[xfer->index];
        xfer->index++;
    }
    spin_unlock_irqrestore(&xfer->lock, xfer_flags);

    return packet;
}

size_t usb_transfer_packet_count (
    struct usb_transfer *_xfer
){
    struct usb_transfer_private *xfer = xfer_from_public(_xfer);
    unsigned long xfer_flags;
    size_t count;

    if (WARN_ON(_xfer == NULL))
        return 0;

    spin_lock_irqsave(&xfer->lock, xfer_flags);
    count = xfer->count - xfer->index;
    spin_unlock_irqrestore(&xfer->lock, xfer_flags);

    return count;
}

#if defined(DEBUG)
void usb_transfer_debug (
    struct usb_transfer *_xfer
){
    struct usb_transfer_private *xfer = xfer_from_public(_xfer);
    unsigned long xfer_flags;

    if (WARN_ON(_xfer == NULL))
        return;

    spin_lock_irqsave(&xfer->lock, xfer_flags);
    pr_debug("Transfer: count=%ld, index=%ld", xfer->count, xfer->index);
    print_hex_dump(KERN_DEBUG, "data: ", DUMP_PREFIX_NONE, 32, 8, xfer->packets, sizeof(xfer->packets), false);
    spin_unlock_irqrestore(&xfer->lock, xfer_flags);
}
#endif

int usb_transfer_queue_init (
    struct usb_transfer_queue *_queue
){
    struct usb_transfer_queue_private *queue;

    if (WARN_ON(_queue == NULL))
        return -EINVAL;

    queue = kzalloc(sizeof(*queue), GFP_KERNEL);
    if (!queue)
        return -ENOMEM;

    spin_lock_init(&queue->lock);
    kref_init(&queue->refs);

    _queue->p = queue;

    return 0;
}

static void __usb_transfer_queue_destroy (
    struct kref *kref
){
    struct usb_transfer_queue_private *queue = queue_from_kref(kref);

    kfree(queue);
}

void usb_transfer_queue_release (
    struct usb_transfer_queue *_queue
){
    struct usb_transfer_queue_private *queue;

    if (WARN_ON(_queue == NULL))
        return;

    queue = _queue->p;

    kref_put(&queue->refs, __usb_transfer_queue_destroy);
}

int usb_transfer_queue_count (
    struct usb_transfer_queue *_queue
){
    struct usb_transfer_queue_private *queue;
    unsigned long flags;
    size_t count;

    if (WARN_ON(_queue == NULL))
        return 0;

    queue = _queue->p;

    spin_lock_irqsave(&queue->lock, flags);
    count = queue->count;
    spin_unlock_irqrestore(&queue->lock, flags);

    return count;
}

struct usb_transfer *usb_transfer_queue_peek (
    struct usb_transfer_queue *_queue
){
    struct usb_transfer_queue_private *queue;
    struct usb_transfer *xfer = NULL;
    unsigned long flags;

    if (WARN_ON(_queue == NULL))
        return NULL;

    queue = _queue->p;

    spin_lock_irqsave(&queue->lock, flags);
    if (queue->count)
        xfer = queue->data[queue->read_index];
    spin_unlock_irqrestore(&queue->lock, flags);

    return xfer;
}

void usb_transfer_queue_pop (
    struct usb_transfer_queue *_queue,
    int err
){
    struct usb_transfer_queue_private *queue;
    struct usb_transfer *xfer = NULL;
    unsigned long flags;

    if (WARN_ON(_queue == NULL))
        return;

    queue = _queue->p;

    spin_lock_irqsave(&queue->lock, flags);

    if (queue->count) {
        xfer = queue->data[queue->read_index];
        queue->data[queue->read_index] = NULL;

        queue->count--;
        queue->read_index = queue_next_index(queue->read_index);

        kref_put(&queue->refs, __usb_transfer_queue_destroy);
    }

    spin_unlock_irqrestore(&queue->lock, flags);

    if (xfer) {
        if (xfer->complete)
            xfer->complete(xfer->data, err);

        usb_transfer_put(xfer);
    }
}

void usb_transfer_queue_clear (
    struct usb_transfer_queue *_queue
){
    struct usb_transfer_queue_private *queue;
    unsigned long flags;

    if (WARN_ON(_queue == NULL))
        return;

    queue = _queue->p;

    spin_lock_irqsave(&queue->lock, flags);

    /*
        If the caller has already tried to release, then the final
        kref_put will cause the queue to become invalid when the
        loop completes.
     */
    kref_get(&queue->refs);

    while (queue->read_index != queue->write_index) {
        usb_transfer_put(queue->data[queue->read_index]);
        queue->data[queue->read_index] = NULL;

        /* Decrease the queue's usage counter */
        kref_put(&queue->refs, __usb_transfer_queue_destroy);
        queue->read_index = queue_next_index(queue->read_index);
    }

    queue->count = 0;
    queue->read_index = 0;
    queue->write_index = 0;

    spin_unlock_irqrestore(&queue->lock, flags);
    kref_put(&queue->refs, __usb_transfer_queue_destroy);
}

int usb_transfer_queue_push (
    struct usb_transfer_queue *_queue,
    struct usb_transfer *xfer
){
    struct usb_transfer_queue_private *queue;
    unsigned long flags;
    size_t count;

    if (WARN_ON(_queue == NULL || xfer == NULL))
        return -EINVAL;

    queue = _queue->p;

    usb_transfer_get(xfer);
    spin_lock_irqsave(&queue->lock, flags);

    queue->data[queue->write_index] = xfer;
    queue->write_index = queue_next_index(queue->write_index);
    queue->count++;
    kref_get(&queue->refs);

    count = queue->count;

    spin_unlock_irqrestore(&queue->lock, flags);

    return count;
}

#if defined(DEBUG)
void usb_transfer_queue_debug (
    struct usb_transfer_queue *_queue
){
    struct usb_transfer_queue_private *queue;
    unsigned long flags;

    if (WARN_ON(_queue == NULL))
        return;

    queue = _queue->p;

    spin_lock_irqsave(&queue->lock, flags);

    pr_debug("usb queue buffer read: %ld, write: %ld, count: %ld", queue->read_index, queue->write_index, queue->count);
    print_hex_dump(KERN_DEBUG, "data: ", DUMP_PREFIX_NONE, 32, 8, queue->data, sizeof(queue->data), false);

    spin_unlock_irqrestore(&queue->lock, flags);
}
#endif
