// SPDX-License-Identifier: GPL-2.0
/*
 * A usb driver for the aura argb headers on asus motherboards.
 *
 * Copyright (C) 2019 Owen Parry
 *
 * Authors:
 * Owen Parry <twifty@zoho.com>
 */

#include <linux/usb.h>
#include <linux/module.h>
#include <linux/slab.h>

#include <adapter/debug.h>
#include "usb-driver.h"

#define dump_packet(msg, p, len) \
({                          \
    print_hex_dump_bytes(   \
        msg,                \
        DUMP_PREFIX_NONE,   \
        p,                  \
        len );      \
})

static const struct file_operations usb_fops = {
    .owner = THIS_MODULE,
};

static LIST_HEAD(usb_controller_store_list);
static DEFINE_SPINLOCK(usb_controller_store_lock);

enum usb_callback_type {
    CALLBACK_CONNECT     = 1,
    CALLBACK_DISCONNECT  = 2,
    CALLBACK_SUSPEND     = 4,
    CALLBACK_RESUME      = 8,

    CALLBACK_FIRST       = CALLBACK_CONNECT,
    CALLBACK_LAST        = CALLBACK_RESUME,
};

/**
 * struct usb_callback - Callback data
 *
 * @siblings:   Next and prev pointers
 * @queue_node: Temp Next and prev pointers
 * @refs:       Reference count
 * @client:     Owning client, also first arg to @func
 * @func:       Callback function
 * @type:       Type indicating where callback should be used
 */
struct usb_callback {
    struct list_head                siblings;
    struct list_head                queue_node;
    struct kref                     refs;
    struct usb_client               *client;
    usb_callback_t                  func;
    enum usb_callback_type          type;
};

/**
 * struct usb_container - Storage for context list
 *
 * @siblings:     Next and prev pointers
 * @context_list: Sorted list of context
 * @refs:         Reference count
 * @descriptor:   The actual properties of the device
 * @lock:         Spin lock for list
 */
struct usb_controller {
    struct list_head                siblings;
    struct list_head                context_list;
    struct list_head                callback_list;
    spinlock_t                      lock;
    struct kref                     refs;
    atomic_t                        controllers;

    struct usb_device_descriptor    descriptor;
    struct usb_class_driver         class_driver;
    struct usb_driver               usb_driver;

    wait_queue_head_t               probe_wait;
    size_t                          packet_size;
};

/**
 * struct interrupt - Input/Output data config
 *
 * @endpoint: Endpoint descriptor
 * @urb:      Associated URB for the endpoint
 * @buffer:   Buffer for the packet data
 * @interval: URB interval
 * @done:     Tranfer completion flag
 */
struct interrupt {
    struct usb_endpoint_descriptor  *endpoint;
    struct urb                      *urb;
    unsigned int                    pipe;
    int                             interval;
    uint8_t                         *buffer;
    bool                            done;
};

/**
 * struct usb_context - Single device
 *
 * @siblings:      Next and Prev pointers
 * @refs:          Reference count
 * @ctrl:          Owning container
 * @udev:          Owning usb device
 * @completion:    Threads waiting for read/write completion
 * @state:         Atomic state of the device
 * @lock:          Mutual exclusion lock for read and write
 * @interrupt_in:  Input config
 * @interrupt_out: Output config
 * @error:         Error seen during interrupt
 * @packet_size:   Max size of packet
 * @name:          Name of the driver
 */
struct usb_context {
    struct list_head                siblings;
    struct kref                     refs;

    struct usb_controller           *ctrl;
    struct usb_device               *udev;

    wait_queue_head_t               completion;
    atomic_t                        state;
    struct mutex                    lock;
    struct interrupt                interrupt_in;
    struct interrupt                interrupt_out;

    error_t                         error;
    size_t                          packet_size;
    const char                      *name;
};

/**
 * usb_context_destroy() - Destructor
 *
 * @ref: Reference counter instance
 */
static void usb_context_destroy (
    struct kref *ref
){
    struct usb_context *context = container_of(ref, struct usb_context, refs);

    LIGHTS_DBG("Destroying usb context for '%s'", context->name);

    mutex_destroy(&context->lock);
    usb_free_urb(context->interrupt_in.urb);
    usb_free_urb(context->interrupt_out.urb);
    kfree_const(context->name);
    kfree(context->interrupt_in.buffer);
    kfree(context->interrupt_out.buffer);
    kfree(context);
}

/**
 * usb_controller_destroy() - Destructor
 *
 * @ref: Reference counter
 *
 * The container should have been removed from containing list prior to call.
 */
