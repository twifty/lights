// SPDX-License-Identifier: GPL-2.0
#include <linux/kref.h>
#include <linux/types.h> /* atomic_t */
#include <linux/workqueue.h>
#include "async.h"

enum queue_state {
    STATE_IDLE      = 1,
    STATE_RUNNING   = 2,
    STATE_PAUSED    = 3,
    STATE_CANCELLED = 4
};

struct async_job {
    struct work_struct      work;
    struct async_context    *context;
    bool                    unique;
    async_execute           callback;
    void                    *data;
};

struct async_context {
    struct async_queue      public;
    struct workqueue_struct *queue;
    spinlock_t              lock;
    atomic_t                unique_count;
    enum queue_state        state;
    wait_queue_head_t       pause_wait;
    wait_queue_head_t       resume_wait;
    struct kref             refs;

    size_t                  pool_size;
    unsigned long           *pool_usage;
    struct async_job        pool[];
};

#define context_from_public(ptr)\
    container_of(ptr, struct async_context, public)

#define context_from_kref(ptr) \
    container_of(ptr, struct async_context, refs)

#define job_from_work(ptr) \
    container_of(ptr, struct async_job, work)

static void __context_release (
    struct kref *kref
){
    struct async_context *context = context_from_kref(kref);

    destroy_workqueue(context->queue);
    kfree(context->pool_usage);
    kfree(context);
}

static struct async_job *__pool_alloc (
    struct async_context *context
){
    struct async_job *job = NULL;
    size_t index;

    spin_lock(&context->lock);

    index = find_first_zero_bit(context->pool_usage, context->pool_size);
    if (likely(index < context->pool_size)) {
        set_bit(index, context->pool_usage);
        kref_get(&context->refs);
        job = &context->pool[index];
    }

    spin_unlock(&context->lock);

    return job;
}

static void __pool_free (
    struct async_job *job
){
    struct async_context *context = job->context;
    ssize_t index;

    index = (job - context->pool) / sizeof(*job);
    if (index >= 0 && index < context->pool_size) {
        spin_lock(&context->lock);
        clear_bit(index, context->pool_usage);
        spin_unlock(&context->lock);
    } else {
        AURA_ERR("Job index (%ld) out of range", index);
    }

    kref_put(&context->refs, __context_release);
}

static void __execute_job (
    struct work_struct *work
){
    struct async_job *job = job_from_work(work);
    struct async_context *context = job->context;
    int max_attempts = 10;

    while (0 < max_attempts--) {
        switch (context->state) {
            case STATE_IDLE:
                AURA_ERR("STATE_IDLE detected when running a job");
                // Fall through
            case STATE_CANCELLED:
                goto free;
            case STATE_PAUSED:
                wake_up_all(&context->pause_wait);
                wait_event_interruptible(context->resume_wait, STATE_PAUSED != context->state);
                // Loop again to read new state
                continue;
            case STATE_RUNNING:
                if (job->unique) {
                    if (atomic_read(&context->unique_count) == 1)
                        job->callback(job->data);
                    atomic_dec(&context->unique_count);
                } else {
                    job->callback(job->data);
                }
                goto free;
        }
    }

free:
    spin_lock(&context->lock);
    if (kref_read(&context->refs) <= 1 && context->state != STATE_CANCELLED)
        context->state = STATE_IDLE;
    spin_unlock(&context->lock);

    __pool_free(job);
}

static error_t __create_job (
    struct async_context *context,
    async_execute callback,
    void *data,
    bool unique
){
    struct async_job *job;

    job = __pool_alloc(context);
    if (!job)
        return -ENOMEM;

    job->callback = callback;
    job->data = data;
    job->unique = unique;

    if (unique)
        atomic_inc(&context->unique_count);

    spin_lock(&context->lock);

    switch (context->state) {
        case STATE_IDLE:
            context->state = STATE_RUNNING;
            // Fall through
        case STATE_RUNNING:
        case STATE_PAUSED:
            spin_unlock(&context->lock);
            if (!queue_work(context->queue, &job->work)) {
                AURA_ERR("Failed to queue work item");
                goto error;
            }
            return 0;

        case STATE_CANCELLED:
            spin_unlock(&context->lock);
            AURA_ERR("Cannot add job to a closed queue");
            goto error;
    }

error:
    if (unique)
        atomic_dec(&context->unique_count);
    __pool_free(job);

    return -EIO;
}

struct async_queue *async_queue_create (
    const char *name,
    size_t pool_size
){
    struct async_context *context;
    int i;

    size_t pool_alloc = sizeof(struct async_job) * pool_size;
    context = kzalloc(sizeof(*context) + pool_alloc, GFP_KERNEL);
    if (!context)
        return ERR_PTR(-ENOMEM);

    context->pool_usage = kcalloc(sizeof(*context->pool_usage), BITS_TO_LONGS(pool_size), GFP_KERNEL);
    if (!context->pool_usage) {
        kfree(context);
        return ERR_PTR(-ENOMEM);
    }

    kref_init(&context->refs);
    spin_lock_init(&context->lock);
    init_waitqueue_head(&context->pause_wait);
    init_waitqueue_head(&context->resume_wait);
    atomic_set(&context->unique_count, 0);

    context->pool_size = pool_size;
    context->state = STATE_IDLE;
    context->queue = create_singlethread_workqueue(name);

    if (!context->queue) {
        AURA_ERR("Failed to create (%s) workqueue", name);
        kfree(context->pool_usage);
        kfree(context);
        return NULL;
    }

    for (i = 0; i < pool_size; i++) {
        INIT_WORK(&context->pool[i].work, __execute_job);
        context->pool[i].context = context;
    }

    return &context->public;
}

void async_queue_destroy (
    struct async_queue *queue
){
    struct async_context *context = context_from_public(queue);

    if (WARN_ON(NULL == queue))
        return;

    spin_lock(&context->lock);
    context->state = STATE_CANCELLED;
    spin_unlock(&context->lock);

    kref_put(&context->refs, __context_release);
}

void async_queue_pause (
    struct async_queue *queue
){
    struct async_context *context = context_from_public(queue);
    int max_attempts = 10;

    if (WARN_ON(NULL == queue))
        return;

    spin_lock(&context->lock);

    while (0 < max_attempts--) {
        if (context->state == STATE_RUNNING) {
            context->state = STATE_PAUSED;

            spin_unlock(&context->lock);
            wait_event_interruptible(context->pause_wait, STATE_RUNNING != context->state);
            spin_lock(&context->lock);

            continue;
        } else if (context->state != STATE_CANCELLED) {
            context->state = STATE_PAUSED;
        }
    }

    spin_unlock(&context->lock);
}

void async_queue_resume (
    struct async_queue *queue
){
    struct async_context *context = context_from_public(queue);

    if (WARN_ON(NULL == queue))
        return;

    spin_lock(&context->lock);

    if (context->state == STATE_PAUSED) {
        if (kref_read(&context->refs) > 1) {
            context->state = STATE_RUNNING;
            wake_up(&context->resume_wait);
        } else {
            context->state = STATE_IDLE;
        }
    }

    spin_unlock(&context->lock);
}

error_t async_queue_add (
    struct async_queue *queue,
    async_execute callback,
    void *data
){
    if (WARN_ON(NULL == queue || NULL == callback))
        return -EINVAL;

    return __create_job(context_from_public(queue), callback, data, false);
}

error_t async_queue_add_unique (
    struct async_queue *queue,
    async_execute callback,
    void *data
){
    if (WARN_ON(NULL == queue || NULL == callback))
        return -EINVAL;

    return __create_job(context_from_public(queue), callback, data, true);
}
