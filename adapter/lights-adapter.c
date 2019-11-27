// SPDX-License-Identifier: GPL-2.0
#include "lights-adapter.h"
#include "lib/reserve.h"
#include "lib/async.h"

/* Forward declare for the vtable */
static error_t lights_adapter_smbus_read (
    const struct lights_adapter_client * client,
    struct lights_adapter_msg * msg
);
static error_t lights_adapter_smbus_write (
    const struct lights_adapter_client * client,
    const struct lights_adapter_msg * msg
);

struct lights_adapter_vtable {
    enum lights_adapter_protocol proto;
    error_t (*read)(const struct lights_adapter_client *, struct lights_adapter_msg *);
    error_t (*write)(const struct lights_adapter_client *, const struct lights_adapter_msg *);
} lights_adapter_vtables[] = {{
    .proto = LIGHTS_PROTOCOL_SMBUS,
    .read  = lights_adapter_smbus_read,
    .write = lights_adapter_smbus_write,
},{
    .proto = LIGHTS_PROTOCOL_I2C,
    .read  = lights_adapter_smbus_read,
    .write = lights_adapter_smbus_write,
}};

static inline const struct lights_adapter_vtable *lights_adapter_vtable_get (
    enum lights_adapter_protocol proto
){
    switch (proto) {
        case LIGHTS_PROTOCOL_SMBUS:
            return &lights_adapter_vtables[0];
        case LIGHTS_PROTOCOL_I2C:
            return &lights_adapter_vtables[1];
        case LIGHTS_PROTOCOL_USB:
            break;
    }

    LIGHTS_ERR("Not a valid adapter protocol: 0x%x", proto);

    return ERR_PTR(-EINVAL);
}

/**
 * struct lights_adapter_context - I2C wrapper
 *
 * @adapter:     vtable container (TODO: remove)
 * @lock:        Context lock
 * @mempool:     Pool of @lights_context_job
 * @async_queue: Single thread to execute async jobs
 * @i2c_adapter: The adapter being wrapped
 */
struct lights_adapter_context {
    const struct lights_adapter_vtable  *vtable;
    struct list_head                    siblings;
    struct kref                         refs;
    struct mutex                        lock;
    reserve_t                           reserve;
    async_queue_t                       async_queue;
    size_t                              max_async;
    const char                          *name;

    atomic_t                            allocated_jobs;

    union {
        struct i2c_adapter              *i2c_adapter;
    };
};
#define adapter_from_ref(ptr)( \
    container_of(ptr, struct lights_adapter_context, refs) \
)

static LIST_HEAD(lights_adapter_list);
static DEFINE_SPINLOCK(lights_adapter_lock);

/**
 * lights_adapter_find() - Searches for a context for a client
 *
 * @client: Client whose context to find
 *
 * @return: NULL or a reference counted context
 *
 * While a client may not have an associated context, there may
 * have been one created for the underlying device.
 */
static struct lights_adapter_context *lights_adapter_find (
    const struct lights_adapter_client *client
){
    struct lights_adapter_context *context;

    if (IS_NULL(client, lights_adapter_vtable_get(client->proto)))
        return ERR_PTR(-EINVAL);

    if (client->adapter) {
        kref_get(&client->adapter->refs);
        return client->adapter;
    }

    list_for_each_entry(context, &lights_adapter_list, siblings) {
        if (context->vtable->proto == client->proto) {
            switch (client->proto) {
                case LIGHTS_PROTOCOL_SMBUS:
                    if (context->i2c_adapter == client->smbus_client.adapter) {
                        kref_get(&context->refs);
                        return context;
                    }
                    break;
                case LIGHTS_PROTOCOL_I2C:
                    if (context->i2c_adapter == client->i2c_client.adapter) {
                        kref_get(&context->refs);
                        return context;
                    }
                    break;
                case LIGHTS_PROTOCOL_USB:
                    break;
            }
        }
    }

    return NULL;
}