static void usb_controller_destroy (
    struct kref *ref
){
    struct usb_controller *ctrl = container_of(ref, struct usb_controller, refs);
    struct usb_callback *cb_iter, *cb_safe;
    struct usb_context *ctx_iter, *ctx_safe;

    LIGHTS_DBG("Destroying USB controller '%s'", ctrl->usb_driver.name);

    if (!list_empty(&ctrl->context_list)) {
        LIGHTS_ERR("Destroying a non-empty controller");

        list_for_each_entry_safe(ctx_iter, ctx_safe, &ctrl->context_list, siblings) {
            list_del(&ctx_iter->siblings);
            kref_put(&ctx_iter->refs, usb_context_destroy);
        }
    }

    list_for_each_entry_safe(cb_iter, cb_safe, &ctrl->callback_list, siblings) {
        list_del(&cb_iter->siblings);
        kfree(cb_iter);
    }

    kfree(ctrl);
}

/**
 * usb_container_node_match() - Checks if a device id matches the node
 *
 * @node: Node to test
 * @id:   Device ID to match
 *
 * @return: True or False
 */
static inline bool usb_container_match (
    const struct usb_controller *ctrl,
    const struct usb_device_id *id
){
    const struct usb_device_id *iter;

    for (
        iter = ctrl->usb_driver.id_table;
        iter->idVendor || iter->idProduct || iter->bDeviceClass || iter->bInterfaceClass || iter->driver_info;
        iter++
    ){
        /* Do we always get the same object instance? */
        if (iter == id)
            return true;
    }

    return false;

    // if ((id->match_flags & USB_DEVICE_ID_MATCH_VENDOR) &&
    //     id->idVendor != le16_to_cpu(container->descriptor.idVendor))
    //         return false;
    //
    // if ((id->match_flags & USB_DEVICE_ID_MATCH_PRODUCT) &&
    //     id->idProduct != le16_to_cpu(container->descriptor.idProduct))
    //         return false;
    //
    // if ((id->match_flags & USB_DEVICE_ID_MATCH_DEV_LO) &&
    //     (id->bcdDevice_lo > le16_to_cpu(container->descriptor.bcdDevice)))
    //         return false;
    //
    // if ((id->match_flags & USB_DEVICE_ID_MATCH_DEV_HI) &&
    //     (id->bcdDevice_hi < le16_to_cpu(container->descriptor.bcdDevice)))
    //         return false;
    //
    // if ((id->match_flags & USB_DEVICE_ID_MATCH_DEV_CLASS) &&
    //     (id->bDeviceClass != container->descriptor.bDeviceClass))
    //         return false;
    //
    // if ((id->match_flags & USB_DEVICE_ID_MATCH_DEV_SUBCLASS) &&
    //     (id->bDeviceSubClass != container->descriptor.bDeviceSubClass))
    //         return false;
    //
    // if ((id->match_flags & USB_DEVICE_ID_MATCH_DEV_PROTOCOL) &&
    //     (id->bDeviceProtocol != container->descriptor.bDeviceProtocol))
    //         return false;
    //
    // return true;
}

/**
 * usb_store_find_controller_by_id() - Searches for a node matching the ID
 *
 * @id: Device ID
 *
 * @return: Null or a reference counted node instance
 */
static struct usb_controller *usb_store_find_controller_by_id (
    const struct usb_device_id *id
){
    struct usb_controller *ctrl;

    spin_lock(&usb_controller_store_lock);

    list_for_each_entry(ctrl, &usb_controller_store_list, siblings) {
        if (usb_container_match(ctrl, id)) {
            kref_get(&ctrl->refs);
            goto found;
        }
    }

    ctrl = NULL;

found:
    spin_unlock(&usb_controller_store_lock);

    return ctrl;
}

/**
 * usb_store_find_container_by_desciptor() - Searches for a node with the given desciptor
 *
 * @descriptor: Descriptor to search for
 *
 * @return: NULL or a reference counted node
 */
static struct usb_controller *usb_store_find_controller_by_name (
    const char *name
){
    struct usb_controller *ctrl;

    spin_lock(&usb_controller_store_lock);

    list_for_each_entry(ctrl, &usb_controller_store_list, siblings) {
        if (strcmp(ctrl->usb_driver.name, name) == 0) {
            kref_get(&ctrl->refs);
            goto found;
        }
    }

    ctrl = NULL;

found:
    spin_unlock(&usb_controller_store_lock);

    return ctrl;
}

/**
 * usb_controller_find_context() - Searches for a context
 *
 * @container: Container to search within
 * @index:     Zero based offset of multiple device contexts
 *
 * @return: NULL or a reference counted context instance
 */
