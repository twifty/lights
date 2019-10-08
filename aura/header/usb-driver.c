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
#include "usb-transfer.h"
#include "usb-driver.h"

/* table of devices that work with this driver */
static const struct usb_device_id usb_header_table[] = {
    { USB_DEVICE(0x0b05, 0x1867) },
    { USB_DEVICE(0x0b05, 0x1872) },

    { } /* Terminating entry */
};
MODULE_DEVICE_TABLE(usb, usb_header_table);

static const struct file_operations usb_header_fops = {
    .owner = THIS_MODULE,
};

static struct usb_class_driver usb_header_class = {
    .name = "mb-argb-header-%d",
    .fops = &usb_header_fops,
    .minor_base = 1,
};

enum {
    // PACKET_SIZE      = 0x41,
    FIVE_SECONDS     = 5 * HZ,
};

enum ctrl_state {
    STATE_UNKNOWN = -1,
    STATE_IDLE    = 0,  /* Allow either reader or writer */
    STATE_WRITING = 1,  /* Block any readers */
    STATE_READING = 2,  /* Block any writers */
    STATE_PAUSED  = 3,  /* Block both readers and writers */
    STATE_EXITING = 4,  /* Prevent any more readers and writers */
};

struct interrupt {
    struct usb_endpoint_descriptor  *endpoint;
    struct urb                      *urb;
    int                             interval;
    size_t                          buffer_size;
    bool                            done;
    char                            buffer[PACKET_SIZE];
};

struct usb_context {
    struct list_head                siblings;
    struct kref                     ref_count;
    struct usb_transfer_cache       transfer_cache;
    struct usb_interface            *intf;
    struct usb_device               *udev;

    spinlock_t                      err_lock;
    error_t                         error;
    atomic_t                        state;      /* Current state of the control */

    wait_queue_head_t               read_wait;  /* List of threads waiting to read */
    atomic_t                        read_queue; /* Number of threads waiting to read */
    struct mutex                    read_lock;  /* Allow only 1 reader */

    struct usb_transfer_queue       write_list; /* Circular buffer of active/pending writes */

    struct interrupt                interrupt_in;
    struct interrupt                interrupt_out;
};

#define ctx_from_kref(ptr)\
    container_of(ptr, struct usb_context, ref_count)

struct usb_controller_private {
    struct usb_controller   public;
    struct list_head        siblings;
};

#define ctrl_from_public(ptr) \
    container_of(ptr, struct usb_controller_private, public)

static struct usb_container {
    struct list_head    ctrl_list;
    struct list_head    ctx_list;
    struct kref         ref_count;
    /*
        usb_register is called while holding this lock, since it may sleep
        a mutex MUST be used.
     */
    struct mutex        lock;
    wait_queue_head_t   wait;
} usb_container = {
    .ctrl_list  = LIST_HEAD_INIT(usb_container.ctrl_list),
    .ctx_list   = LIST_HEAD_INIT(usb_container.ctx_list),
    .ref_count  = KREF_INIT(0),
    .lock       = __MUTEX_INITIALIZER(usb_container.lock),
    .wait       = __WAIT_QUEUE_HEAD_INITIALIZER(usb_container.wait)
};

static struct usb_context *global_context (
    struct usb_controller *_ctrl
){
    struct usb_controller_private *ctrl = ctrl_from_public(_ctrl);
    struct usb_controller_private *iter;
    long timeout;

    if (WARN_ON(NULL == _ctrl))
        return ERR_PTR(-EINVAL);

    list_for_each_entry(iter, &usb_container.ctrl_list, siblings) {
        if (iter == ctrl)
            goto found;
    }

    ctrl = NULL;

found:
    if (!ctrl)
        return ERR_PTR(-ENODEV);

    /*
        If the device is going through a reconnection cycle, it may not be
        immediately available. We should give it at least 5 seconds to appear.
     */
    AURA_DBG("ctx list: %s", list_empty(&usb_container.ctx_list) ? "empty" : "valid");
    timeout = wait_event_timeout(
        usb_container.wait,
        !list_empty(&usb_container.ctx_list),
        FIVE_SECONDS
    );

    if (timeout == 0)
        return ERR_PTR(-ETIMEDOUT);

    return list_first_entry(&usb_container.ctx_list, struct usb_context, siblings);
}


