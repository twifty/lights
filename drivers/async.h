/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UAPI_DRIVER_ASYNC_H
#define _UAPI_DRIVER_ASYNC_H

#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/wait.h>
#include "drivers.h"

struct async_queue {

};

/**
 * async_execute() - Callback function prototype
 * @data: User supplied data
 */
typedef void (*async_execute)(void* data);

/**
 * async_free() - Memory release prototype
 * @data: User supplied data
 */
typedef void (*async_free)(void* data);

/**
 * async_queue_create()
 * @name:       A unique name for the queue
 * @pool_size:  The maximum number of job entries
 *
 * @return:     A queue object
 */
struct async_queue *async_queue_create (const char *name, size_t pool_size);

/**
 * async_queue_destroy() - Releases the queue
 *
 * @queue: The queue returned by @async_queue_create()
 */
void async_queue_destroy (struct async_queue *queue);

/**
 * async_queue_pause() - Pauses the queue
 *
 * @queue: The queue returned by @async_queue_create()
 *
 * Pausing the queue must be followed by a call to @async_queue_pause.
 * Any blocking calls should first pause a queue, read/write to the
 * device, then resume.
 */
void async_queue_pause (struct async_queue *queue);

/**
 * async_queue_resume() - Resumes the queue
 *
 * @queue: The queue returned by @async_queue_create()
 */
void async_queue_resume (struct async_queue *queue);

/**
 * async_queue_add() - Adds a job to the queue
 *
 * @queue:    The queue returned by @async_queue_create()
 * @data:     Job data, passed to the callback
 * @callback: The job to execute
 * @release:  If given, called to free @data
 *
 * @return: zero or a negative error number.
 *
 * The @callback will be ignored after a call to @async_queue_destroy.
 * For all jobs processed, the @data argument will be passed to @release
 * regardless of the @callback being called or not. It is up to the caller
 * to implement suitable reference counting on @data.
 */
error_t async_queue_add (struct async_queue *queue, void *data, async_execute callback, async_free release);

/**
 * async_queue_add_unique() - Adds a unique job to the queue
 *
 * @queue:    The queue returned by @async_queue_create()
 * @data:     Job data, passed to the callback
 * @callback: The job to execute
 * @release:  If given, called to free @data
 *
 * @return: zero or a negative error number.
 *
 * If any unique jobs already exist in the queue, the @callback will be skipped.
 * This means that only the last unique job in the queue will run.
 */
error_t async_queue_add_unique (struct async_queue *queue, void *data, async_execute callback, async_free release);

#endif