static struct usb_context *usb_controller_find_context (
    struct usb_controller *ctrl,
    uint8_t index
){
    struct usb_context *context;

    if (IS_NULL(ctrl))
        return ERR_PTR(-EINVAL);

    spin_lock(&ctrl->lock);

    context = list_first_entry_or_null(&ctrl->context_list, struct usb_context, siblings);

    while (context && index--) {
        if (list_is_last(&context->siblings, &ctrl->context_list))
            context = NULL;
        else
            context = list_next_entry(context, siblings);
    }

    if (context)
        kref_get(&context->refs);
    else
        context = ERR_PTR(-ENODEV);

    spin_unlock(&ctrl->lock);

    return context;
}

/**
 * usb_store_find_context() - Finds a context usable by the client
 *
 * @client: Client to search for
 *
 * @return: Context or an error code
 */
static struct usb_context *usb_store_find_context (
    const struct usb_client *client
){
    struct usb_controller *ctrl;

    if (IS_NULL(client, client->name))
        return ERR_PTR(-EINVAL);

    ctrl = client->controller;
    if (!ctrl)
        ctrl = usb_store_find_controller_by_name(client->name);

    if (ctrl)
        return usb_controller_find_context(ctrl, client->index);

    return ERR_PTR(-ENODEV);
}

/**
 * usb_store_add_context() - Adds the context to a new or existing node
 *
 * @container: Owning container
 * @context:   Context to add
 *
 * The container property of @context will be configured and the reference
 * counter of @container will be increased.
 */
static void usb_store_add_context (
    struct usb_controller *ctrl,
    struct usb_context *context
){
    struct usb_context *iter;

    spin_lock(&ctrl->lock);

    if (!list_empty(&ctrl->context_list)) {
        list_for_each_entry(iter, &ctrl->context_list, siblings) {
            // TODO - Do we need to check for duplicates?
            if (strcmp(iter->name, context->name) > 0) {
                list_splice_tail(&context->siblings, &iter->siblings);
                goto exit;
            }
        }
    }

    kref_get(&ctrl->refs);
    list_add_tail(&context->siblings, &ctrl->context_list);

exit:
    context->ctrl = ctrl;
    spin_unlock(&ctrl->lock);

    LIGHTS_DBG("Added context to controller");
}

/**
 * usb_store_remove_context() - Removes a context from the store
 *
 * @context: Context to remove
 *
 * @return: True or False
 *
 * NOTE: The context is not destroyed, but the containing node may be.
 */
static bool usb_store_remove_context (
    struct usb_context *context
){
    struct usb_controller *ctrl = context->ctrl;
    struct usb_context *iter;
    bool removed = false;

    if (!ctrl) {
        LIGHTS_ERR("Context not configured with controller");
        return false;
    }

    spin_lock(&ctrl->lock);

    list_for_each_entry(iter, &ctrl->context_list, siblings) {
        if (iter == context) {
            list_del(&context->siblings);
            removed = true;
            goto exit;
        }
    }

exit:
    spin_unlock(&ctrl->lock);

    if (removed) {
        LIGHTS_DBG("Removed context from controller");

        kref_put(&ctrl->refs, usb_controller_destroy);
        context->ctrl = NULL;
    } else {
        LIGHTS_ERR("Context not found in controller");
    }

    return removed;
}

/**
 * usb_controller_callback_create() - Adds all the callbacks for the client
 *
 * @ctrl:   Target to store the callbacks
 * @client: Client to add
 *
 * @return: Bitmask of added callback types
 */
static int usb_controller_callback_create (
    struct usb_controller *ctrl,
    struct usb_client *client
){
    LIST_HEAD(callbacks);
    usb_callback_t func = NULL;
    struct usb_callback *cb, *safe;
    uint8_t mask = 0, type;

    for (type = CALLBACK_FIRST; type <= CALLBACK_LAST; type <<= 1) {
        switch (type) {
            case CALLBACK_CONNECT:
                func = client->onConnect;
                break;
            case CALLBACK_DISCONNECT:
                func = client->onDisconnect;
                break;
            case CALLBACK_SUSPEND:
                func = client->onSuspend;
                break;
            case CALLBACK_RESUME:
                func = client->onResume;
                break;
        }

        if (func) {
            cb = kzalloc(sizeof(*cb), GFP_KERNEL);
            if (!cb)
                goto error;

            kref_init(&cb->refs);
            cb->type = type;
            cb->func = func;

            mask |= type;
            list_add_tail(&cb->siblings, &callbacks);
        }
    }

    if (!list_empty(&callbacks)) {
        spin_lock(&ctrl->lock);
        list_splice_tail(&callbacks, &ctrl->callback_list);
        spin_unlock(&ctrl->lock);
    }

    return mask;

error:
    list_for_each_entry_safe(cb, safe, &callbacks, siblings) {
        list_del(&cb->siblings);
        kfree(cb);
    }

    return -ENOMEM;
}

/**
 * usb_controller_callback_destroy() - Destroys a single callback
 *
 * @ref: Reference count
 *
 * Instance is expected to be removed from any lists before calling.
 */