/**
 * struct lights_adapter_job - Storage for queued job data
 *
 * @async:      Required by async
 * @msg:        Linked list of messaged
 * @client:     Adapter and address
 * @completion: Callers completion handler
 * @priv_data:  Callers suplemental completion data
 *
 * The objects are memory pool managed. A linked list of messages
 * consists of one fully initialized job head, followed by zero
 * or more partially initialized job.
 */
struct lights_adapter_job {
    struct async_job                async;
    struct lights_adapter_msg       msg;
    struct lights_adapter_client    client;
    lights_adapter_done_t           completion;
    void                            *priv_data;
};
#define job_from_async(ptr)( \
    container_of(ptr, struct lights_adapter_job, async) \
)
#define job_from_msg(ptr)( \
    container_of(ptr, struct lights_adapter_job, msg) \
)

/**
 * reserve_alloc_job() - Fetches a job from the memory pool
 *
 * @context: Created when calling @lights_adapter_register
 *
 * @return: An empty job (don't assume the memory is zeroed)
 */
static inline struct lights_adapter_job *reserve_alloc_job (
    struct lights_adapter_context * context
){
    struct lights_adapter_job *job = reserve_alloc(context->reserve);

    if (!IS_ERR_OR_NULL(job)) {
        memset(job, 0, sizeof(*job));
        atomic_inc(&context->allocated_jobs);
    }

    return job;
}

/**
 * reserve_free_job() - Returns a job to the memory pool
 *
 * @context: Created when calling @lights_adapter_register
 * @job:     Previously created with @reserve_alloc_job
 */
static inline void reserve_free_job (
    struct lights_adapter_context * context,
    struct lights_adapter_job const * job
){
    if (IS_NULL(context, job))
        return;

    atomic_dec(&context->allocated_jobs);
    reserve_free(context->reserve, job);
}

/**
 * lights_adapter_destroy() - Destructor
 *
 * @ref: Reference counter
 */
static void lights_adapter_destroy (
    struct kref *ref
){
    struct lights_adapter_context *context = adapter_from_ref(ref);
    int alloc;

    spin_lock(&lights_adapter_lock);
    list_del(&context->siblings);
    spin_unlock(&lights_adapter_lock);

    LIGHTS_DBG("Releasing adapter '%s'", context->i2c_adapter->name);

    if (context->async_queue)
        async_queue_destroy(context->async_queue);

    alloc = atomic_read(&context->allocated_jobs);
    if (alloc > 0) {
        LIGHTS_ERR("Reserve contains %d unallocated jobs", alloc);
    }

    if (context->reserve)
        reserve_put(context->reserve);

    kfree(context);
}

/**
 * lights_adapter_smbus_read() - Processes a single read
 *
 * @client: Provided by the adapter caller
 * @msg:    Provided by the adapter caller
 *
 * @return: Zero or negative error number
 *
 * Read values are stored in the given @msg.
 */
static error_t lights_adapter_smbus_read (
    const struct lights_adapter_client *client,
    struct lights_adapter_msg *msg
){
    s32 result = -EIO;

    switch (msg->type) {
        case I2C_SMBUS_BYTE:
            result = i2c_smbus_read_byte(&client->i2c_client);
            msg->data.byte = (u8)result;
            msg->length = 1;
            break;
        case I2C_SMBUS_BYTE_DATA:
            result = i2c_smbus_read_byte_data(&client->i2c_client, msg->command);
            msg->data.byte = (u8)result;
            msg->length = 1;
            break;
        case I2C_SMBUS_WORD_DATA:
            if (msg->swapped)
                result = i2c_smbus_read_word_swapped(&client->i2c_client, msg->command);
            else
                result = i2c_smbus_read_word_data(&client->i2c_client, msg->command);
            msg->data.word = (u16)result;
            msg->length = 2;
            break;
        case I2C_SMBUS_BLOCK_DATA:
            result = i2c_smbus_read_block_data(&client->i2c_client, msg->command, msg->data.block);
            msg->length = (u8)result;
            break;
        default:
            return -EINVAL;
    }

    return result < 0 ? result : 0;
}

