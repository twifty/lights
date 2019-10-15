/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UAPI_AURA_ASYNC_H
#define _UAPI_AURA_ASYNC_H

#include "../aura.h"

struct async_queue {

};

/**
 * async_execute() - Callback function prototype
 * @data: User supplied data
 */
typedef void (*async_execute)(void* data);

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
 * @param queue [description]
 */
void async_queue_destroy (struct async_queue *queue);

void async_queue_pause (struct async_queue *queue);

void async_queue_resume (struct async_queue *queue);

error_t async_queue_add (struct async_queue *queue, async_execute callback, void *data);

error_t async_queue_add_unique (struct async_queue *queue, async_execute callback, void *data);

#endif