static void usb_controller_callback_destroy (
    struct kref *ref
){
    struct usb_callback *callback = container_of(ref, struct usb_callback, refs);

    kfree(callback);
}

/**
 * usb_controller_callback_remove() - Destroys all callbacks for the client
 *
 * @ctrl:   Owning container
 * @client: Client to remove
 */
static void usb_controller_callback_remove (
    struct usb_controller *ctrl,
    struct usb_client *client
){
    LIST_HEAD(temp);
    struct usb_callback *iter, *safe;

    spin_lock(&ctrl->lock);
    list_for_each_entry_safe(iter, safe, &ctrl->callback_list, siblings) {
        if (iter->client == client)
            list_move_tail(&iter->siblings, &temp);
    }
    spin_unlock(&ctrl->lock);

    list_for_each_entry_safe(iter, safe, &temp, siblings) {
        list_del(&iter->siblings);
        kref_put(&iter->refs, usb_controller_callback_destroy);
    }
}

/**
 * usb_controller_callback_invoke() - Calls each callback of a given type
 *
 * @ctrl: Owning container
 * @type: Type of callback to call
 *
 * Each callback is added to a temporary list so that a spin lock doesn't
 * need to be held. Since all driver callbacks are mutually exlusive, we
 * don't need to worry about multiple calls.
 *
 * However, if in the future a read callback is added. That callback instance
 * cannot be grouped together with the existing methods.
 */
static void usb_controller_callback_invoke (
    struct usb_controller *ctrl,
    enum usb_callback_type type
){
    LIST_HEAD(queue);
    struct usb_callback *iter, *safe;

    if (IS_NULL(ctrl))
        return;

    /*
     * Since connect and disconnect are mutually exclusive, we don't need
     * to worry about contention on the queue_node parameter. However,
     * it is possible for a callback handler to sleep, which cannot be done
     * while holding a spinlock; or modify the callback list, which will
     * cause problems while terating.
     */

    spin_lock(&ctrl->lock);
    list_for_each_entry(iter, &ctrl->callback_list, siblings) {
        if (iter->type == type) {
            kref_get(&iter->refs);
            list_add_tail(&iter->queue_node, &queue);
        }
    }
    spin_unlock(&ctrl->lock);

    list_for_each_entry_safe(iter, safe, &queue, queue_node) {
        iter->func(iter->client);
        kref_put(&iter->refs, usb_controller_callback_destroy);
    }
}


enum ctrl_state {
    STATE_UNKNOWN = -1,
    STATE_IDLE    = 0,  /* Allow either reader or writer */
    STATE_WRITING = 1,  /* Block any readers */
    STATE_READING = 2,  /* Block any writers */
    STATE_PAUSED  = 3,  /* Block both readers and writers */
    STATE_EXITING = 4,  /* Prevent any more readers and writers */
};

static inline enum ctrl_state change_state (
    struct usb_context *ctx,
    enum ctrl_state from,
    enum ctrl_state to
){
    return atomic_cmpxchg(&ctx->state, from, to);
}

static inline enum ctrl_state read_state (
    struct usb_context *ctx
){
    return atomic_read(&ctx->state);
}


#define ctrl_wait_event(ctx, event) \
({                                                      \
    error_t __err = 0;                                  \
    long __timeout = wait_event_interruptible_timeout(  \
        (ctx)->completion, event, 5 * HZ                \
    );                                                  \
    if (__timeout == 0)                                 \
        __err = -ETIMEDOUT;                             \
    __err;                                              \
})

/**
 * urb_check_status() - Reads any URB error and adds it to the context
 *
 * @urb: URB being processed
 *
 * @return: Status of the URB
 */
static inline int urb_check_status (
    struct urb *urb
){
    struct usb_context *context = urb->context;
    int status = urb->status;

    /* sync/async unlink faults aren't errors */
    if (status) {
        if (!(status == -ENOENT || status == -ECONNRESET || status == -ESHUTDOWN)) {
            LIGHTS_DBG("NonZero interrupt status: %d", status);
        }

        context->error = status;
    }

    return status;
}

/**
 * usb_context_write_packet_callback() - Write completion handler
 *
 * @urb: URB being processed
 *
 * @context: interrupt
 */
static void usb_context_write_packet_callback (
    struct urb *urb
){
    struct usb_context *context = urb->context;
    urb_check_status(urb);

    context->interrupt_out.done = true;
    wake_up_interruptible(&context->completion);
}

/**
 * usb_context_write_packet() - Sends a write URB to the device
 *
 * @context: Owning context
 * @packet:  Data to send
 *
 * @return: Error code
 */