/**
 * lights_adapter_smbus_write() - Processes a single write
 *
 * @client: Provided by the adapter caller
 * @msg:    Provided by the adapter caller
 *
 * @return: Zero or negative error number
 */
static error_t lights_adapter_smbus_write (
    const struct lights_adapter_client * client,
    const struct lights_adapter_msg * msg
){
    int result = -EIO;

    switch (msg->type) {
        case I2C_SMBUS_BYTE:
            result = i2c_smbus_write_byte(&client->i2c_client, msg->data.byte);
            break;
        case I2C_SMBUS_BYTE_DATA:
            result = i2c_smbus_write_byte_data(&client->i2c_client, msg->command, msg->data.byte);
            break;
        case I2C_SMBUS_WORD_DATA:
            if (msg->swapped)
                result = i2c_smbus_write_word_swapped(&client->i2c_client, msg->command, msg->data.word);
            else
                result = i2c_smbus_write_word_data(&client->i2c_client, msg->command, msg->data.word);
            break;
        case I2C_SMBUS_BLOCK_DATA:
            result = i2c_smbus_write_block_data(&client->i2c_client, msg->command, msg->length, msg->data.block);
            break;
        default:
            return -EINVAL;
    }

    return result < 0 ? result : 0;
}

/**
 * lights_adapter_job_free() - returns the job to the memory pool
 *
 * @job: A linked list of jobs (linked through @job->msg.next)
 *
 * NOTE: Only the head of the list is fully initialized, all
 * siblings only contain a msg.
 */
static int lights_adapter_job_free (
    const struct lights_adapter_job * job
){
    struct lights_adapter_context * context;
    struct lights_adapter_job const * next;
    int sanity = LIGHTS_ADAPTER_MAX_MSGS;
    int count = 0;

    if (IS_NULL(job))
        return 0;

    context = job->client.adapter;
    if (IS_NULL(context))
        return 0;

    do {
        next = job->msg.next ? job_from_msg(job->msg.next) : NULL;
        count++;

        reserve_free_job(context, job);
        job = next;
    } while (job && --sanity);

    if (!sanity)
        LIGHTS_ERR("Message count exceeded LIGHTS_ADAPTER_MAX_MSGS");

    return count;
}

/**
 * lights_adapter_job_execute() - Processes a list of queued messages
 *
 * @async_job: The head of the linked list
 */
static void lights_adapter_job_execute (
    struct async_job *async_job,
    enum async_queue_state state
){
    struct lights_adapter_job * const job = job_from_async(async_job);
    struct lights_adapter_context * context;
    struct lights_adapter_msg * msg;
    int sanity = LIGHTS_ADAPTER_MAX_MSGS;
    int count = 0;
    error_t err = 0;

    if (IS_NULL(async_job))
        return;

    context = job->client.adapter;
    msg = &job->msg;

    if (!context) {
        LIGHTS_ERR("Job submitted without a context");
        return;
    }

    if (state == ASYNC_STATE_RUNNING) {
        mutex_lock(&context->lock);

        /* Process each message in the job */
        while (msg && --sanity) {
            if (msg->read)
                err = context->vtable->read(&job->client, msg);
            else
                err = context->vtable->write(&job->client, msg);

            if (err)
                break;

            msg = msg->next;
            count++;
        }

        mutex_unlock(&context->lock);

        if (!sanity)
            LIGHTS_ERR("Message count exceeded LIGHTS_ADAPTER_MAX_MSGS");

        /* Notify caller, pass the erroring message, or first */
        job->completion(err ? msg : &job->msg, job->priv_data, err);
    } else {
        job->completion(&job->msg, job->priv_data, -ECANCELED);
    }

    if (count != lights_adapter_job_free(job))
        LIGHTS_ERR("Disparity between job count and jobs freed");
}

