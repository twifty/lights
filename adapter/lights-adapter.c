// SPDX-License-Identifier: GPL-2.0
#include <adapter/debug.h>

#include "lights-adapter.h"
#include "lib/reserve.h"
#include "lib/async.h"

#define dump_msg(msg, p, len) \
({                          \
    print_hex_dump_bytes(   \
        msg,                \
        DUMP_PREFIX_NONE,   \
        p,                  \
        len );      \
})

/* Forward declare for the vtable */
static error_t lights_adapter_smbus_read (
    struct lights_adapter_client const *client,
    struct lights_adapter_msg *msg
);
static error_t lights_adapter_smbus_write (
    struct lights_adapter_client const *client,
    struct lights_adapter_msg const *msg
);
static error_t lights_adapter_usb_read (
    struct lights_adapter_client const *client,
    struct lights_adapter_msg *msg
);
static error_t lights_adapter_usb_write (
    struct lights_adapter_client const *client,
    struct lights_adapter_msg const *msg
);

struct lights_adapter_vtable {
    enum lights_adapter_protocol proto;
    error_t (*read)(struct lights_adapter_client const *, struct lights_adapter_msg *);
    error_t (*write)(struct lights_adapter_client const *, struct lights_adapter_msg const *);
} lights_adapter_vtables[] = {{
    .proto = LIGHTS_PROTOCOL_SMBUS,
    .read  = lights_adapter_smbus_read,
    .write = lights_adapter_smbus_write,
},{
    .proto = LIGHTS_PROTOCOL_I2C,
    .read  = lights_adapter_smbus_read,
    .write = lights_adapter_smbus_write,
},{
    .proto = LIGHTS_PROTOCOL_USB,
    .read  = lights_adapter_usb_read,
    .write = lights_adapter_usb_write,
}};

