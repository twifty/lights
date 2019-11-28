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

#include <aura/debug.h>
// #include "usb-transfer.h"
#include "usb-driver.h"

/* table of devices that work with this driver */
static const struct usb_device_id usb_header_table[] = {
    { USB_DEVICE(0x0b05, 0x1867) },
    { USB_DEVICE(0x0b05, 0x1872) },

    { } /* Terminating entry */
};
// MODULE_DEVICE_TABLE(usb, usb_header_table);

static LIST_HEAD(usb_container_store_list);
static DEFINE_SPINLOCK(usb_container_store_lock);

/**
 * struct usb_container - Storage for context list
 *
 * @siblings:     Next and prev pointers
 * @context_list: Sorted list of context
 * @refs:         Reference count
 * @descriptor:   The actual properties of the device
 * @lock:         Spin lock for list
 */
struct usb_container {
    struct list_head                siblings;
    struct list_head                context_list;
    spinlock_t                      lock;
    struct kref                     refs;
    atomic_t                        controllers;

    struct usb_device_descriptor    descriptor;
    struct usb_class_driver         class_driver;
    struct usb_driver               usb_driver;

    wait_queue_head_t               probe_wait;
    size_t                          packet_size;
};

struct interrupt {
    struct usb_endpoint_descriptor  *endpoint;
    struct urb                      *urb;
    char                            *buffer;
    // size_t                          buffer_size;
    int                             interval;
    bool                            done;
};

struct usb_context {
    struct usb_container            *container;

    struct list_head                siblings;
    struct kref                     refs;
    struct usb_interface            *intf;
    struct usb_device               *udev;

    wait_queue_head_t               completion;
    atomic_t                        state;
    struct mutex                    lock;

    spinlock_t                      err_lock;
    error_t                         error;

    struct interrupt                interrupt_in;
    struct interrupt                interrupt_out;

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

    AURA_DBG("Destroying usb context for '%s'", context->name);

    mutex_destroy(&context->lock);
    usb_free_urb(context->interrupt_in.urb);
    usb_free_urb(context->interrupt_out.urb);
    kfree_const(context->name);
    kfree(context->interrupt_in.buffer);
    kfree(context->interrupt_out.buffer);
    kfree(context);
}

/**
 * usb_container_destroy() - Destructor
 *
 * @ref: Reference counter
 *
 * The container should have been removed from containing list prior to call.
 */