/**
 * lights_adapter_job_create() - Creates a linked list of messages
 *
 * @context: Created when calling @lights_adapter_register
 * @count:   Number of @msg objects
 * @msg:     Array of messages
 *
 * @return: The head of the linked list.
 *
 * Each message is copied into the list.
 */
static struct lights_adapter_job *lights_adapter_job_create (
    struct lights_adapter_context * const context,
    size_t count,
    struct lights_adapter_msg * const msg
){
    struct lights_adapter_job *head, *prev, *job;
    int i;

    head = prev = NULL;
    for (i = 0; i < count; i++) {
        job = reserve_alloc_job(context);
        if (IS_ERR(job)) {

        }

        memcpy(&job->msg, &msg[i], sizeof(*msg));
        job->msg.next = NULL;

        if (!head) {
            head = prev = job;
        } else {
            prev->msg.next = &job->msg;
            prev = job;
        }
    }

    INIT_ASYNC_JOB(&head->async, lights_adapter_job_execute);

    return head;
}

/**
 * lights_adapter_init() - Creates the pool and queue for the adapter
 *
 * @context: Adapter on which to begin async transactions
 *
 * @return: Zero or a negative error number
 */
static error_t lights_adapter_init (
    struct lights_adapter_context *context
){
    error_t err = 0;

    if (context->async_queue && context->reserve)
        return 0;

    mutex_lock(&context->lock);

    if (!context->async_queue) {
        context->async_queue = async_queue_create(context->name, context->max_async);
        if (IS_ERR(context->async_queue)) {
            err = CLEAR_ERR(context->async_queue);
            goto error;
        }
    }

    if (!context->reserve) {
        context->reserve = reserve_get(lights_adapter_job, context->max_async, SLAB_POISON, GFP_KERNEL);
        if (IS_ERR(context->reserve)) {
            err = CLEAR_ERR(context->reserve);
            goto error;
        }
    }

error:
    mutex_unlock(&context->lock);

    return err;
}


/**
 * lights_adapter_xfer() - Synchronous reads/writes
 *
 * @client:    Hardware parameters
 * @msgs:      One or more messages to send
 * @msg_count: Number of messages to send
 *
 * @return: Zero or a negative error code
 */
error_t lights_adapter_xfer (
    const struct lights_adapter_client *client,
    struct lights_adapter_msg *msgs,
    size_t count
){
    struct lights_adapter_context * context;
    const struct lights_adapter_vtable * vtable;
    error_t err = 0;
    int i;

    if (IS_NULL(client, msgs) || IS_TRUE(0 == count) || IS_TRUE(count > LIGHTS_ADAPTER_MAX_MSGS))
        return -EINVAL;

    vtable = lights_adapter_vtable_get(client->proto);
    if (IS_ERR(vtable))
        return CLEAR_ERR(vtable);

    context = lights_adapter_find(client);
    if (IS_ERR(context))
        return CLEAR_ERR(context);

    if (context) {
        if (context->async_queue)
            async_queue_pause(context->async_queue);
        mutex_lock(&context->lock);
    }

    for (i = 0; i < count && !err; i++) {
        if (msgs[i].read)
            err = vtable->read(client, &msgs[i]);
        else
            err = vtable->write(client, &msgs[i]);
    }

    if (context) {
        mutex_unlock(&context->lock);
        if (context->async_queue)
            async_queue_resume(context->async_queue);

        kref_put(&context->refs, lights_adapter_destroy);
    }

    return err;
}
EXPORT_SYMBOL_NS_GPL(lights_adapter_xfer, LIGHTS);

/**
 * lights_adapter_xfer_async() - Asynchronous reads/writes
 *
 * @client:    Hardware parameters
 * @msgs:      One or more messages to send
 * @msg_count: Number of messages to send
 * @cb_data:   Second parameter of @callback
 * @callback:  Completion function
 *
 * @return: Zero or a negative error code
 */