static error_t usb_context_write_packet (
    struct usb_context *context,
    const struct usb_packet *packet
){
    size_t packet_size = context->packet_size;
    error_t err;

    if (packet->length < packet_size)
        packet_size = packet->length;

    /* Reset the completion flags */
    context->interrupt_out.done = false;

    /* Write data into interput_out */
    memcpy(context->interrupt_out.buffer, packet->data, packet_size);

    /* Fill the urb (callback should set a completion flag) */
    usb_fill_int_urb(
        context->interrupt_out.urb,
        context->udev,
        context->interrupt_out.pipe,
        context->interrupt_out.buffer,
        packet_size,
        usb_context_write_packet_callback,
        context,
        context->interrupt_out.interval
    );

    // dump_packet("Sending Packet:", ctx->interrupt_out.buffer, packet_size);

    /* Send the urb */
    err = usb_submit_urb(context->interrupt_out.urb, GFP_KERNEL);
    if (err) {
        LIGHTS_ERR("Failed to submit OUT urb: %d", err);
        return err;
    }

    /* Wait for completion flag */
    err = ctrl_wait_event(context, context->interrupt_out.done);
    if (err) {
        LIGHTS_ERR("Waiting for interrupt OUT error: %d", err);
        return err;
    }

    /* Remove arror from context */
    if (context->error) {
        context->error = 0;
        err = (err == -EPIPE) ? err : -EIO;
    }

    return err;
}

/**
 * usb_context_read_packet_callback() - Read completion handler
 *
 * @urb: URB being processed
 *
 * @context: interrupt
 */
static void usb_context_read_packet_callback (
    struct urb *urb
){
    struct usb_context *context = urb->context;

    LIGHTS_DBG("Processing read callback");
    urb_check_status(urb);

    context->interrupt_in.done = true;
    wake_up_interruptible(&context->completion);
}

/**
 * usb_context_read_packet() - Reads data from the device
 *
 * @context: Owning context
 * @packet:  Buffer to populate
 *
 * @return: Error code
 */
static error_t usb_context_read_packet (
    struct usb_context *context,
    struct usb_packet *packet
){
    error_t err;
    size_t packet_size = context->packet_size;

    if (packet->length < packet_size)
        packet_size = packet->length;

    context->interrupt_in.done = false;

    usb_fill_int_urb(
        context->interrupt_in.urb,
        context->udev,
        context->interrupt_in.pipe,
        context->interrupt_in.buffer,
        packet_size,
        usb_context_read_packet_callback,
        context,
        context->interrupt_in.interval
    );

    /* Send the urb */
    err = usb_submit_urb(context->interrupt_in.urb, GFP_KERNEL);
    if (err) {
        LIGHTS_ERR("Failed to submit IN urb: %d", err);
        return err;
    }

    /* Wait for completion flag */
    err = ctrl_wait_event(context, context->interrupt_in.done);
    if (err) {
        LIGHTS_ERR("Waiting for interrupt IN error: %d", err);
        return err;
    }

    /* Remove arror from context */
    if (context->error) {
        context->error = 0;
        err = (err == -EPIPE) ? err : -EIO;
    }

    if (!err)
        memcpy(packet->data, context->interrupt_in.buffer, packet_size);

    return err;
}

/**
 * usb_context_read_write() - Writes and Reads the device
 *
 * @context: Owning context
 * @packet:  Input/Output packet
 * @do_read: Flag to indicate if a read should be performed after the write
 *
 * @return: Error code
 */
static error_t usb_context_read_write (
    struct usb_context *context,
    const struct usb_packet *packet,
    bool do_read
){
    error_t err = 0;

    if (IS_NULL(context, packet))
        return -EINVAL;

    if (packet->length > context->packet_size)
        return -E2BIG;

    /* Allow only one thread at a time */
    err = mutex_lock_interruptible(&context->lock);
    if (err)
        goto error_out;

    if (STATE_IDLE != read_state(context))
        goto error_out;

    /* Send the packet */
    err = usb_context_write_packet(context, packet);

    /* Read a response */
    if (!err && do_read)
        err = usb_context_read_packet(context, (void*)packet);

error_out:
    mutex_unlock(&context->lock);

    return err;
}


/**
 * usb_driver_register() - Creates a context for a device
 *
 * @intf: Device interface
 * @ctrl: Container to store the context
 *
 * @return: Error code
 */