static inline enum ctrl_state ctrl_switch_state (
    struct usb_context *ctx,
    enum ctrl_state from,
    enum ctrl_state to
){
    return atomic_cmpxchg(&ctx->state, from, to);
}

static inline enum ctrl_state ctrl_state (
    struct usb_context *ctx
){
    return atomic_read(&ctx->state);
}


static void usb_context_destroy (
    struct kref *kref
){
    struct usb_context *ctx = ctx_from_kref(kref);

    AURA_DBG("Destroying usb controller");

    list_del(&ctx->siblings);

    /* free data structures */
    usb_transfer_queue_clear(&ctx->write_list);
    usb_transfer_queue_release(&ctx->write_list);
    usb_transfer_cache_release(&ctx->transfer_cache);

    usb_free_urb(ctx->interrupt_in.urb);
    usb_free_urb(ctx->interrupt_out.urb);

    kfree(ctx);
}

#define ctrl_inc_ref_count(ctx) \
    kref_get(&ctx->ref_count)

#define ctrl_dec_ref_count(ctx) \
    kref_put(&ctx->ref_count, usb_context_destroy)

#define ctrl_wait_event(ctx, event)                     \
({                                                      \
    error_t __err = 0;                                  \
    long __timeout = wait_event_interruptible_timeout(  \
        (ctx)->read_wait, event, FIVE_SECONDS           \
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


static error_t __usb_resume_xfer (
    struct usb_context *ctx
);

static error_t __usb_send_packet (
    struct usb_context *ctx
);


static void __usb_write_packet_complete (
    struct urb *urb
){
    struct usb_context *ctx = urb->context;
    urb_check_status(urb);

    ctx->interrupt_out.done = true;
    wake_up_interruptible(&ctx->read_wait);
}

static error_t __usb_write_packet (
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
        __usb_write_packet_complete,
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

static void __usb_read_packet_complete (
    struct urb *urb
){
    struct usb_context *ctx = urb->context;
    urb_check_status(urb);

    ctx->interrupt_in.done = true;
    wake_up_interruptible(&ctx->read_wait);
}

static error_t __usb_read_packet (
    struct usb_context *ctx,
    struct usb_packet *packet
){
    /*
        Read a packet synchronously, return the error code
     */
    error_t err;

    ctx->interrupt_in.done = false;

    usb_fill_int_urb(
        ctx->interrupt_in.urb,
        ctx->udev,
        usb_rcvintpipe(ctx->udev, ctx->interrupt_in.endpoint->bEndpointAddress),
        ctx->interrupt_in.buffer,
        PACKET_SIZE,
        __usb_read_packet_complete,
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

static error_t __usb_write_read_packet (
    struct usb_context *ctx,
    struct usb_packet *packet,
    bool do_read
){
    /*
        This requires a 2 packet transfer. The first to
        send a command, the second to recieve the data.
     */
    int max_attempts = 10;
    enum ctrl_state state;
    error_t err;

    if (WARN_ON(NULL == ctx || NULL == packet))
        return -EINVAL;

    /* Prevent the control from being deleted */
    ctrl_inc_ref_count(ctx);
    /* Notify any write thread that we want to read */
    atomic_inc(&ctx->read_queue);

    /* Allow only one reader at a time */
    err = mutex_lock_interruptible(&ctx->read_lock);
    if (err)
        goto error_out;

    /*
        The current state is unknown at this point. If it's idle we
        can convert it directly to read. Exiting means we must exit
        but when writing we must wait for the transfer to complete.

        A concurrency problem exists where we cannot simply read the
        state and perform an action since the state may change directly
        after reading it. All we can do is try to claim each state.
     */
    state = STATE_UNKNOWN;
    while (0 < max_attempts-- && state == STATE_UNKNOWN) {
        switch (ctrl_state(ctx)) {
        case STATE_IDLE:
            if (STATE_IDLE == ctrl_switch_state(ctx, STATE_IDLE, STATE_READING))
                state = STATE_READING;
            break;
        case STATE_READING:
            state = STATE_READING;
            break;
        case STATE_EXITING:
            goto error_unlock;
        case STATE_PAUSED:
            wait_event_interruptible(ctx->read_wait, STATE_PAUSED != ctrl_state(ctx));
            break;
        case STATE_WRITING:
            /* Wait until write transaction has completed, or exit event */
            err = ctrl_wait_event(ctx, STATE_WRITING != ctrl_state(ctx));
            if (err) {
                AURA_DBG("Wait for write state failed: %d", err);
                goto error_resume_writing;
            }
            /* Loop again to read the changed state */
            continue;
        case STATE_UNKNOWN:
        default:
            break;
        }
    }

    /* Sanity check */
    if (state != STATE_READING) {
        AURA_ERR("Expected STATE_READING but got: %d", state);
        goto error_resume_writing;
    }

    /* Send the packet */
    err = __usb_write_packet(ctx, packet);
    if (err)
        goto error_resume_writing;

    /* Read a response */
    if (do_read) {
        err = __usb_read_packet(ctx, packet);
        if (err)
            goto error_resume_writing;
    }

error_resume_writing:
    /*
        The final reader should resume any transfer. If the value
        increases between reading it and resuming a write, the
        write will win and the read will have to wait for it to
        complete. But, if there is no pending write, the state
        needs changing to STATE_IDLE, this will still allow a
        newly queued reader to proceed
     */
    if (1 == atomic_read(&ctx->read_queue))
        /* This will set STATE_IDLE if no pending write */
        __usb_resume_xfer(ctx);

error_unlock:
    mutex_unlock(&ctx->read_lock);
error_out:
    ctrl_dec_ref_count(ctx);
    atomic_dec(&ctx->read_queue);

    return err;
}


static void __urb_send_next_packet (
    struct urb *urb
){
    /*
        Cancel transaction on any error or STATE_EXITING
     */
    struct usb_context *ctx = urb->context;
    error_t err = urb_check_status(urb);
    enum ctrl_state state;
    unsigned long flags;

    state = ctrl_state(ctx);
    if (!err) {
        if (STATE_EXITING == state)
            err = -ESHUTDOWN;
        if (STATE_PAUSED == state)
            err = -ECONNRESET;
    }

    if (err) {
        usb_transfer_queue_pop(&ctx->write_list, err);
        ctrl_dec_ref_count(ctx);
    } else {
        AURA_DBG("Write thread sending next packet");
        err = __usb_send_packet(ctx);
        if (err) {
            spin_lock_irqsave(&ctx->err_lock, flags);
            ctx->error = err;
            spin_unlock_irqrestore(&ctx->err_lock, flags);

            ctrl_dec_ref_count(ctx);
        }
    }
}

static void __urb_send_next_xfer (
    struct urb *urb
){
    /*
        Cancel transaction on any error or STATE_EXITING
        Pause transaction when readers waiting.
     */
    struct usb_context *ctx = urb->context;
    error_t err = urb_check_status(urb);
    unsigned long flags;
    enum ctrl_state state;

    usb_transfer_queue_pop(&ctx->write_list, err);

    state = ctrl_state(ctx);
    if (err || STATE_EXITING == state || STATE_PAUSED == state) {
        AURA_DBG("Write thread detected STATE_EXITING");
        ctrl_dec_ref_count(ctx);
    }
    /* Attempt to start a pending reader */
    else if (0 != atomic_read(&ctx->read_queue)) {
        /* The only time this can fail is when EXITING has been set */
        state = ctrl_switch_state(ctx, STATE_WRITING, STATE_READING);
        if (STATE_WRITING == state) {
            AURA_DBG("Write thread switched to STATE_READING");
            wake_up_interruptible(&ctx->read_wait);
        } else {
            AURA_DBG("Write failed to switch to Read with: %d", state);
        }
        ctrl_dec_ref_count(ctx);
    }
    /*
        Begin the next transfer. It's possible for the state to have
        already been changed to STATE_EXITING, but __usb_send_packet()
        will check it again and return an error.
     */
    else if (0 != usb_transfer_queue_count(&ctx->write_list)) {
        AURA_DBG("Write thread starting next transfer");
        err = __usb_send_packet(ctx);
        if (err) {
            AURA_DBG("Write thread failed to start next transfer: %d", err);
            spin_lock_irqsave(&ctx->err_lock, flags);
            ctx->error = err;
            spin_unlock_irqrestore(&ctx->err_lock, flags);

            ctrl_dec_ref_count(ctx);
        } else {
            /* Retain the ref count lock */
        }
    }
    /* Begin idling */
    else {
        AURA_DBG("Write thread complete, setting STATE_IDLE");
        /* It doesn't matter if this fails */
        ctrl_switch_state(ctx, STATE_WRITING, STATE_IDLE);
        ctrl_dec_ref_count(ctx);
    }

    AURA_DBG("kref: %d", kref_read(&ctx->ref_count));
}

static error_t __usb_send_packet (
    struct usb_context *ctx
){
    struct usb_transfer *xfer;
    struct usb_packet *packet;
    error_t err = 0;
    bool last_packet;

    /* Verify device wasn't disconnected */
    if (STATE_WRITING != ctrl_state(ctx)) {
        return -ENODEV;
    }

    /* Fetch the next packet */
    xfer = usb_transfer_queue_peek(&ctx->write_list);
    if (!xfer) {
        AURA_ERR("No xfer returned from ring buffer");
        usb_transfer_queue_debug(&ctx->write_list);
        return -ENODATA;
    }

    packet = usb_transfer_packet_get(xfer);
    if (!packet) {
        AURA_ERR("No packet returned from transfer");
        usb_transfer_debug(xfer);
        return -ENODATA;
    }
    last_packet = usb_transfer_packet_count(xfer) == 0;

    /* Write data into interput_out */
    memcpy(ctx->interrupt_out.buffer, packet, PACKET_SIZE);

    /* Fill the urb (callback should set a completion flag) */
    usb_fill_int_urb(
        ctx->interrupt_out.urb,
        ctx->udev,
        usb_sndintpipe(ctx->udev, ctx->interrupt_out.endpoint->bEndpointAddress),
        ctx->interrupt_out.buffer,
        PACKET_SIZE,
        last_packet ? __urb_send_next_xfer : __urb_send_next_packet,
        ctx,
        ctx->interrupt_out.interval
    );

    // /* Send the urb */
    err = usb_submit_urb(ctx->interrupt_out.urb, GFP_ATOMIC);
    if (err) {
        AURA_ERR("Failed to submit interrupt_out_urb %d", err);
    }

    return err;
}

static error_t __usb_resume_xfer (
    struct usb_context *ctx
){
    /*
        Only a final reader may call this. To resume we
        need to ref count the ctrl and sent the first
        packet.
     */
    error_t err = 0;
    enum ctrl_state state;

    /* Try switching from a read to write */
    AURA_DBG("switch state: %d", atomic_read(&ctx->state));
    state = ctrl_switch_state(ctx, STATE_READING, STATE_WRITING);
    if (STATE_READING == state) {
        /* Check if there is actally anything to write */
        if (0 == usb_transfer_queue_count(&ctx->write_list)) {
            ctrl_switch_state(ctx, STATE_WRITING, STATE_IDLE);
        } else {
            ctrl_inc_ref_count(ctx);
            err = __usb_send_packet(ctx);
            if (err) {
                ctrl_switch_state(ctx, STATE_WRITING, STATE_IDLE);
                ctrl_dec_ref_count(ctx);
            }
        }
    } else if (!(STATE_EXITING == state || STATE_PAUSED == state)) {
        AURA_DBG("Invalid state while switching from read to write: %d", state);
        err = -EIO;
    }

    if (err) {
        spin_lock_irq(&ctx->err_lock);
        ctx->error = err;
        spin_unlock_irq(&ctx->err_lock);
    }

    return err;
}


static int __usb_connect (
    struct usb_interface *intf,
    const struct usb_device_id *id
){
    struct usb_device *udev = interface_to_usbdev(intf);
    struct usb_context *ctx = NULL;
    struct usb_host_interface *iface_desc = intf->cur_altsetting;
    int err = -ENOMEM;

    /* allocate memory for our device state and initialize it */
    ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
    if (!ctx)
        return -ENOMEM;

    ctx->intf = intf;
    ctx->udev = udev;
    kref_init(&ctx->ref_count);
    atomic_set(&ctx->state, STATE_IDLE);

    /* Init the readers */
    mutex_init(&ctx->read_lock);
    init_waitqueue_head(&ctx->read_wait);
    atomic_set(&ctx->read_queue, 0);

    err = usb_transfer_cache_init(&ctx->transfer_cache);
    if (err) {
        AURA_ERR("Failed to initialize transfer cache");
        goto error_free_ctrl;
    }

    /* Init the writers*/
    err = usb_transfer_queue_init(&ctx->write_list);
    if (err) {
        AURA_ERR("Failed to initialize transfer queue");
        goto error_free_ctrl;
    }

    /* Initialize the IN endpoint */
    err = usb_find_int_in_endpoint(iface_desc, &ctx->interrupt_in.endpoint);
    if (err) {
        AURA_ERR("Interrupt IN endpoint not found");
        goto error_free_ctrl;
    }

    ctx->interrupt_in.interval = ctx->interrupt_in.endpoint->bInterval;
    ctx->interrupt_in.urb = usb_alloc_urb(0, GFP_KERNEL);
    if (!ctx->interrupt_in.urb)
        goto error_free_ctrl;

    /* Initialize the OUT endpoint */
    err = usb_find_int_out_endpoint(iface_desc, &ctx->interrupt_out.endpoint);
    if (err) {
        AURA_ERR("Interrupt OUT endpoint not found");
        goto error_free_ctrl;
    }

    ctx->interrupt_out.interval = ctx->interrupt_out.endpoint->bInterval;
    ctx->interrupt_out.urb = usb_alloc_urb(0, GFP_KERNEL);
    if (!ctx->interrupt_out.urb)
        goto error_free_ctrl;

    /* we can register the device now, as it is ready */
    err = usb_register_dev(intf, &usb_header_class);
    if (err) {
        AURA_ERR("Failed to get a minor for this device.");
        goto error_free_ctrl;
    }

    usb_set_intfdata(intf, ctx);

    /* Wake all threads waiting for the device */
    list_add_tail(&ctx->siblings, &usb_container.ctx_list);
    wake_up_all(&usb_container.wait);

    AURA_INFO("Registered USB controller for product 0x%04X", udev->descriptor.idProduct);

    return err;

error_free_ctrl:
    ctrl_dec_ref_count(ctx);

    return err;
}

static void __usb_disconnect (
    struct usb_interface *intf
){
    struct usb_context *ctx;
    int minor, state;
    int readers, writers;

    ctx = usb_get_intfdata(intf);
    usb_set_intfdata(intf, NULL);

    minor = intf->minor;

    /* give back our minor */
    usb_deregister_dev(intf, &usb_header_class);

    /* if the device is not opened, then we clean up right now */
    state = atomic_xchg(&ctx->state, STATE_EXITING);
    readers = atomic_read(&ctx->read_queue);
    writers = usb_transfer_queue_count(&ctx->write_list);

    AURA_DBG(
        "usb disconnect: state = %d, readers = %d, writers = %d, kref = %d",
        state, readers, writers, kref_read(&ctx->ref_count)
    );

    /* Force close on invalid state */
    if (STATE_IDLE == state || (!readers && !writers)) {
        kref_put(&ctx->ref_count, usb_context_destroy);
    } else {
        usb_kill_urb(ctx->interrupt_in.urb);
        usb_kill_urb(ctx->interrupt_out.urb);
        /* wake up any pending readers */
        wake_up_interruptible_all(&ctx->read_wait);
    }
}

static int __usb_suspend (
    struct usb_interface *intf,
    pm_message_t message
){
    /*
        Pause all readers and writers to prevent any more packets
        from being sent. If suspend was called mid-transfer, then
        the completion callback for that urb should see an error
        and invoke the transfer complete function.
     */
    struct usb_context *ctx = usb_get_intfdata(intf);

    if (!ctx)
        return 0;

    atomic_set(&ctx->state, STATE_PAUSED);
    usb_kill_urb(ctx->interrupt_in.urb);
    usb_kill_urb(ctx->interrupt_out.urb);

    AURA_INFO("USB suspended");

    return 0;
}

static int __usb_resume (
    struct usb_interface *intf
){
    struct usb_context *ctx = usb_get_intfdata(intf);
    enum ctrl_state state;
    error_t err;

    AURA_INFO("USB resuming");

    if (!ctx) {
        AURA_DBG("Interface missing context");
        goto reset_driver;
    }

    /* Check for pending readers */
    state = ctrl_switch_state(ctx, STATE_PAUSED, STATE_READING);
    if (STATE_PAUSED == state) {
        if (0 != atomic_read(&ctx->read_queue)) {
            wake_up_interruptible(&ctx->read_wait);
            AURA_INFO("USB resumed readers");
            return 0;
        } else {
            AURA_DBG("No pending readers found");
            ctrl_switch_state(ctx, STATE_READING, STATE_PAUSED);
        }
    } else {
        AURA_ERR("Expected a paused state, got: %d", state);
        goto reset_driver;
    }

    /* Check for pending writers */
    state = ctrl_switch_state(ctx, STATE_PAUSED, STATE_WRITING);
    if (STATE_PAUSED == state) {
        if (0 != usb_transfer_queue_count(&ctx->write_list)) {
            err = __usb_send_packet(ctx);
            if (err) {
                AURA_DBG("__usb_send_packet() Failed: %d", err);
                goto reset_driver;
            }
            AURA_INFO("USB resumed writer");
            return 0;
        } else {
            AURA_DBG("No pending writers found");
            ctrl_switch_state(ctx, STATE_WRITING, STATE_PAUSED);
        }
    } else {
        AURA_ERR("Expected a paused state, got: %d", state);
        goto reset_driver;
    }

    /* Begin to idle */
    state = ctrl_switch_state(ctx, STATE_PAUSED, STATE_IDLE);
    if (STATE_PAUSED == state) {
        AURA_INFO("USB resumed idle");
        return 0;
    } else {
        AURA_ERR("Expected a paused state, got: %d", state);
        goto reset_driver;
    }

reset_driver:
    __usb_disconnect(intf);
    return __usb_connect(intf, NULL);
}

static int __usb_pre_reset (
    struct usb_interface *intf
){
    AURA_DBG("__usb_pre_reset");
    return 0;
}

static int __usb_post_reset (
    struct usb_interface *intf
){
    AURA_DBG("__usb_post_reset");
    return 0;
}


struct usb_driver header_driver = {
    .name           = "aura-mb-headers",
    .probe          = __usb_connect,
    .disconnect     = __usb_disconnect,
    .suspend        = __usb_suspend,
    .resume         = __usb_resume,
    .reset_resume   = __usb_resume,
    .pre_reset      = __usb_pre_reset,
    .post_reset     = __usb_post_reset,
    .id_table       = usb_header_table,
};

struct usb_transfer *usb_transfer_alloc (
    struct usb_controller *ctrl
){
    struct usb_context *ctx = global_context(ctrl);
    if (IS_ERR(ctx))
        return ERR_CAST(ctx);

    return usb_transfer_create(&ctx->transfer_cache);
}

error_t usb_read_packet (
    struct usb_controller *ctrl,
    void *packet
){
    struct usb_context *ctx = global_context(ctrl);
    if (IS_ERR(ctx))
        return PTR_ERR(ctx);

    return __usb_write_read_packet(ctx, packet, true);
}

error_t usb_write_packet (
    struct usb_controller *ctrl,
    const void *packet
){
    struct usb_context *ctx = global_context(ctrl);
    if (IS_ERR(ctx))
        return PTR_ERR(ctx);

    return __usb_write_read_packet(ctx, (void*)packet, false);
}

error_t usb_send_transfer (
    struct usb_controller *ctrl,
    struct usb_transfer *xfer
){
    /*
        Send packets asynchronously, store the error in the ctrl.

        The transaction should be cancellable at any time, and should
        be pausable only between transactions.

        Called from a non interrupt context.

        If STATE_WRITING we should presume an async operation is in
        progress, BUT since the operation can change the state, a
        race condiftion may occur.
     */
    struct usb_context *ctx;
    error_t err = 0;
    int queue_index;

    ctx = global_context(ctrl);
    if (IS_ERR(ctx))
        return PTR_ERR(ctx);

    queue_index = usb_transfer_queue_push(&ctx->write_list, xfer);
    if (queue_index < 0)
        return queue_index;

    AURA_DBG("Added xfer to ring buffer at index: %d", queue_index);

    /*
        To avoid race conditions, only begin a transfer if this
        thread can successfully change the state.
     */
    if (STATE_IDLE == ctrl_switch_state(ctx, STATE_IDLE, STATE_WRITING)) {
        /* Prevent the ctrl from being destroyed */
        ctrl_inc_ref_count(ctx);
        err = __usb_send_packet(ctx);
        if (err) {
            AURA_DBG("Failed to send packet: %d", err);
            ctrl_switch_state(ctx, STATE_WRITING, STATE_IDLE);
            ctrl_dec_ref_count(ctx);
        }
    } else {
        /*
            If the state is not already writing, then either the
            final reader thread will begin the transfer, or an
            exit event was issued. Non of which is an error.
         */
    }

    return err;
}

static error_t usb_driver_register (
    void
){
    struct usb_context *ctx;
    size_t count = 0;
    error_t err;

    err = usb_register_driver(&header_driver, THIS_MODULE, "aura");
    if (err)
       return err;

    /* There should be only one context */
    list_for_each_entry(ctx, &usb_container.ctx_list, siblings)
       count++;

    if (count != 1) {
       AURA_ERR("Expected exactly 1 USB controller, got: %ld", count);
       usb_deregister(&header_driver);
       return -ENODEV;
    }

    return 0;
}

static void usb_driver_unregister (
    struct kref *kref
){
    usb_deregister(&header_driver);
}

struct usb_controller *usb_controller_create (
    void
){
    /*
        If the underlying device gets disconnected, which can happen
        during a power cycle or from user intervention, the ctrl
        returned to the caller would become invalid.

        Here, we create a controller with a reference count. The driver
        is bound to this count but the context can be recreated at any time.
     */
    struct usb_controller_private *ctrl;
    error_t err;

    ctrl = kzalloc(sizeof(*ctrl), GFP_KERNEL);
    if (!ctrl)
        return ERR_PTR(-ENOMEM);

    mutex_lock(&usb_container.lock);

    if (0 == kref_read(&usb_container.ref_count)) {
        err = usb_driver_register();
        if (err)
            goto error_unlock;
    }

    kref_get(&usb_container.ref_count);
    list_add_tail(&ctrl->siblings, &usb_container.ctrl_list);

    mutex_unlock(&usb_container.lock);

    return &ctrl->public;

error_unlock:
    mutex_unlock(&usb_container.lock);

    return ERR_PTR(err);
}

void usb_controller_destroy (
    struct usb_controller *_ctrl
){
    struct usb_controller_private *ctrl = ctrl_from_public(_ctrl);
    struct usb_controller_private *iter;

    if (WARN_ON(NULL == _ctrl))
        return;

    list_for_each_entry(iter, &usb_container.ctrl_list, siblings) {
        if (iter == ctrl)
            goto found;
    }

    ctrl = NULL;

found:
    if (!ctrl)
        return;

    mutex_lock(&usb_container.lock);

    kref_put(&usb_container.ref_count, usb_driver_unregister);
    list_del(&ctrl->siblings);
    kfree(ctrl);

    mutex_unlock(&usb_container.lock);
}
