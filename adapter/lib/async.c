// SPDX-License-Identifier: GPL-2.0
#include <linux/kref.h>
#include <linux/workqueue.h>
#include "async.h"

/* Same length as internal */
#define WQ_NAME_LENGTH 24

/**
 * struct async_queue - Queue data
 *
 * @workqueue:      Single thread in kernel context
 * @jobs:           Linked list of pending jobs
 * @lock:           Lock for list
 * @state:          Current state of the queue
 * @paused:         Number of threads waiting for pause state
 * @thread_wait:    Thread resume event
 * @pause_wait:     Pause unlock event
 * @refs:           Reference counter
 * @name:           Name of the queue
 * @work:           Work data
 */
struct async_queue {
    struct workqueue_struct *workqueue;
    struct list_head        jobs;
    spinlock_t              lock;
    atomic_t                state;
    atomic_t                paused;
    wait_queue_head_t       thread_wait;
    wait_queue_head_t       pause_wait;
    struct kref             refs;
    char                    name[WQ_NAME_LENGTH];
    struct work_struct      work;
};
#define queue_from_kref(ptr) ( \
    container_of(ptr, struct async_queue, refs) \
)
#define queue_from_work(ptr) ( \
    container_of(ptr, struct async_queue, work) \
)

/**
 * switch_state() - Atomic change state
 *
 * @queue: Owning queue
 * @from:  Old state
 * @to:    New state
 *
 * @return: The value of the state before the change
 *
 * The value is only changed is the state is equal to @old.
 *
 * Success is evaluated by comparing @return and @old.
 */
static inline enum async_queue_state switch_state (
    struct async_queue *queue,
    enum async_queue_state from,
    enum async_queue_state to
){
    return atomic_cmpxchg(&queue->state, from, to);
}

/**
 * read_state() - Atomic read state
 *
 * @queue: Owning queue
 *
 * @return: Value of the state
 */
static inline enum async_queue_state read_state (
    struct async_queue *queue
){
    return atomic_read(&queue->state);
}

/**
 * has_state() - Ckecks if the state has a bit set
 *
 * @queue: Owning queue
 * @state: A bitmask of ASYNC_STATE_
 *
 * @return: True if bit is set
 */
static inline bool has_state (
    struct async_queue *queue,
    enum async_queue_state state
){
    return (read_state(queue) & state) != 0;
}

/**
 * remove_job() - Removed the first job from the queue
 *
 * @queue: Owning queue
 *
 * @return: NULL or the first job
 */
static inline struct async_job *remove_job (
    struct async_queue *queue
){
    struct async_job *job = list_first_entry_or_null(&queue->jobs, struct async_job, siblings);

    if (job) {
        spin_lock(&queue->lock);
        list_del(&job->siblings);
        spin_unlock(&queue->lock);
    }

    return job;
}

/**
 * async_context_release() - Frees the context when no more references
 *
 * @kref: Reference counter
 *
 * @destroy_workqueue will complete all pending items before it closes.
 * That shouldn't be a problem since the context is ref counted.
 */
static void async_context_release (
    struct kref *kref
){
    struct async_queue *queue = queue_from_kref(kref);

    LIGHTS_DBG("Waiting for workqueue to complete");
    flush_workqueue(queue->workqueue);

    LIGHTS_DBG("Releasing queue '%s'", queue->name);
    destroy_workqueue(queue->workqueue);

    kfree(queue);
}

/**
 * async_job_execute() - Runs a queued task
 *
 * @work: Work object for the current task
 *
 * The job will not run if the queue has been paused or cancelled.
 */
static void async_job_execute (
    struct work_struct *work
){
    struct async_queue *queue = queue_from_work(work);
    struct async_job *job;
    bool running = true;

    // kref_get(&queue->refs);

    while (running) {
        /* The state may change between this read and case statement */
        switch (read_state(queue)) {
            case ASYNC_STATE_IDLE:
                /* Switch to running state */
                if (ASYNC_STATE_IDLE == switch_state(queue, ASYNC_STATE_IDLE, ASYNC_STATE_RUNNING)) {
                    job = remove_job(queue);
                    if (job) {
                        // LIGHTS_DBG("Executing job");
                        job->execute(job, ASYNC_STATE_RUNNING);
                        // LIGHTS_DBG("Job execution complete");
                        // kref_put(&queue->refs, async_context_release);
                    }

                    /* Wake any pending master threads */
                    if (atomic_read(&queue->paused) > 0) {
                        switch_state(queue, ASYNC_STATE_RUNNING, ASYNC_STATE_PAUSED);
                        wake_up_interruptible_all(&queue->pause_wait);
                    } else {
                        switch_state(queue, ASYNC_STATE_RUNNING, ASYNC_STATE_IDLE);
                        // wake_up(&queue->master_wait);
                    }

                    /* Put this thread to sleep if queue is empty */
                    // LIGHTS_DBG("Putting async thread to sleep");
                    wait_event_interruptible(
                        queue->thread_wait,
                        has_state(queue, ASYNC_STATE_CANCELLED) || !list_empty(&queue->jobs)
                    );
                    // LIGHTS_DBG("Async thread woke with state %d", read_state(queue));
                }
                break;
            case ASYNC_STATE_PAUSED:
                /* Wait for the unpaused state */
            case ASYNC_STATE_RUNNING:
                /* Wait for an idle state */
                // LIGHTS_DBG("Putting async thread to sleep");
                wait_event_interruptible(
                    queue->thread_wait,
                    has_state(queue, ASYNC_STATE_IDLE | ASYNC_STATE_CANCELLED)
                );
                // LIGHTS_DBG("Async thread woke with state %d", read_state(queue));
                break;
            case ASYNC_STATE_CANCELLED:
                // LIGHTS_DBG("Cancel state detected");
                running = false;
                job = remove_job(queue);
                while (job) {
                    // LIGHTS_DBG("Releasing job");
                    job->execute(job, ASYNC_STATE_CANCELLED);
                    // kref_put(&queue->refs, async_context_release);
                    job = remove_job(queue);
                }
                break;
        }
    }

    // kref_put(&queue->refs, async_context_release);
}