static int usb_driver_register (
    struct usb_interface *intf,
    struct usb_controller *ctrl
){
    struct usb_device *udev = interface_to_usbdev(intf);
    struct usb_host_interface *iface_desc = intf->cur_altsetting;
    struct usb_context *context = NULL;
    int err;

    LIGHTS_DBG("USB connecting: %s", dev_name(&udev->dev));

    /* allocate memory for our device state and initialize it */
    context = kzalloc(sizeof(*context), GFP_KERNEL);
    if (!context)
        return -ENOMEM;

    kref_init(&context->refs);
    atomic_set(&context->state, STATE_IDLE);
    context->udev = udev;
    context->packet_size = ctrl->packet_size;
    context->name = kstrdup_const(dev_name(&udev->dev), GFP_KERNEL);

    /* Init the readers */
    mutex_init(&context->lock);
    init_waitqueue_head(&context->completion);

    /* Initialize the IN endpoint */
    err = usb_find_int_in_endpoint(iface_desc, &context->interrupt_in.endpoint);
    if (err) {
        LIGHTS_ERR("Interrupt IN endpoint not found");
        goto error;
    }

    context->interrupt_in.pipe = usb_rcvintpipe(udev, context->interrupt_in.endpoint->bEndpointAddress);
    context->interrupt_in.interval = context->interrupt_in.endpoint->bInterval;
    context->interrupt_in.urb = usb_alloc_urb(0, GFP_KERNEL);
    context->interrupt_in.buffer = kzalloc(ctrl->packet_size, GFP_KERNEL);
    if (!(context->interrupt_in.urb && context->interrupt_in.buffer)) {
        err = -ENOMEM;
        goto error;
    }

    /* Initialize the OUT endpoint */
    err = usb_find_int_out_endpoint(iface_desc, &context->interrupt_out.endpoint);
    if (err) {
        LIGHTS_ERR("Interrupt OUT endpoint not found");
        goto error;
    }

    context->interrupt_out.pipe = usb_sndintpipe(udev, context->interrupt_out.endpoint->bEndpointAddress);
    context->interrupt_out.interval = context->interrupt_out.endpoint->bInterval;
    context->interrupt_out.urb = usb_alloc_urb(0, GFP_KERNEL);
    context->interrupt_out.buffer = kzalloc(ctrl->packet_size, GFP_KERNEL);
    if (!(context->interrupt_out.urb && context->interrupt_out.buffer)) {
        err = -ENOMEM;
        goto error;
    }

    /* we can register the device now, as it is ready */
    err = usb_register_dev(intf, &ctrl->class_driver);
    if (err) {
        LIGHTS_ERR("Failed to get a minor for this device.");
        goto error;
    }

    /* Context already has a ref count of 1 */
    usb_set_intfdata(intf, context);

    /* Increase the ref count of the ctrl */
    usb_store_add_context(ctrl, context);

    LIGHTS_INFO("Registered USB controller for product 0x%04X", udev->descriptor.idProduct);

    /* Wake any threads waiting in usb_controller_register() */
    wake_up_interruptible_all(&ctrl->probe_wait);

    /* Call all registered callbacks */
    usb_controller_callback_invoke(ctrl, CALLBACK_CONNECT);

    return err;

error:
    kref_put(&context->refs, usb_context_destroy);

    return err;
}

/**
 * usb_driver_connect() - Probe handler
 *
 * @intf: Device interface
 * @id:   Device ID used on driver registration
 *
 * @return: Error code
 */
static int usb_driver_connect (
    struct usb_interface *intf,
    const struct usb_device_id *id
){
    struct usb_controller *ctrl;
    int ret;

    /* Container is reference counted */
    ctrl = usb_store_find_controller_by_id(id);
    if (!ctrl)
        return -ENODEV;

    ret = usb_driver_register(intf, ctrl);

    kref_put(&ctrl->refs, usb_controller_destroy);

    return ret;
}

/**
 * usb_driver_disconnect() - Disconnect handler
 *
 * @intf: Device interface
 */
static void usb_driver_disconnect (
    struct usb_interface *intf
){
    struct usb_context *context = usb_get_intfdata(intf);
    struct usb_controller *ctrl;

    if (IS_NULL(context, context->ctrl)) {
        LIGHTS_ERR("Context not found in interface");
        return;
    }

    LIGHTS_INFO("USB disconnecting '%s'", context->name);

    /* Prevent new handles from being created */
    ctrl = context->ctrl;
    kref_get(&ctrl->refs);
    usb_store_remove_context(context);

    /* Prevent any new readers writers */
    atomic_set(&context->state, STATE_EXITING);

    /* Cancel all existing transfers */
    usb_kill_urb(context->interrupt_in.urb);
    usb_kill_urb(context->interrupt_out.urb);

    /* There should be nothing waiting, but just in-case */
    wake_up_interruptible_all(&context->completion);

    /* Call all registered callbacks */
    usb_controller_callback_invoke(ctrl, CALLBACK_DISCONNECT);

    /* Give back the minor */
    usb_deregister_dev(intf, &ctrl->class_driver);

    /* Remove the interfaces hold */
    usb_set_intfdata(intf, NULL);
    kref_put(&ctrl->refs, usb_controller_destroy);
    kref_put(&context->refs, usb_context_destroy);
}

