/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UAPI_LIGHTS_ADAPTER_ASYNC_H
#define _UAPI_LIGHTS_ADAPTER_ASYNC_H

#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/wait.h>

#include <adapter/debug.h>
#include <include/types.h>

/* Forward declaration */
struct async_job;
struct async_queue;

typedef enum async_queue_state {
    ASYNC_STATE_IDLE      = 1,
    ASYNC_STATE_RUNNING   = 2,
    ASYNC_STATE_PAUSED    = 4,
    ASYNC_STATE_CANCELLED = 8
} async_state_t;

/* Keep struct async_context members private */
typedef struct async_queue *async_queue_t;
typedef struct async_job *async_job_t;

/**
 * typedef async_execute_t - Callback function prototype
 *
 * @job:    Previously queued job
 * @status: Status of the transaction
 *
 * If @status is any value other than ASYNC_STATE_RUNNING, the function
 * should only free any associated memory.
 */
typedef void (*async_execute_t)(async_job_t job, async_state_t status);

/**
 * struct async_job - Callback data
 *
 * @execute:  Callback function
 * @siblings: Next and Prev pointers
 * @context:  Owning queue
 *
 * Note, This struct is declared so that it can be embedded with
 * allocations belonging to the caller.
 */
struct async_job {
    async_execute_t     execute;

    /* Private */
    struct list_head    siblings;
    async_queue_t       context;
};
#define INIT_ASYNC_JOB(_job, _exec) ({ \
    (_job)->execute = (_exec); \
})

/**
 * async_queue_create()
 *
 * @name:       A unique name for the queue
 * @pool_size:  The maximum number of job entries
 *
 * @return: A queue object
 */
async_queue_t async_queue_create (const char *name, size_t pool_size);

/**
 * async_queue_destroy() - Releases the queue
 *
 * @queue: The queue returned by @async_queue_create()
 */
void async_queue_destroy (async_queue_t queue);

/**
 * async_queue_pause() - Pauses the queue
 *
 * @queue: The queue returned by @async_queue_create()
 *
 * Pausing the queue must be followed by a call to @async_queue_resume.
 * This function will block until the queue is in a paused state. Any
 * number of threads may call, however it is up to the caller to ensure
 * exclusive access to the underlying device.
 */
void async_queue_pause (async_queue_t queue);

/**
 * async_queue_resume() - Resumes the queue
 *
 * @queue: The queue returned by @async_queue_create()
 *
 * Only when the final thread holding a pause calls this function will
 * the queue continue to resume.
 */
void async_queue_resume (async_queue_t queue);

/**
 * async_queue_add() - Adds a job to the queue
 *
 * @queue: The queue returned by @async_queue_create()
 * @job:   Job data initialized with INIT_ASYNC_JOB()
 *
 * @return: zero or a negative error number.
 */
error_t async_queue_add (async_queue_t queue, async_job_t const job);

#endif