/**
 * async_queue_create
 *
 * @name:       A unique name for the queue
 * @pool_size:  The maximum number of job entries
 *
 * @return: A queue object
 */
async_queue_t async_queue_create (
    const char *name,
    size_t pool_size
){
    struct async_queue *queue;

    if (IS_NULL(name) || IS_FALSE(name[0]))
        return ERR_PTR(-EINVAL);

    queue = kzalloc(sizeof(*queue), GFP_KERNEL);
    if (!queue)
        return ERR_PTR(-ENOMEM);

    strncpy(queue->name, name, WQ_NAME_LENGTH - 1);

    kref_init(&queue->refs);
    spin_lock_init(&queue->lock);
    init_waitqueue_head(&queue->pause_wait);
    init_waitqueue_head(&queue->thread_wait);
    INIT_WORK(&queue->work, async_job_execute);
    INIT_LIST_HEAD(&queue->jobs);
    atomic_set(&queue->state, ASYNC_STATE_IDLE);

    queue->workqueue = create_singlethread_workqueue(queue->name);

    if (!queue->workqueue) {
        LIGHTS_ERR("Failed to create (%s) workqueue", queue->name);
        kfree(queue);
        return NULL;
    }

    LIGHTS_DBG("Created queue '%s'", queue->name);

    return queue;
}

/**
 * async_queue_destroy() - Releases the queue
 *
 * @queue: The queue returned by @async_queue_create()
 */
void async_queue_destroy (
    async_queue_t queue
){
    if (IS_NULL(queue))
        return;

    LIGHTS_DBG("Setting cancel state");

    atomic_set(&queue->state, ASYNC_STATE_CANCELLED);
    wake_up_interruptible_all(&queue->thread_wait);
    wake_up_interruptible_all(&queue->pause_wait);

    if (!kref_put(&queue->refs, async_context_release))
        LIGHTS_DBG("Queue has %d open handles", kref_read(&queue->refs));
}

/**
 * async_queue_pause() - Pauses the queue
 *
 * @queue: The queue returned by @async_queue_create()
 *
 * Pausing the queue must be followed by a call to @async_queue_pause.
 * Any blocking calls should first pause a queue, read/write to the
 * device, then resume.
 */
void async_queue_pause (
    async_queue_t queue
){
    if (IS_NULL(queue))
        return;

    /* Signal all running threads that a pause is required */
    atomic_inc(&queue->paused);

    wait_event_interruptible(
        queue->pause_wait,
        has_state(queue, ASYNC_STATE_PAUSED | ASYNC_STATE_CANCELLED)
    );
}

/**
 * async_queue_resume() - Resumes the queue
 *
 * @queue: The queue returned by @async_queue_create()
 */
void async_queue_resume (
    async_queue_t queue
){
    if (IS_NULL(queue))
        return;

    if (atomic_dec_and_test(&queue->paused)) {
        if (switch_state(queue, ASYNC_STATE_PAUSED, ASYNC_STATE_IDLE)) {
            // wake_up(&queue->master_wait);
            wake_up_interruptible(&queue->thread_wait);
        }
    }
}

/**
 * async_queue_add() - Adds a job to the queue
 *
 * @queue: The queue returned by @async_queue_create()
 * @job:   Job data
 *
 * @return: zero or a negative error number.
 */
error_t async_queue_add (
    async_queue_t queue,
    async_job_t job
){
    if (IS_NULL(queue, job, job->execute))
        return -EINVAL;

    // LIGHTS_DBG("Adding job to queue");

    job->context = queue;

    // kref_get(&queue->refs);
    list_add_tail(&job->siblings, &queue->jobs);

    /* Start or wake the thread */
    if (ASYNC_STATE_IDLE == read_state(queue)) {
        queue_work(queue->workqueue, &queue->work);
        wake_up_interruptible(&queue->thread_wait);
    }

    return 0;
}