/**
 * usb_driver_suspend() - Suspend handler
 *
 * @intf:    Device interface
 * @message: Unused
 *
 * @return: Error code
 */
static int usb_driver_suspend (
    struct usb_interface *intf,
    pm_message_t message
){
    /*
        Pause all readers and writers to prevent any more packets
        from being sent. If suspend was called mid-transfer, then
        the completion callback for that urb should see an error
        and invoke the transfer complete function.
     */
    struct usb_context *context = usb_get_intfdata(intf);

    if (!context) {
        LIGHTS_ERR("Context not found in interface");
        return 0;
    }

    LIGHTS_INFO("USB suspending '%s'", context->name);

    atomic_set(&context->state, STATE_PAUSED);

    usb_kill_urb(context->interrupt_in.urb);
    usb_kill_urb(context->interrupt_out.urb);

    /* Call all registered callbacks */
    usb_controller_callback_invoke(context->ctrl, CALLBACK_SUSPEND);

    return 0;
}

/**
 * usb_driver_resume() - Resume handler
 *
 * @intf: Device interface
 *
 * @return: Error code
 */
static int usb_driver_resume (
    struct usb_interface *intf
){
    struct usb_context *context = usb_get_intfdata(intf);
    struct usb_controller *ctrl;
    enum ctrl_state state;
    int ret;

    if (!context) {
        LIGHTS_ERR("Context not found in interface");
        goto reset;
    }

    state = change_state(context, STATE_PAUSED, STATE_IDLE);
    if (STATE_PAUSED != state) {
        LIGHTS_ERR("Expected a paused state, got: %d", state);
        goto reset;
    }

    LIGHTS_INFO("USB resuming '%s'", context->name);

    /* Call all registered callbacks */
    usb_controller_callback_invoke(context->ctrl, CALLBACK_RESUME);

    return 0;

reset:
    ctrl = context->ctrl;
    kref_get(&ctrl->refs);

    LIGHTS_INFO("USB resetting '%s'", context->name);

    usb_driver_disconnect(intf);

    ret = usb_driver_register(intf, ctrl);
    kref_put(&ctrl->refs, usb_controller_destroy);

    return ret;
}

/**
 * usb_driver_pre_reset() - Pre reset handler
 *
 * @intf: Device interface
 *
 * @return: Error code
 */
static int usb_driver_pre_reset (
    struct usb_interface *intf
){
    LIGHTS_DBG("usb_driver_pre_reset()");
    return 0;
}

/**
 * usb_driver_post_reset() - Post reset handler
 *
 * @intf: Device interface
 *
 * @return: Error code
 */
static int usb_driver_post_reset (
    struct usb_interface *intf
){
    LIGHTS_DBG("usb_driver_post_reset()");
    return 0;
}


/**
 * usb_read_packet() - Reads a packet from a device
 *
 * @client: Previously registered client
 * @packet: Data to send and buffer to recieve
 *
 * @return: Error code
 */
error_t usb_read_packet (
    const struct usb_client *client,
    struct usb_packet *packet
){
    struct usb_context *context;
    error_t err = -EIO;

    if (IS_NULL(client, packet))
        return -EINVAL;

    context = usb_store_find_context(client);
    if (IS_ERR(context))
        return PTR_ERR(context);

    err = usb_context_read_write(context, packet, true);
    kref_put(&context->refs, usb_context_destroy);

    return err;
}

/**
 * usb_write_packet() - Writes a packet to a device
 *
 * @client: Previously registered client
 * @packet: Data to send
 *
 * @return: Error code
 */
error_t usb_write_packet (
    const struct usb_client *client,
    const struct usb_packet *packet
){
    struct usb_context *context;
    error_t err;

    if (IS_NULL(client, packet))
        return -EINVAL;

    context = usb_store_find_context(client);
    if (IS_ERR(context))
        return PTR_ERR(context);

    err = usb_context_read_write(context, packet, false);
    kref_put(&context->refs, usb_context_destroy);

    return err;
}


/**
 * usb_store_create_controller() - Creates a new node in the store
 *
 * @descriptor: Descriptor for the new node
 *
 * @return: Reference counted node or an Error Code
 */