error_t lights_adapter_xfer_async (
    const struct lights_adapter_client *client,
    struct lights_adapter_msg *msgs,
    size_t count,
    void *cb_data,
    lights_adapter_done_t callback
){
    struct lights_adapter_context * context;
    struct lights_adapter_job *job;
    error_t err = 0;

    if (IS_NULL(client, client->adapter, msgs) || IS_TRUE(0 == count) || IS_TRUE(count > LIGHTS_ADAPTER_MAX_MSGS))
        return -EINVAL;

    context = client->adapter;

    err = lights_adapter_init(context);
    if (err) {
        LIGHTS_ERR("Failed to initialize adapter async: %d", err);
        kref_put(&context->refs, lights_adapter_destroy);
        return err;
    }

    /* Create a linked list of jobs, one for each message */
    job = lights_adapter_job_create(context, count, msgs);
    job->client     = *client;
    job->completion = callback;
    job->priv_data  = cb_data;

    err = async_queue_add(context->async_queue, &job->async);
    if (err) {
        LIGHTS_ERR("Failed to add async job: %d", err);
        lights_adapter_job_free(job);
    }

    return err;
}
EXPORT_SYMBOL_NS_GPL(lights_adapter_xfer_async, LIGHTS);

/**
 * lights_adapter_unregister() - Releases an async adapter
 *
 * @client: The client which holds the adapter
 *
 * See @lights_context_adapter_create for more info
 */
void lights_adapter_unregister (
    struct lights_adapter_client *client
){
    if (!(client && client->adapter))
        return;

    kref_put(&client->adapter->refs, lights_adapter_destroy);

    client->adapter = NULL;
}
EXPORT_SYMBOL_NS_GPL(lights_adapter_unregister, LIGHTS);

/**
 * lights_adapter_register - Associates an I2C/SMBUS adapter
 *
 * @client:    The client and adapter being wrapped
 * @max_async: A maximum number of pending jobs
 *
 * @return: Zero or a negative error number
 */
error_t lights_adapter_register (
    struct lights_adapter_client *client,
    size_t max_async
){
    struct lights_adapter_context *context;
    const struct lights_adapter_vtable *vtable;

    if (IS_NULL(client))
        return -EINVAL;

    if (client->adapter) {
        LIGHTS_ERR("Adapter is already registered.");
        return -EINVAL;
    }

    vtable = lights_adapter_vtable_get(client->proto);
    if (IS_ERR(vtable))
        return PTR_ERR(vtable);

    context = lights_adapter_find(client);
    if (IS_ERR(context))
        return PTR_ERR(context);

    if (!context) {
        context = kzalloc(sizeof(*context), GFP_KERNEL);
        if (!context)
            return -ENOMEM;

        mutex_init(&context->lock);
        kref_init(&context->refs);
        atomic_set(&context->allocated_jobs, 0);
        context->max_async = max_async;
        context->vtable = vtable;

        switch (client->proto) {
            case LIGHTS_PROTOCOL_SMBUS:
                context->i2c_adapter = client->smbus_client.adapter;
                context->name = context->i2c_adapter->name;
                LIGHTS_DBG("Created SMBUS adapter '%s'", context->name);
                break;
            case LIGHTS_PROTOCOL_I2C:
                context->i2c_adapter = client->i2c_client.adapter;
                context->name = context->i2c_adapter->name;
                LIGHTS_DBG("Created I2C adapter '%s'", context->name);
                break;
            case LIGHTS_PROTOCOL_USB:
                return -EIO;
        }

        spin_lock(&lights_adapter_lock);
        list_add_tail(&context->siblings, &lights_adapter_list);
        spin_unlock(&lights_adapter_lock);
    }

    client->adapter = context;

    return 0;
}
EXPORT_SYMBOL_NS_GPL(lights_adapter_register, LIGHTS);