static inline struct lights_adapter_vtable const *lights_adapter_vtable_get (
    enum lights_adapter_protocol proto
){
    switch (proto) {
        case LIGHTS_PROTOCOL_SMBUS:
            return &lights_adapter_vtables[0];
        case LIGHTS_PROTOCOL_I2C:
            return &lights_adapter_vtables[1];
        case LIGHTS_PROTOCOL_USB:
            return &lights_adapter_vtables[2];
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
    struct lights_adapter_vtable  const *vtable;
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
        struct usb_controller           *usb_controller;
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
    struct lights_adapter_client const *client
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
                    if (context->usb_controller == client->usb_client.controller) {
                        kref_get(&context->refs);
                        return context;
                    }
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
 * @thunk:      Callers suplemental completion data
 * @completion: Callers completion handler
 *
 * The objects are memory pool managed. A linked list of messages
 * consists of one fully initialized job head, followed by zero
 * or more partially initialized job.
 */
struct lights_adapter_job {
    struct async_job                async;
    struct lights_adapter_msg       msg;
    struct lights_adapter_client    client;
    struct lights_thunk             *thunk;
    lights_adapter_done_t           completion;
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
    struct lights_adapter_context *context,
    struct lights_adapter_job const *job
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

    LIGHTS_DBG("Releasing adapter '%s'", context->name);

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

#define MSG_ACTION  (MSG_QUICK|MSG_BYTE|MSG_BYTE_DATA|MSG_WORD_DATA|MSG_BLOCK_DATA)

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
    struct lights_adapter_client const *client,
    struct lights_adapter_msg *msg
){
    union i2c_smbus_data data;
    s32 result = -EIO;

    switch (msg->flags & MSG_ACTION) {
        case MSG_BYTE:
            result = i2c_smbus_xfer(
                client->i2c_client.adapter,
                client->i2c_client.addr,
                client->i2c_client.flags,
                I2C_SMBUS_READ, 0,
                I2C_SMBUS_BYTE, &data
            );
            msg->data.byte = data.byte;
            msg->length = 1;
            break;
        case MSG_BYTE_DATA:
            result = i2c_smbus_xfer(
                client->i2c_client.adapter,
                client->i2c_client.addr,
                client->i2c_client.flags,
                I2C_SMBUS_READ, msg->command,
                I2C_SMBUS_BYTE_DATA, &data
            );
            msg->data.byte = data.byte;
            msg->length = 1;
            break;
        case MSG_WORD_DATA:
            result = i2c_smbus_xfer(
                client->i2c_client.adapter,
                client->i2c_client.addr,
                client->i2c_client.flags,
                I2C_SMBUS_READ, msg->command,
                I2C_SMBUS_WORD_DATA, &data
            );

            if (msg->flags & MSG_SWAPPED)
                msg->data.word = swab16(data.word);
            else
                msg->data.word = data.word;

            msg->length = 2;
            break;
        case MSG_BLOCK_DATA:
            result = i2c_smbus_xfer(
                client->i2c_client.adapter,
                client->i2c_client.addr,
                client->i2c_client.flags,
                I2C_SMBUS_READ, msg->command,
                I2C_SMBUS_BLOCK_DATA, &data
            );

        	if (!result) {
                memcpy(msg->data.block, &data.block[1], data.block[0]);
                msg->length = data.block[0];
            }
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
    struct lights_adapter_client const *client,
    struct lights_adapter_msg const *msg
){
    union i2c_smbus_data data;
    int result = -EIO;

    switch (msg->flags & MSG_ACTION) {
        case MSG_BYTE:
            result = i2c_smbus_xfer(
                client->i2c_client.adapter,
                client->i2c_client.addr,
                client->i2c_client.flags,
                I2C_SMBUS_WRITE, msg->data.byte,
                I2C_SMBUS_BYTE, NULL
            );
            break;
        case MSG_BYTE_DATA:
            data.byte = msg->data.byte;
            result = i2c_smbus_xfer(
                client->i2c_client.adapter,
                client->i2c_client.addr,
                client->i2c_client.flags,
                I2C_SMBUS_WRITE, msg->command,
                I2C_SMBUS_BYTE_DATA, &data
            );
            break;
        case MSG_WORD_DATA:
            if (msg->flags & MSG_SWAPPED)
                data.word = swab16(msg->data.word);
            else
                data.word = msg->data.word;

            result = i2c_smbus_xfer(
                client->i2c_client.adapter,
                client->i2c_client.addr,
                client->i2c_client.flags,
                I2C_SMBUS_WRITE, msg->command,
                I2C_SMBUS_WORD_DATA, &data
            );
            break;
        case MSG_BLOCK_DATA:
            if (msg->length > I2C_SMBUS_BLOCK_MAX)
                return -EINVAL;

            data.block[0] = msg->length;
            memcpy(&data.block[1], msg->data.block, msg->length);

            result = i2c_smbus_xfer(
                client->i2c_client.adapter,
                client->i2c_client.addr,
                client->i2c_client.flags,
                I2C_SMBUS_WRITE, msg->command,
                I2C_SMBUS_BLOCK_DATA, &data
            );
            break;
        default:
            return -EINVAL;
    }

    return result < 0 ? result : 0;
}

/**
 * lights_adapter_usb_read() - Processes a single write
 *
 * @client: Provided by the adapter caller
 * @msg:    Provided by the adapter caller
 *
 * @return: Zero or negative error number
 */
static error_t lights_adapter_usb_read (
    struct lights_adapter_client const *client,
    struct lights_adapter_msg *msg
){
    struct usb_packet pkt = {
        .length = msg->length,
        .data = msg->data.block
    };

    return usb_read_packet(&client->usb_client, &pkt);
}

/**
 * lights_adapter_usb_write() - Processes a single write
 *
 * @client: Provided by the adapter caller
 * @msg:    Provided by the adapter caller
 *
 * @return: Zero or negative error number
 */
static error_t lights_adapter_usb_write (
    struct lights_adapter_client const *client,
    struct lights_adapter_msg const *msg
){
    struct usb_packet pkt = {
        .length = msg->length,
        .data = (char*)msg->data.block
    };

    return usb_write_packet(&client->usb_client, &pkt);
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
    struct lights_adapter_job const *job
){
    struct lights_adapter_context *context;
    struct lights_adapter_job const *next;
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
    struct lights_adapter_context *context;
    struct lights_adapter_msg *msg;
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
            if (msg->flags & MSG_READ)
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
        job->completion(err ? msg : &job->msg, job->thunk, err);
    } else {
        job->completion(&job->msg, job->thunk, -ECANCELED);
    }

    if (count != lights_adapter_job_free(job) && !err)
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
    struct lights_adapter_client const *client,
    struct lights_adapter_msg *msgs,
    size_t count
){
    struct lights_adapter_context *context;
    struct lights_adapter_vtable const *vtable;
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
        if (msgs[i].flags & MSG_READ)
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
 * @thunk:     Second parameter of @callback
 * @callback:  Completion function
 *
 * @return: Zero or a negative error code
 */
error_t lights_adapter_xfer_async (
    struct lights_adapter_client const *client,
    struct lights_adapter_msg *msgs,
    size_t count,
    struct lights_thunk *thunk,
    lights_adapter_done_t callback
){
    struct lights_adapter_context *context;
    struct lights_adapter_job *job;
    error_t err = 0;

    if (IS_NULL(client, client->adapter, msgs) || IS_TRUE(0 == count) || IS_TRUE(count > LIGHTS_ADAPTER_MAX_MSGS))
        return -EINVAL;

    context = client->adapter;

    /* Create a linked list of jobs, one for each message */
    job = lights_adapter_job_create(context, count, msgs);
    job->client     = *client;
    job->completion = callback;
    job->thunk      = thunk;

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
    if (IS_NULL(client, client->adapter))
        return;

    if (client->proto == LIGHTS_PROTOCOL_USB)
        usb_controller_unregister(&client->usb_client);

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
    struct lights_adapter_vtable const *vtable;
    bool usb_registered = false;
    error_t err = 0;

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
                /*
                 * NOTE
                 * The clients onConnect handler is not able to make
                 * async calls until this function has returned.
                 */
                err = usb_controller_register(&client->usb_client);
                if (err)
                    goto error_free;

                usb_registered = true;
                context->name = client->usb_client.name;
                context->usb_controller = client->usb_client.controller;
                LIGHTS_DBG("Created USB adapter '%s'", context->name);
                break;
            default:
                LIGHTS_ERR("Unsupported protocol");
                err = -EINVAL;
                goto error_free;
        }

        err = lights_adapter_init(context);
        if (err) {
            LIGHTS_ERR("Failed to initialize adapter async: %d", err);
            goto error_free;
        }

        spin_lock(&lights_adapter_lock);
        list_add_tail(&context->siblings, &lights_adapter_list);
        spin_unlock(&lights_adapter_lock);
    }

    client->adapter = context;

    return err;

error_free:
    if (usb_registered)
        usb_controller_unregister(&client->usb_client);

    kfree(context);

    client->adapter = NULL;

    return err;
}
EXPORT_SYMBOL_NS_GPL(lights_adapter_register, LIGHTS);