static struct usb_controller *usb_store_create_controller (
    const struct usb_client *client
){
    struct usb_controller *ctrl, *iter;
    size_t name_len;

    name_len = strlen(client->name);
    if (!name_len) {
        LIGHTS_ERR("Empty strings not allowed");
        return ERR_PTR(-EINVAL);
    }

    ctrl = kzalloc(sizeof(*ctrl), GFP_KERNEL);
    if (!ctrl)
        return ERR_PTR(-ENOMEM);

    kref_init(&ctrl->refs);
    INIT_LIST_HEAD(&ctrl->context_list);
    INIT_LIST_HEAD(&ctrl->callback_list);
    spin_lock_init(&ctrl->lock);
    atomic_set(&ctrl->controllers, 1);
    init_waitqueue_head(&ctrl->probe_wait);

    ctrl->packet_size = client->packet_size;

    ctrl->usb_driver.name         = kstrdup_const(client->name, GFP_KERNEL);
    ctrl->usb_driver.probe        = usb_driver_connect;
    ctrl->usb_driver.disconnect   = usb_driver_disconnect;
    ctrl->usb_driver.suspend      = usb_driver_suspend;
    ctrl->usb_driver.resume       = usb_driver_resume;
    ctrl->usb_driver.reset_resume = usb_driver_resume;
    ctrl->usb_driver.pre_reset    = usb_driver_pre_reset;
    ctrl->usb_driver.post_reset   = usb_driver_post_reset;
    ctrl->usb_driver.id_table     = client->ids;
    ctrl->class_driver.fops       = &usb_fops,
    ctrl->class_driver.minor_base = 1,

    name_len += 4;
    ctrl->class_driver.name = kzalloc(name_len, GFP_KERNEL);
    snprintf(ctrl->class_driver.name, name_len, "%s-%%d", client->name);

    spin_lock(&usb_controller_store_lock);

    /* Edge case where function was called twice */
    list_for_each_entry(iter, &usb_controller_store_list, siblings) {
        if (strcmp(iter->usb_driver.name, ctrl->usb_driver.name) == 0) {
            kref_get(&iter->refs);
            goto exists;
        }
    }

    list_add_tail(&ctrl->siblings, &usb_controller_store_list);
    spin_unlock(&usb_controller_store_lock);

    LIGHTS_DBG("Created USB controller for '%s'", client->name);

    return ctrl;

exists:
    spin_unlock(&usb_controller_store_lock);

    kref_put(&ctrl->refs, usb_controller_destroy);

    LIGHTS_DBG("Using existing USB controller for '%s'", client->name);

    return iter;
}

/**
 * usb_controller_register() - Creates a driver and/or associates a client
 *
 * @client: Contains driver ids and callbacks
 *
 * @return: Error code
 */
error_t usb_controller_register (
    struct usb_client *client
){
    struct usb_controller *ctrl;
    int callbacks;
    error_t err;

    if (IS_NULL(client, client->ids, client->name))
        return -EINVAL;

    if (client->controller) {
        LIGHTS_ERR("USB controller is already registered");
        return -EEXIST;
    }

    ctrl = usb_store_find_controller_by_name(client->name);
    if (ctrl) {
        LIGHTS_DBG("Using previously registered driver for '%s'", client->name);
        /* Already reference counted */
        atomic_inc(&ctrl->controllers);
        client->controller = ctrl;
        return 0;
    }

    /* Initialized with a ref count of 1, and ctrl count of 1 */
    ctrl = usb_store_create_controller(client);
    if (IS_ERR(ctrl))
        return PTR_ERR(ctrl);

    callbacks = usb_controller_callback_create(ctrl, client);
    if (callbacks < 0) {
        err = callbacks;
        goto error;
    }

    LIGHTS_DBG("Registering driver for '%s'", client->name);

    err = usb_register_driver(&ctrl->usb_driver, THIS_MODULE, client->name);
    if (err)
        goto error;

    if ((callbacks & CALLBACK_CONNECT) == 0) {
        if (!wait_event_interruptible_timeout(
            ctrl->probe_wait,
            !list_empty(&ctrl->context_list),
            5 * HZ
        )){
            err = -ETIMEDOUT;
            goto error;
        }
    }

    /* Already ref counted */
    // atomic_inc(&ctrl->controllers);
    client->controller = ctrl;

    return 0;

error:
    /*
     * It's possible for the context to be added after the above timeout. If
     * this happens, we should destroy it.
     */
    if (err == -ETIMEDOUT) {
        LIGHTS_DBG("Register driver timed out");
        usb_deregister(&ctrl->usb_driver);
    }

    /* Remove the reference the controller would have held */
    kref_put(&ctrl->refs, usb_controller_destroy);

    return err;
}

/**
 * usb_controller_unregister() - Destroys a driver and/or dissassociates a client
 *
 * @client: Previously registered client
 */
void usb_controller_unregister (
    struct usb_client *client
){
    struct usb_controller *ctrl;

    if (IS_NULL(client, client->controller)) {
        LIGHTS_ERR("Cannot unregister a null pointer");
        return;
    }

    ctrl = client->controller;

    usb_controller_callback_remove(ctrl, client);

    if (atomic_dec_and_test(&ctrl->controllers))
        usb_deregister(&ctrl->usb_driver);

    kref_put(&ctrl->refs, usb_controller_destroy);
    client->controller = NULL;
}