static void usb_container_destroy (
    struct kref *ref
){
    struct usb_container *container = container_of(ref, struct usb_container, refs);
    struct usb_context *iter, *safe;

    if (!list_empty(&container->context_list)) {
        AURA_ERR("Destroying a non-empty container");

        list_for_each_entry_safe(iter, safe, &container->context_list, siblings) {
            list_del(&iter->siblings);
            kref_put(&iter->refs, usb_context_destroy);
        }
    }

    kfree(container);
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
    const struct usb_container *container,
    const struct usb_device_id *id
){
    const struct usb_device_id *iter;

    for (
        iter = container->usb_driver.id_table;
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
 * usb_store_find_container_by_id() - Searches for a node matching the ID
 *
 * @id: Device ID
 *
 * @return: Null or a reference counted node instance
 */
static struct usb_container *usb_store_find_container_by_id (
    const struct usb_device_id *id
){
    struct usb_container *container;

    spin_lock(&usb_container_store_lock);

    list_for_each_entry(container, &usb_container_store_list, siblings) {
        if (usb_container_match(container, id)) {
            kref_get(&container->refs);
            goto found;
        }
    }

    container = NULL;

found:
    spin_unlock(&usb_container_store_lock);

    return container;
}

/**
 * usb_store_find_container_by_desciptor() - Searches for a node with the given desciptor
 *
 * @descriptor: Descriptor to search for
 *
 * @return: NULL or a reference counted node
 */
static struct usb_container *usb_store_find_container_by_name (
    const char *name
){
    struct usb_container *container;

    spin_lock(&usb_container_store_lock);

    list_for_each_entry(container, &usb_container_store_list, siblings) {
        if (strcmp(container->usb_driver.name, name) == 0) {
            kref_get(&container->refs);
            goto found;
        }
    }

    container = NULL;

found:
    spin_unlock(&usb_container_store_lock);

    return container;
}

/**
 * usb_store_create_container() - Creates a new node in the store
 *
 * @descriptor: Descriptor for the new node
 *
 * @return: Reference counted node or an Error Code
 */
static struct usb_container *usb_store_create_container (
    void
){
    struct usb_container *container;

    container = kzalloc(sizeof(*container), GFP_KERNEL);
    if (!container)
        return ERR_PTR(-ENOMEM);

    kref_init(&container->refs);
    INIT_LIST_HEAD(&container->context_list);
    spin_lock_init(&container->lock);
    atomic_set(&container->controllers, 0);

    spin_lock(&usb_container_store_lock);
    list_add_tail(&container->siblings, &usb_container_store_list);
    spin_unlock(&usb_container_store_lock);

    return container;
}

/**
 * usb_store_find_context() - Searches for a context
 *
 * @container: Container to search within
 * @index:     Zero based offset of multiple device contexts
 *
 * @return: NULL or a reference counted context instance
 */
static struct usb_context *usb_store_find_context (
    struct usb_container *container,
    uint8_t index
){
    struct usb_context *context;

    if (IS_NULL(container))
        return ERR_PTR(-EINVAL);

    spin_lock(&container->lock);

    context = list_first_entry(&container->context_list, struct usb_context, siblings);

    while (context && index--) {
        if (list_is_last(&context->siblings, &container->context_list))
            context = NULL;
        else
            context = list_next_entry(context, siblings);
    }

    if (context)
        kref_get(&context->refs);
    else
        context = ERR_PTR(-ENODEV);

    spin_unlock(&container->lock);

    return context;
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
    struct usb_container *container,
    struct usb_context *context
){
    struct usb_context *iter;

    spin_lock(&container->lock);

    if (!list_empty(&container->context_list)) {
        list_for_each_entry(iter, &container->context_list, siblings) {
            // TODO - Do we need to check for duplicates?
            if (strcmp(iter->name, context->name) > 0) {
                list_splice_tail(&context->siblings, &iter->siblings);
                goto exit;
            }
        }
    }

    kref_get(&container->refs);
    list_add_tail(&context->siblings, &container->context_list);

exit:
    context->container = container;
    kref_get(&container->refs);
    spin_unlock(&container->lock);
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
    struct usb_container *container = context->container;
    struct usb_context *iter;
    bool removed = false;

    if (!container)
        return false;

    spin_lock(&container->lock);

    list_for_each_entry(iter, &container->context_list, siblings) {
        if (iter == context) {
            list_del(&context->siblings);
            removed = true;
            goto exit;
        }
    }

exit:
    spin_unlock(&container->lock);

    if (removed) {
        kref_put(&container->refs, usb_container_destroy);
        context->container = NULL;
    }

    return removed;
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


static inline int urb_check_status (
    struct urb *urb
){
    struct usb_context *ctx = urb->context;
    int status = urb->status;
    unsigned long flags;

    /* sync/async unlink faults aren't errors */
    if (status) {
        if (!(status == -ENOENT || status == -ECONNRESET || status == -ESHUTDOWN)) {
            AURA_DBG("NonZero interrupt status: %d", status);
        }

        spin_lock_irqsave(&ctx->err_lock, flags);
        ctx->error = status;
        spin_unlock_irqrestore(&ctx->err_lock, flags);
    }

    return status;
}

static void usb_context_write_packet_callback (
    struct urb *urb
){
    struct usb_context *ctx = urb->context;
    urb_check_status(urb);

    ctx->interrupt_out.done = true;
    wake_up_interruptible(&ctx->completion);
}

static error_t usb_context_write_packet (
    struct usb_context *ctx,
    const struct usb_packet *packet
){
    /*
        Send a packet synchronously, return the error code
     */
    error_t err;

    /* Reset the completion flags */
    ctx->interrupt_out.done = false;

    /* Write data into interput_out */
    memcpy(ctx->interrupt_out.buffer, packet, PACKET_SIZE);

    /* Fill the urb (callback should set a completion flag) */
    usb_fill_int_urb(
        ctx->interrupt_out.urb,
        ctx->udev,
        usb_sndintpipe(ctx->udev, ctx->interrupt_out.endpoint->bEndpointAddress),
        ctx->interrupt_out.buffer,
        PACKET_SIZE,
        usb_context_write_packet_callback,
        ctx,
        ctx->interrupt_out.interval
    );

    /* Send the urb */
    err = usb_submit_urb(ctx->interrupt_out.urb, GFP_KERNEL);
    if (err) {
        AURA_ERR("Failed to submit OUT urb: %d", err);
        return err;
    }

    /* Wait for completion flag */
    err = ctrl_wait_event(ctx, ctx->interrupt_out.done);
    if (err) {
        AURA_ERR("Waiting for interrupt OUT error: %d", err);
        return err;
    }

    /* Remove arror from context */
    spin_lock_irq(&ctx->err_lock);
    err = ctx->error;
    if (err) {
        ctx->error = 0;
        err = (err == -EPIPE) ? err : -EIO;
    }
    spin_unlock_irq(&ctx->err_lock);

    return err;
}

static void usb_context_read_packet_callback (
    struct urb *urb
){
    struct usb_context *ctx = urb->context;
    urb_check_status(urb);

    ctx->interrupt_in.done = true;
    wake_up_interruptible(&ctx->completion);
}

static error_t usb_context_read_packet (
    struct usb_context *ctx,
    struct usb_packet *packet
){
    error_t err;

    ctx->interrupt_in.done = false;

    usb_fill_int_urb(
        ctx->interrupt_in.urb,
        ctx->udev,
        usb_rcvintpipe(ctx->udev, ctx->interrupt_in.endpoint->bEndpointAddress),
        ctx->interrupt_in.buffer,
        PACKET_SIZE,
        usb_context_read_packet_callback,
        ctx,
        ctx->interrupt_in.interval
    );

    /* Send the urb */
    err = usb_submit_urb(ctx->interrupt_in.urb, GFP_KERNEL);
    if (err) {
        AURA_ERR("Failed to submit IN urb: %d", err);
        return err;
    }

    /* Wait for completion flag */
    err = ctrl_wait_event(ctx, ctx->interrupt_in.done);
    if (err) {
        AURA_ERR("Waiting for interrupt IN error: %d", err);
        return err;
    }

    /* Remove arror from context */
    spin_lock_irq(&ctx->err_lock);
    err = ctx->error;
    if (err) {
        ctx->error = 0;
        err = (err == -EPIPE) ? err : -EIO;
    }
    spin_unlock_irq(&ctx->err_lock);

    if (!err)
        memcpy(packet, ctx->interrupt_in.buffer, PACKET_SIZE);

    return err;
}

static error_t usb_context_read_write (
    struct usb_context * context,
    const struct usb_packet * packet,
    bool do_read
){
    error_t err = 0;

    if (IS_NULL(context, packet))
        return -EINVAL;

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


static int usb_driver_register (
    struct usb_interface *intf,
    struct usb_container *container
){
    struct usb_device *udev = interface_to_usbdev(intf);
    struct usb_host_interface *iface_desc = intf->cur_altsetting;
    struct usb_context *context = NULL;
    int err;

    AURA_DBG("USB connecting: %s", dev_name(&udev->dev));

    /* allocate memory for our device state and initialize it */
    context = kzalloc(sizeof(*context), GFP_KERNEL);
    if (!context)
        return -ENOMEM;

    context->intf = intf;
    context->udev = udev;
    kref_init(&context->refs);
    atomic_set(&context->state, STATE_IDLE);
    context->packet_size = container->packet_size;
    context->name = kstrdup_const(dev_name(&udev->dev), GFP_KERNEL);

    /* Init the readers */
    mutex_init(&context->lock);
    init_waitqueue_head(&context->completion);

    /* Initialize the IN endpoint */
    err = usb_find_int_in_endpoint(iface_desc, &context->interrupt_in.endpoint);
    if (err) {
        AURA_ERR("Interrupt IN endpoint not found");
        goto error;
    }

    context->interrupt_in.interval = context->interrupt_in.endpoint->bInterval;
    context->interrupt_in.urb = usb_alloc_urb(0, GFP_KERNEL);
    if (!context->interrupt_in.urb) {
        err = -ENOMEM;
        goto error;
    }

    context->interrupt_in.buffer = kzalloc(container->packet_size, GFP_KERNEL);
    if (!context->interrupt_in.buffer) {
        err = -ENOMEM;
        goto error;
    }

    /* Initialize the OUT endpoint */
    err = usb_find_int_out_endpoint(iface_desc, &context->interrupt_out.endpoint);
    if (err) {
        AURA_ERR("Interrupt OUT endpoint not found");
        goto error;
    }

    context->interrupt_out.interval = context->interrupt_out.endpoint->bInterval;
    context->interrupt_out.urb = usb_alloc_urb(0, GFP_KERNEL);
    if (!context->interrupt_out.urb) {
        err = -ENOMEM;
        goto error;
    }

    context->interrupt_out.buffer = kzalloc(container->packet_size, GFP_KERNEL);
    if (!context->interrupt_out.buffer) {
        err = -ENOMEM;
        goto error;
    }

    /* we can register the device now, as it is ready */
    err = usb_register_dev(intf, &container->class_driver);
    if (err) {
        AURA_ERR("Failed to get a minor for this device.");
        goto error;
    }

    /* Context already has a ref count of 1 */
    usb_set_intfdata(intf, context);

    /* Increase the ref count of the container */
    usb_store_add_context(container, context);

    AURA_INFO("Registered USB controller for product 0x%04X", udev->descriptor.idProduct);

    return err;

error:
    kref_put(&context->refs, usb_context_destroy);

    return err;
}

static int usb_driver_connect (
    struct usb_interface *intf,
    const struct usb_device_id *id
){
    struct usb_container *container;
    int ret;

    /* Container is reference counted */
    container = usb_store_find_container_by_id(id);
    if (!container)
        return -ENODEV;

    ret = usb_driver_register(intf, container);

    kref_put(&container->refs, usb_container_destroy);

    return ret;
}

static void usb_driver_disconnect (
    struct usb_interface *intf
){
    /*
     * The interface holds a reference to the context as do any read/write
     * threads. Here we need to prevent any new reads/writes, cancel existing
     * transfers and release the interfaces hold of the context.
     */
    struct usb_context *context = usb_get_intfdata(intf);

    if (IS_NULL(context)) {
        AURA_ERR("Context not found in interface");
        return;
    }

    /* Prevent new handles from being created */
    usb_store_remove_context(context);

    /* Prevent any new readers writers */
    atomic_set(&context->state, STATE_EXITING);

    /* Cancel all existing transfers */
    usb_kill_urb(context->interrupt_in.urb);
    usb_kill_urb(context->interrupt_out.urb);

    /* There should be nothing waiting, but just in-case */
    wake_up_interruptible_all(&context->completion);

    /* Remove the interfaces hold */
    usb_set_intfdata(intf, NULL);
    kref_put(&context->refs, usb_context_destroy);
}

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
        AURA_ERR("Context not found in interface");
        return 0;
    }

    atomic_set(&context->state, STATE_PAUSED);

    usb_kill_urb(context->interrupt_in.urb);
    usb_kill_urb(context->interrupt_out.urb);

    AURA_INFO("USB suspended");

    return 0;
}

static int usb_driver_resume (
    struct usb_interface *intf
){
    struct usb_context *context = usb_get_intfdata(intf);
    struct usb_container *container;
    enum ctrl_state state;
    int ret;

    AURA_INFO("USB resuming");

    if (!context) {
        AURA_ERR("Context not found in interface");
        goto reset;
    }

    state = change_state(context, STATE_PAUSED, STATE_IDLE);
    if (STATE_PAUSED != state) {
        AURA_ERR("Expected a paused state, got: %d", state);
        goto reset;
    }

    return 0;

reset:
    container = context->container;
    kref_get(&container->refs);

    usb_driver_disconnect(intf);

    ret = usb_driver_register(intf, container);
    kref_put(&container->refs, usb_container_destroy);

    return ret;
}

static int usb_driver_pre_reset (
    struct usb_interface *intf
){
    AURA_DBG("__usb_pre_reset");
    return 0;
}

static int usb_driver_post_reset (
    struct usb_interface *intf
){
    AURA_DBG("__usb_post_reset");
    return 0;
}


error_t usb_read_packet (
    struct usb_controller *ctrl,
    struct usb_packet *packet
){
    struct usb_context *context;
    error_t err;

    if (IS_NULL(ctrl, packet))
        return -EINVAL;

    context = usb_store_find_context(ctrl->container, ctrl->index);
    if (IS_ERR(context))
        return PTR_ERR(context);

    err = usb_context_read_write(context, packet, true);
    kref_put(&context->refs, usb_context_destroy);

    return err;
}

error_t usb_write_packet (
    struct usb_controller *ctrl,
    const struct usb_packet *packet
){
    struct usb_context *context;
    error_t err;

    if (IS_NULL(ctrl, packet))
        return -EINVAL;

    context = usb_store_find_context(ctrl->container, ctrl->index);
    if (IS_ERR(context))
        return PTR_ERR(context);

    err = usb_context_read_write(context, packet, false);
    kref_put(&context->refs, usb_context_destroy);

    return err;
}


error_t usb_controller_register (
    struct usb_controller *ctrl
){
    const struct usb_device_id *id;
    struct usb_container *container;
    size_t name_len;
    error_t err;

    if (IS_NULL(ctrl, ctrl->ids, ctrl->name))
        return -EINVAL;

    if (ctrl->container) {
        AURA_ERR("USB controller is already registered");
        return -EEXIST;
    }

    container = usb_store_find_container_by_name(ctrl->name);
    if (container) {
        /* Already reference counted */
        atomic_inc(&container->controllers);
        ctrl->container = container;
        return 0;
    }

    /*
     * We need to keep track of if a device exists in the system. The first
     * call to register should delay until context is loaded. Calls to
     * read/write should only delay if a device is known to exist but no
     * context is available. So, each controller should hold a reference
     * to the container.
     *
     * Create a container populated with usb_driver and usb_class_driver.
     * Regsiter the driver and wait for a condition in the container. If
     * the wait doesn't timeout, create a reference to the container. Otherwise,
     * destroy the container (beware of probe trying to use a missing container).
     *
     * Can probe be called before disconnect?
     */
    name_len = strlen(ctrl->name);
    if (!name_len) {
        AURA_ERR("Empty strings not allowed");
        return -EINVAL;
    }

    container = usb_store_create_container();
    if (IS_ERR(container))
        return PTR_ERR(container);

    container->packet_size = ctrl->packet_size;

    container->usb_driver.name         = kstrdup_const(ctrl->name, GFP_KERNEL);
    container->usb_driver.probe        = usb_driver_connect;
    container->usb_driver.disconnect   = usb_driver_disconnect;
    container->usb_driver.suspend      = usb_driver_suspend;
    container->usb_driver.resume       = usb_driver_resume;
    container->usb_driver.reset_resume = usb_driver_resume;
    container->usb_driver.pre_reset    = usb_driver_pre_reset;
    container->usb_driver.post_reset   = usb_driver_post_reset;
    container->usb_driver.id_table     = ctrl->ids;
    container->class_driver.fops       = &usb_header_fops,
    container->class_driver.minor_base = 1,

    name_len += 4;
    container->class_driver.name = kzalloc(name_len, GFP_KERNEL);
    snprintf(container->class_driver.name, name_len, "%s-%%d", ctrl->name);

    /*
     * usb_driver_connect is passed an id. It needs to call usb_store_find_container_by_id
     * which should search the usb_driver.id_table
     *
     * When it has added a context
     */
    err = usb_register_driver(&header_driver, THIS_MODULE, "aura");
    if (err)
        goto error;

    if (!wait_event_interruptible_timeout(
        container->probe_wait,
        !list_empty(&container->context_list),
        5 * HZ
    )){
        err = -ETIMEDOUT;
        goto error;
    }

    /*
     * container should have 2 ref counts, first from this function and second
     * from adding the new context. Unregistering should need only decrease
     * the ref count.
     */
    atomic_inc(&container->controllers);
    ctrl->container = container;

    return 0;

error:
    /*
     * It's possible for the context to be added after the above timeout. If
     * this happens, we should destroy it.
     */
    if (err == -ETIMEDOUT)
        usb_deregister(&container->usb_driver);

    /* Remove the reference the controller would have held */
    kref_put(&container->refs, usb_container_destroy);

    return err;
}

void usb_controller_unregister (
    struct usb_controller * ctrl
){
    /*
     * Each controller holds a ref count of a container.
     * Each call to read/write creates a ref count to the context.
     *
     * usb_deregister() needs the usb_driver from the container. When called
     * it should invoke usb_driver_disconnect() for each context, which also
     * needs access to the container.
     *
     * If each context holds a reference to the container, calling usb_deregister()
     * should leave the container with only a single ref.
     *
     * Here we need to call usb_deregister() when, and only when, no more
     * controllers holds a reference.
     */

    if (IS_NULL(ctrl, ctrl->container))
        return -EINVAL;

    kref_put(&ctrl->container, usb_container_destroy);
    ctrl->container = NULL;

    if (atomic_dec_and_test(&container->controllers))
        usb_deregister(&container->usb_driver);
}
