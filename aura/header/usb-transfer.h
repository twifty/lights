/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UAPI_AURA_USB_TRANSFER_H
#define _UAPI_AURA_USB_TRANSFER_H

/*
 * Memory managment for usb packets.
 *
 * Copyright (C) 2019 Owen Parry
 *
 * Authors:
 * Owen Parry <twifty@zoho.com>
 */
#include <linux/types.h>

#ifndef PACKET_SIZE
#define PACKET_SIZE             65 /* Defined by the USB device */
#endif

#ifndef MAX_TRANSFERS
#define MAX_TRANSFERS           16 /* Increase if memory allocation errors */
#endif

#ifndef MAX_TRANSFER_PACKETS
#define MAX_TRANSFER_PACKETS    16 /* Allows for 13 data packets (256 leds) + 3 additional */
#endif

struct usb_packet {
    char    data[PACKET_SIZE];
};

struct usb_transfer {
    void    *data;
    void    (*complete)(void *data, int status);
};

struct usb_transfer_cache {
    void    *p;
};

/**
 * usb_transfer_cache_init() - Initializes the memory for transfers and packets
 * @cache:  The cache instance to initialize
 *
 * @return: Negative on error zero on success
 */
int usb_transfer_cache_init (
    struct usb_transfer_cache *cache
);

/**
 * usb_transfer_cache_release() - Frees a previously initialized cache instance
 * @cache:  The cache instance to un-initialize
 *
 * NOTE - The instance will remain in memory until all transfers
 * have been released.
 */
void usb_transfer_cache_release (
    struct usb_transfer_cache *cache
);

/**
 * usb_transfer_create() - Allocates an empty transfer instance
 * @cache: The cache instance from which to allocate
 *
 * @return: The transfer instance or negative err
 * @context: kernel
 *
 * If no instances are available, the function will sleep until
 * an instance is becomes free by calling usb_transfer_put().
 */
struct usb_transfer *usb_transfer_create (
    struct usb_transfer_cache *cache
);

/**
 * usb_transfer_get() - Increases the ref count of the transfer instance
 * @xfer: A transfer allocated with usb_transfer_create()
 */
void usb_transfer_get (
    struct usb_transfer *xfer
);

/**
 * usb_transfer_get() - Decreases the ref count of the transfer instance
 * @xfer: A transfer allocated with usb_transfer_create()
 *
 * Once all references are gone, the object is free. It is not safe
 * to continue using the object after calling this function.
 */
void usb_transfer_put (
    struct usb_transfer *xfer
);

/**
 * usb_transfer_packet_alloc() - Allocates and adds a packet to the transfer.
 * @xfer: A transfer allocated with usb_transfer_create()
 *
 * @return: A usb_packet object or NULL
 *
 * The packets are added to an internal FIFO queue of MAX_TRANSFER_PACKETS
 * length. If no packets are immediately available, the function will sleep.
 *
 * NOTE - The function returns void to aid in casting to other types.
 */
void *usb_transfer_packet_alloc (
    struct usb_transfer *xfer
);

/**
 * usb_transfer_packet_get() -
 * @xfer: A transfer allocated with usb_transfer_create()
 *
 * @return: A usb_packet object or NULL
 *
 * This function pops a packet from the internal FIFO queue.
 */
struct usb_packet *usb_transfer_packet_get (
    struct usb_transfer *xfer
);

/**
 * usb_transfer_packet_count()
 * @xfer: A transfer allocated with usb_transfer_create()
 *
 * @return: The number of packets remaining in the queue
 */
size_t usb_transfer_packet_count (
    struct usb_transfer *xfer
);

#if defined(DEBUG)
void usb_transfer_debug (
    struct usb_transfer *xfer
);
#else
#define usb_transfer_debug(_)
#endif

/**
 * struct usb_transfer_queue
 * @p: internal use only
 *
 * A queue represents a FIFO queue of transfers.
 */
struct usb_transfer_queue {
    void *p;
};

/**
 * usb_transfer_queue_init() - Initializes the memory for the queue
 * @queue: The instance to initialize
 *
 * @return: Negative error code or 0
 */
int usb_transfer_queue_init (
    struct usb_transfer_queue *queue
);

/**
 * usb_transfer_queue_release() - Releases a previously initialized queue
 * @param queue [description]
 */
void usb_transfer_queue_release (
    struct usb_transfer_queue *queue
);

/**
 * usb_transfer_queue_count()
 * @queue: A previously allocated queue
 *
 * @return: The number of transfers remaining in the queue
 */
int usb_transfer_queue_count (
    struct usb_transfer_queue *queue
);

/**
 * usb_transfer_queue_peek() - Fetches a transfer without removing it
 * @queue: A previously allocated queue
 *
 * @return: A transfer object or NULL
 */
struct usb_transfer *usb_transfer_queue_peek (
    struct usb_transfer_queue *queue
);

/**
 * usb_transfer_queue_pop() - Fetches a transfer and removes it
 * @queue: A previously allocated queue
 * @err: The error value to pass to any callback
 */
void usb_transfer_queue_pop (
    struct usb_transfer_queue *queue,
    int err
);

/**
 * usb_transfer_queue_clear() - Removes all transfers
 * @queue: A previously allocated queue
 */
void usb_transfer_queue_clear (
    struct usb_transfer_queue *queue
);

/**
 * usb_transfer_queue_push() - Adds an item to the queue
 * @queue: A previously allocated queue
 * @xfer: The transfer object to add
 *
 * @return: The number of items in the queue
 */
int usb_transfer_queue_push (
    struct usb_transfer_queue *queue,
    struct usb_transfer *xfer
);

#if defined(DEBUG)
void usb_transfer_queue_debug (
    struct usb_transfer_queue *queue
);
#else
#define usb_transfer_queue_debug(queue)
#endif

#endif
