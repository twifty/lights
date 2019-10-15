// SPDX-License-Identifier: GPL-2.0
/*
 * A usb driver for the aura argb headers on asus motherboards.
 *
 * Copyright (C) 2019 Owen Parry
 *
 * Authors:
 * Owen Parry <twifty@zoho.com>
 */
#include "usb-driver.h"
#include "../aura.h"

static uint16_t header_led_count[5] = {60, 60, 60, 60, 60};

module_param_array(header_led_count, short, NULL, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(header_led_count, "An array of numbers representing the count of leds on each header.");

enum aura_header_mode {
    /* Lights modes */
    AURA_MODE_OFF                   = 0x00,
    AURA_MODE_STATIC                = 0x01,
    AURA_MODE_BREATHING             = 0x02,
    AURA_MODE_FLASHING              = 0x03,
    AURA_MODE_CYCLE                 = 0x04,
    AURA_MODE_RAINBOW               = 0x05,

    /* Custom modes */
    AURA_MODE_CYCLE_BREATHING       = 0x06,
    AURA_MODE_CHASE_FADE            = 0x07,
    AURA_MODE_CYCLE_CHASE_FADE      = 0x08,
    AURA_MODE_CHASE                 = 0x09,
    AURA_MODE_CYCLE_CHASE           = 0x0A,
    AURA_MODE_CYCLE_WAVE            = 0x0B,

    INDEX_MODE_LAST                 = AURA_MODE_CYCLE_WAVE,

    /* Non sequential entries */
    AURA_MODE_CYCLE_RANDOM_FLICKER  = 0x0D,
    INDEX_MODE_CYCLE_RANDOM_FLICKER = INDEX_MODE_LAST + 1,

    AURA_MODE_DIRECT                = 0xFF,
    INDEX_MODE_DIRECT               = INDEX_MODE_LAST + 2,
};

static const struct lights_mode aura_header_modes[] = {
    LIGHTS_MODE(OFF),
    LIGHTS_MODE(STATIC),
    LIGHTS_MODE(BREATHING),
    LIGHTS_MODE(FLASHING),
    LIGHTS_MODE(CYCLE),
    LIGHTS_MODE(RAINBOW),

    LIGHTS_CUSTOM_MODE(AURA_MODE_CYCLE_BREATHING,       "cycle_breathing"),
    LIGHTS_CUSTOM_MODE(AURA_MODE_CHASE_FADE,            "chase_fade"),
    LIGHTS_CUSTOM_MODE(AURA_MODE_CYCLE_CHASE_FADE,      "cycle_chase_fade"),
    LIGHTS_CUSTOM_MODE(AURA_MODE_CHASE,                 "chase"),
    LIGHTS_CUSTOM_MODE(AURA_MODE_CYCLE_CHASE,           "cycle_chase"),
    LIGHTS_CUSTOM_MODE(AURA_MODE_CYCLE_WAVE,            "cycle_wave"),
    LIGHTS_CUSTOM_MODE(AURA_MODE_CYCLE_RANDOM_FLICKER,  "cycle_random_flicker"),

    LIGHTS_CUSTOM_MODE(AURA_MODE_DIRECT,                "direct"),

    LIGHTS_MODE_LAST_ENTRY()
};

static error_t get_header_mode (
    const struct lights_mode *mode,
    enum aura_header_mode *header_mode
){
    if (lights_is_custom_mode(mode)) {
        *header_mode = lights_custom_mode_id(mode);

        if (*header_mode <= INDEX_MODE_LAST)
            return 0;
        if (*header_mode <= AURA_MODE_CYCLE_RANDOM_FLICKER)
            return 0;
        if (*header_mode <= AURA_MODE_DIRECT)
            return 0;

        return -ENODATA;
    }

    switch (lights_mode_id(mode)) {
        case LIGHTS_MODE_OFF:
            *header_mode = AURA_MODE_OFF;
            return 0;
        case LIGHTS_MODE_STATIC:
            *header_mode = AURA_MODE_STATIC;
            return 0;
        case LIGHTS_MODE_BREATHING:
            *header_mode = AURA_MODE_BREATHING;
            return 0;
        case LIGHTS_MODE_FLASHING:
            *header_mode = AURA_MODE_FLASHING;
            return 0;
        case LIGHTS_MODE_CYCLE:
            *header_mode = AURA_MODE_CYCLE;
            return 0;
        case LIGHTS_MODE_RAINBOW:
            *header_mode = AURA_MODE_RAINBOW;
            return 0;
    }

    return -ENODATA;
}

static uint8_t get_aura_mode (
    const struct lights_mode *mode
){
    enum aura_header_mode aura_mode;

    get_header_mode(mode, &aura_mode);

    return (uint8_t)aura_mode;
}

static const struct lights_mode *get_lights_mode (
    enum aura_header_mode header_mode
){
    if (header_mode == AURA_MODE_CYCLE_RANDOM_FLICKER) {
        return &aura_header_modes[INDEX_MODE_CYCLE_RANDOM_FLICKER];
    }

    if (header_mode == AURA_MODE_DIRECT) {
        return &aura_header_modes[INDEX_MODE_DIRECT];
    }

    if (header_mode >= 0 && header_mode <= INDEX_MODE_LAST) {
        return &aura_header_modes[header_mode];
    }

    return NULL;
}

enum HEADER_CONSTS {
    PACKET_RAW_SIZE     = PACKET_SIZE - 2,
    PACKET_DIRECT_SIZE  = PACKET_SIZE - 5,
    PACKET_LED_COUNT    = 0x14,
    PACKET_LED_SIZE     = PACKET_LED_COUNT * 3,

    MAX_SPEED_VALUE     = 5,
    MAX_HEADER_COUNT    = 5,
};

enum HEADER_CONTROL {
    PACKET_CONTROL          = 0xEC,
    PACKET_CMD_READ         = 0x80,

    PACKET_CMD_NAME         = 0x02,
    PACKET_CMD_CAPS         = 0x30,
    PACKET_CMD_ENABLE       = 0x35,
    PACKET_CMD_EFFECT       = 0x3B,
    PACKET_CMD_RESET        = 0x3F,
    PACKET_CMD_DIRECT       = 0x40,
    PACKET_CMD_OLED_CAPS    = 0x50,
};

#pragma pack(1)

struct data_direct {
    uint8_t     flags;
    uint8_t     offset;
    uint8_t     count;
    uint8_t     value[PACKET_DIRECT_SIZE];
};

struct data_effect {
    uint8_t     header;
    uint8_t     unknown;
    uint8_t     mode;
    uint8_t     red;
    uint8_t     green;
    uint8_t     blue;
    uint8_t     direction;
    uint8_t     speed;
};

struct packet_data {
    uint8_t     control;
    uint8_t     command;

    union {
        uint8_t             raw[PACKET_RAW_SIZE];
        struct data_effect  effect;
        struct data_direct  direct;
    } data;
};

#pragma pack()

#define packet_init(p, c)           \
({                                  \
    compiletime_assert(             \
        sizeof(*p) == PACKET_SIZE,  \
        "Incorrect packet size"     \
    );                              \
    memset(p, 0, PACKET_SIZE);      \
    (p)->control = PACKET_CONTROL;  \
    (p)->command = c;               \
    (p);                            \
})

enum update_flags {
    UPDATE_COLOR        = 0x01,
    UPDATE_MODE         = 0x02,
    UPDATE_SPEED        = 0x04,
    UPDATE_DIRECTION    = 0x08,
};

struct aura_effect {
    struct lights_color color;
    struct lights_mode  mode;
    uint8_t             speed;
    uint8_t             direction;
};

struct aura_header_zone {
    struct aura_header_controller   *ctrl;

    spinlock_t                      lock;
    uint8_t                         id;
    uint16_t                        led_count;
    char                            name[16]; // "argb-strip-00"
    struct lights_dev               lights;

    struct aura_effect              effect;
    struct aura_effect              applied;
};

struct update_event {
    struct aura_header_zone         *zone;
    uint16_t                        flags;
    struct aura_effect              effect;
};

struct aura_header_controller {
    struct usb_controller           *usb_ctrl;

    bool                            oled_capable;
    uint8_t                         oled_type;

    uint8_t                         zone_count;
    struct aura_header_zone         *zones;
    char                            name[PACKET_RAW_SIZE];

    struct event_cache {
        spinlock_t                  lock;
        struct update_event         buffer[MAX_TRANSFERS];
        DECLARE_BITMAP(             usage, MAX_TRANSFERS);
    } event_cache;
};

static struct aura_header_controller *aura_header_ctrl = NULL;

static const struct aura_effect effect_direct = {
    .mode = aura_header_modes[INDEX_MODE_DIRECT]
};

static const struct aura_effect effect_default = {
    .mode = aura_header_modes[AURA_MODE_RAINBOW]
};


static void event_release (
    struct update_event *event
){
    struct event_cache *cache;
    unsigned long index, flags;

    if (unlikely(event == NULL))
        return;

    if (WARN_ON(NULL == event->zone))
        return;

    cache = &event->zone->ctrl->event_cache;
    index = ((void*)event - (void*)cache->buffer) / sizeof(*event);
    if (index >= MAX_TRANSFERS) {
        AURA_DBG("event index is out of bounds: %ld", index);
        return;
    }

    if (in_interrupt()) {
        spin_lock_irqsave(&cache->lock, flags);
        clear_bit(index, cache->usage);
        spin_unlock_irqrestore(&cache->lock, flags);
    } else {
        spin_lock_irq(&cache->lock);
        clear_bit(index, cache->usage);
        spin_unlock_irq(&cache->lock);
    }
}

static struct update_event *event_create (
    struct aura_header_zone *zone
){
    struct update_event *event = NULL;
    struct event_cache *cache;
    uint8_t index;

    if (!zone)
        return NULL;

    cache = &zone->ctrl->event_cache;

    spin_lock_irq(&cache->lock);

    index = find_first_zero_bit(cache->usage, MAX_TRANSFERS);
    if (index != MAX_TRANSFERS) {
        event = &cache->buffer[index];
        set_bit(index, cache->usage);
    }

    spin_unlock_irq(&cache->lock);

    if (!event) {
        AURA_DBG("Not enough memory allocated for event cache");
        return NULL;
    }

    event->zone   = zone;
    event->flags  = 0;

    // spin_lock_irq(&zone->lock);
    // event->effect = zone->applied;
    // spin_unlock_irq(&zone->lock);

    return event;
}

static inline void event_set_mode (
    struct update_event *event,
    const struct lights_mode *mode
){
    event->flags |= UPDATE_MODE;
    event->effect.mode = *mode;
}

static inline void event_set_color (
    struct update_event *event,
    const struct lights_color *color
){
    event->flags |= UPDATE_COLOR;
    event->effect.color = *color;
}

static inline void event_set_speed (
    struct update_event *event,
    uint8_t speed
){
    event->flags |= UPDATE_SPEED;
    event->effect.speed = speed;
}

static inline void event_set_direction (
    struct update_event *event,
    uint8_t direction
){
    event->flags |= UPDATE_DIRECTION;
    event->effect.direction = direction;
}


static int usb_get_zone_count (
    struct usb_controller *usb_ctrl,
    uint8_t *zone_count
){
    const uint8_t map[0x1E] = {
        00, 05, 01, 05, 05, 05, 02, 05, 05, 05,
        05, 05, 05, 05, 03, 05, 05, 05, 05, 05,
        05, 05, 05, 05, 05, 05, 05, 05, 05, 05//, 04
    };
    struct packet_data packet;
    int count;
    error_t err;

    packet_init(&packet, PACKET_CMD_CAPS | PACKET_CMD_READ);

    err = usb_read_packet(usb_ctrl, &packet);
    if (err) {
        AURA_DBG("read failed with %d", err);
        return err;
    }

    if (packet.command != PACKET_CMD_CAPS) {
        AURA_DBG("Unexpected reply while handshaking");
        dump_packet("PACKET_CMD_CAPS: ", &packet);

        return -EBADMSG;
    }

    count = packet.data.raw[5] - 1;
    if (count < 0x1E) {
        count = map[count] + 1;
        if (count > MAX_HEADER_COUNT)
            count = 0;
    } else {
        count = 0;
    }

    AURA_DBG("Detected %d headers", count);

    if (!count)
        return -ENODEV;

    *zone_count = count;

    return 0;
}

static error_t usb_get_name (
    struct usb_controller *usb_ctrl,
    char *name,
    size_t len
){
    struct packet_data packet;
    error_t err;

    packet_init(&packet, PACKET_CMD_READ | PACKET_CMD_NAME);

    err = usb_read_packet(usb_ctrl, (void*)&packet);
    if (err) {
        AURA_DBG("read failed with %d", err);
        return err;
    }

    if (packet.command != PACKET_CMD_NAME) {
        AURA_DBG("Unexpected reply while handshaking");
        dump_packet("0xB0 packet: ", &packet);

        return -EBADMSG;
    }

    memcpy(name, packet.data.raw, min_t(size_t, len, PACKET_RAW_SIZE));

    AURA_INFO("Discovered aura controller '%s'", name);

    return 0;
}

static error_t usb_detect_oled (
    struct usb_controller *usb_ctrl,
    bool *oled_capable,
    uint8_t *oled_type
){
    struct packet_data packet;
    error_t err;

    packet_init(&packet, PACKET_CMD_READ | PACKET_CMD_OLED_CAPS);

    err = usb_read_packet(usb_ctrl, (void*)&packet);
    if (err) {
        AURA_DBG("read failed with %d", err);
        return err;
    }

    if (packet.command != PACKET_CMD_OLED_CAPS) {
        AURA_DBG("Unexpected reply while handshaking");
        dump_packet("0x30 packet: ", &packet);

        return -EBADMSG;
    }

    *oled_capable = packet.data.raw[0];
    *oled_type = packet.data.raw[2];

    AURA_INFO(
        "Oled capable: %s, type: %d",
        *oled_capable ? "true" : "false",
        *oled_type
    );

    return 0;
}

static error_t usb_reset_device (
    struct aura_header_controller *ctrl
){
    /*
        NOTE: Sending this packet on a freshly booted system will cause the
        device to reconnect. Upon its return it may have a different devnum.

        ANY and all queued packets will be discarded.

        The udev rule needs to be in place for the newly connected device
        to be bound to this driver.
     */
    struct aura_header_zone *zone;
    struct packet_data packet;
    error_t err;
    int i;

    packet_init(&packet, PACKET_CMD_RESET);
    packet.data.raw[0] = 0xAA;

    err = usb_write_packet(ctrl->usb_ctrl, &packet);
    if (err) {
        AURA_DBG("usb_write_packet() failed with %d", err);
        return err;
    }

    for (i = 0; i < ctrl->zone_count; i++) {
        zone = &ctrl->zones[i];

        spin_lock_irq(&zone->lock);

        zone->effect = effect_default;
        zone->applied = effect_default;

        spin_unlock_irq(&zone->lock);
    }

    return err;
}


static error_t transfer_add_effect (
    struct usb_transfer *xfer,
    struct aura_header_zone *zone,
    const struct aura_effect *effect
){
    /*
        The speed given should be an int between 0 (slowest) and 5 (fastest)
     */
    uint8_t aura_speeds[] = {0xFF, 0xCC, 0x99, 0x66, 0x33, 0x00};
    struct packet_data *packet;

    packet = usb_transfer_packet_alloc(xfer);
    if (!packet)
        return -ENOMEM;

    packet_init(packet, PACKET_CMD_EFFECT);

    packet->data.effect.header       = zone->id;
    packet->data.effect.mode         = get_aura_mode(&effect->mode);
    packet->data.effect.red          = effect->color.r;
    packet->data.effect.green        = effect->color.g;
    packet->data.effect.blue         = effect->color.b;
    packet->data.effect.direction    = effect->direction;
    packet->data.effect.speed        = aura_speeds[effect->speed];

    return 0;
}

static error_t transfer_add_direct (
    struct usb_transfer *xfer,
    struct aura_header_zone *zone,
    uint8_t command,
    void *data,
    uint8_t data_size,
    uint8_t data_count
){
    /*
        NOTE: Beware of packing when passing arrays to this function.
     */
    struct packet_data *packet;
    struct data_direct *direct;
    size_t bytes_to_transfer = data_size * data_count;
    size_t max_loops = (bytes_to_transfer / PACKET_DIRECT_SIZE) + 1;
    size_t max_items_per_packet;
    uint16_t src_offset = 0;
    int curr_loop, i;

    if (!(data_size && data_count))
        return -EINVAL;

    max_items_per_packet = PACKET_DIRECT_SIZE / data_size;

    for (curr_loop = 0; curr_loop < max_loops; curr_loop++) {
        packet = usb_transfer_packet_alloc(xfer);
        if (!packet)
            return -ENOMEM;

        packet_init(packet, command);

        direct = &packet->data.direct;
        direct->flags = zone->id;

        /* NOTE - LightsService has this as only greater than */
        if (src_offset >= 0x100)
            direct->flags = (src_offset >> 8) & 0xf;

        if (curr_loop + 1 == max_loops)
            direct->flags |= 0x80;

        direct->offset = (uint8_t)src_offset;
        direct->count  = min_t(uint8_t, data_count, max_items_per_packet);

        for (i = 0; i < direct->count; i++) {
            direct->value[i] = data ? ((uint8_t*)data)[src_offset] : 0;
            src_offset++;
        }
    }

    return 0;
}

static error_t transfer_add_enable (
    struct usb_transfer *xfer,
    struct aura_header_zone *zone,
    bool enable
){
    /*
        This packet actually pauses the leds (when on rainbow)
     */
    struct packet_data *packet;

    packet = usb_transfer_packet_alloc(xfer);
    if (!packet)
        return -ENOMEM;

    packet_init(packet, PACKET_CMD_ENABLE);
    packet->data.raw[0] = zone->id;
    packet->data.raw[1] = (uint8_t)enable;

    return 0;
}


static void handle_event (
    void *data,
    error_t err
){
    struct update_event *event = data;
    unsigned long flags;

    if (err) {
        AURA_DBG("transfer completed with %d", err);
    } else if (event) {
        if (WARN_ON(NULL == event->zone)) {
            event_release(event);
            return;
        }

        spin_lock_irqsave(&event->zone->lock, flags);

        if (event->flags & UPDATE_COLOR) {
            AURA_DBG(
                "Setting color for zone:%d to 0x%02x%02x%02x",
                event->zone->id,
                event->effect.color.r,
                event->effect.color.g,
                event->effect.color.b
            );

            event->zone->effect.color = event->effect.color;
        }

        if (event->flags & UPDATE_MODE) {
            AURA_DBG(
                "Setting mode for zone:%d to %s",
                event->zone->id,
                event->effect.mode.name
            );

            event->zone->effect.mode = event->effect.mode;
        }

        if (event->flags & UPDATE_SPEED) {
            AURA_DBG(
                "Setting speed for zone:%d to %d",
                event->zone->id,
                event->effect.speed
            );

            event->zone->effect.speed = event->effect.speed;
        }

        if (event->flags & UPDATE_DIRECTION) {
            AURA_DBG(
                "Setting direction for zone:%d to %d",
                event->zone->id,
                event->effect.direction
            );

            event->zone->effect.direction = event->effect.direction;
        }

        spin_unlock_irqrestore(&event->zone->lock, flags);
    }

    event_release(event);
}


static error_t zone_set_mode (
    struct aura_header_zone *zone,
    const struct lights_mode *mode
){
    enum aura_header_mode header_mode;
    struct update_event *event = NULL;
    struct usb_transfer *xfer;
    error_t err;

    err = get_header_mode(mode, &header_mode);
    if (err)
        return err;

    xfer = usb_transfer_alloc(zone->ctrl->usb_ctrl);
    if (IS_ERR(xfer))
        return PTR_ERR(xfer);

    event = event_create(zone);
    if (!event) {
        err = -ENOMEM;
        goto error_free;
    }

    spin_lock_irq(&zone->lock);
    event->effect = zone->applied;

    if (header_mode == get_aura_mode(&event->effect.mode))
        goto error_free;

    switch (header_mode) {
        case AURA_MODE_OFF:
        case AURA_MODE_DIRECT:
            event_set_mode(event, mode);
            if ((err = transfer_add_effect(xfer, zone, &effect_direct)))
                goto error_free;
            if ((err = transfer_add_direct(xfer, zone, 0x40, NULL, 3, zone->led_count)))
                goto error_free;
            break;

        default:
            if (AURA_MODE_OFF == get_aura_mode(&event->effect.mode)) {
                if ((err = transfer_add_enable(xfer, zone, true)))
                    goto error_free;
            }

            event_set_mode(event, mode);
            if ((err = transfer_add_effect(xfer, zone, &event->effect)))
                goto error_free;
            break;
    }

    xfer->data = event;
    xfer->complete = handle_event;

    err = usb_send_transfer(zone->ctrl->usb_ctrl, xfer);
    if (err)
        goto error_free;

    /* It's impossible for the transfer to complete while we hold the lock */
    zone->applied = event->effect;

    spin_unlock_irq(&zone->lock);
    usb_transfer_put(xfer);

    return 0;

error_free:
    spin_unlock_irq(&zone->lock);
    event_release(event);
    usb_transfer_put(xfer);

    return err;
}

static error_t zone_set_color (
    struct aura_header_zone *zone,
    const struct lights_color *color
){
    struct update_event *event = NULL;
    struct usb_transfer *xfer;
    error_t err = 0;

    xfer = usb_transfer_alloc(zone->ctrl->usb_ctrl);
    if (IS_ERR(xfer))
        return PTR_ERR(xfer);

    event = event_create(zone);
    if (!event) {
        err = -ENOMEM;
        goto error_free;
    }

    spin_lock_irq(&zone->lock);
    event->effect = zone->applied;

    if (lights_color_equal(color, &event->effect.color))
        goto error_free;

    event_set_color(event, color);
    if ((err = transfer_add_effect(xfer, zone, &event->effect)))
        goto error_free;

    xfer->data = event;
    xfer->complete = handle_event;

    err = usb_send_transfer(zone->ctrl->usb_ctrl, xfer);
    if (err)
        goto error_free;

    /* It's impossible for the transfer to complete while we hold the lock */
    zone->applied = event->effect;

    spin_unlock_irq(&zone->lock);
    usb_transfer_put(xfer);

    return 0;

error_free:
    spin_unlock_irq(&zone->lock);
    event_release(event);
    usb_transfer_put(xfer);

    return err;
}

static error_t zone_set_speed (
    struct aura_header_zone *zone,
    uint8_t speed
){
    struct update_event *event = NULL;
    struct usb_transfer *xfer;
    error_t err = 0;

    if (speed > MAX_SPEED_VALUE)
        speed = 5;

    xfer = usb_transfer_alloc(zone->ctrl->usb_ctrl);
    if (IS_ERR(xfer))
        return PTR_ERR(xfer);

    event = event_create(zone);
    if (!event) {
        err = -ENOMEM;
        goto error_free;
    }

    spin_lock_irq(&zone->lock);
    event->effect = zone->applied;

    if (speed == event->effect.speed)
        goto error_free;

    event_set_speed(event, speed);
    if ((err = transfer_add_effect(xfer, zone, &event->effect)))
        goto error_free;

    xfer->data = event;
    xfer->complete = handle_event;

    err = usb_send_transfer(zone->ctrl->usb_ctrl, xfer);
    if (err)
        goto error_free;

    /* It's impossible for the transfer to complete while we hold the lock */
    zone->applied = event->effect;

    spin_unlock_irq(&zone->lock);
    usb_transfer_put(xfer);

    return 0;

error_free:
    spin_unlock_irq(&zone->lock);
    event_release(event);
    usb_transfer_put(xfer);

    return err;
}

static error_t zone_set_direction (
    struct aura_header_zone *zone,
    uint8_t direction
){
    struct update_event *event = NULL;
    struct usb_transfer *xfer;
    error_t err = 0;

    xfer = usb_transfer_alloc(zone->ctrl->usb_ctrl);
    if (IS_ERR(xfer))
        return PTR_ERR(xfer);

    event = event_create(zone);
    if (!event) {
        err = -ENOMEM;
        goto error_free;
    }

    spin_lock_irq(&zone->lock);
    event->effect = zone->applied;

    if (direction > 1)
        direction = 1;
    if (direction == event->effect.direction)
        goto error_free;

    event_set_direction(event, direction);
    if ((err = transfer_add_effect(xfer, zone, &event->effect)))
        goto error_free;

    xfer->data = event;
    xfer->complete = handle_event;

    err = usb_send_transfer(zone->ctrl->usb_ctrl, xfer);
    if (err)
        goto error_free;

    /* It's impossible for the transfer to complete while we hold the lock */
    zone->applied = event->effect;

    spin_unlock_irq(&zone->lock);
    usb_transfer_put(xfer);

    return 0;

error_free:
    spin_unlock_irq(&zone->lock);
    event_release(event);
    usb_transfer_put(xfer);

    return err;
}

static error_t zone_set_direct (
    struct aura_header_zone *zone,
    const struct lights_buffer *buffer
){
    struct usb_transfer *xfer;
    error_t err = 0;

    if (buffer->length != zone->led_count * 3)
        return -EINVAL;

    xfer = usb_transfer_alloc(zone->ctrl->usb_ctrl);
    if (IS_ERR(xfer))
        return PTR_ERR(xfer);

    if ((err = transfer_add_direct(
        xfer,
        zone,
        PACKET_CMD_DIRECT,
        buffer->data,
        3,
        zone->led_count)
    ))
        goto error_free;

    if ((err = usb_send_transfer(zone->ctrl->usb_ctrl, xfer)))
        goto error_free;

error_free:
    usb_transfer_put(xfer);

    return err;
}


static error_t aura_header_color_read (
    void *data,
    struct lights_io *io
){
    struct aura_header_zone *zone = data;

    if (WARN_ON(NULL == data || NULL == io || io->type != LIGHTS_TYPE_COLOR))
        return -EINVAL;

    spin_lock_irq(&zone->lock);
    io->data.color = zone->effect.color;
    spin_unlock_irq(&zone->lock);

    return 0;
}

static error_t aura_header_color_write (
    void *data,
    const struct lights_io *io
){
    struct aura_header_zone *zone = data;

    if (WARN_ON(NULL == data || NULL == io || io->type != LIGHTS_TYPE_COLOR))
        return -EINVAL;

    return zone_set_color(zone, &io->data.color);
}

static error_t aura_header_color_update (
    const struct lights_state *state
){
    struct aura_header_controller *ctrl = aura_header_ctrl;
    int i;
    error_t err = 0;

    if (ctrl) {
        for (i = 0; i < ctrl->zone_count; i++) {
            if ((err = zone_set_color(&ctrl->zones[i], &state->color)))
                return err;
        }
    } else {
        err = -ENODEV;
    }

    return err;
}

static error_t aura_header_mode_read (
    void *data,
    struct lights_io *io
){
    struct aura_header_zone *zone = data;

    if (WARN_ON(NULL == data || NULL == io || io->type != LIGHTS_TYPE_MODE))
        return -EINVAL;

    spin_lock_irq(&zone->lock);
    io->data.mode = zone->effect.mode;
    spin_unlock_irq(&zone->lock);

    return 0;
}

static error_t aura_header_mode_write (
    void *data,
    const struct lights_io *io
){
    struct aura_header_zone *zone = data;

    if (WARN_ON(NULL == data || NULL == io || io->type != LIGHTS_TYPE_MODE))
        return -EINVAL;

    return zone_set_mode(zone, &io->data.mode);
}

static error_t aura_header_mode_update (
    const struct lights_state *state
){
    struct aura_header_controller *ctrl = aura_header_ctrl;
    int i;
    error_t err = 0;

    if (ctrl) {
        for (i = 0; i < ctrl->zone_count; i++) {
            if ((err = zone_set_mode(&ctrl->zones[i], &state->mode)))
                return err;
        }
    } else {
        err = -ENODEV;
    }

    return err;
}

static error_t aura_header_speed_read (
    void *data,
    struct lights_io *io
){
    struct aura_header_zone *zone = data;

    if (WARN_ON(NULL == data || NULL == io || io->type != LIGHTS_TYPE_SPEED))
        return -EINVAL;

    spin_lock_irq(&zone->lock);
    io->data.speed = zone->effect.speed;
    spin_unlock_irq(&zone->lock);

    return 0;
}

static error_t aura_header_speed_write (
    void *data,
    const struct lights_io *io
){
    struct aura_header_zone *zone = data;

    if (WARN_ON(NULL == data || NULL == io || io->type != LIGHTS_TYPE_SPEED))
        return -EINVAL;

    return zone_set_speed(zone, io->data.speed);
}

static error_t aura_header_speed_update (
    const struct lights_state *state
){
    struct aura_header_controller *ctrl = aura_header_ctrl;
    int i;
    error_t err = 0;

    if (ctrl) {
        for (i = 0; i < ctrl->zone_count; i++) {
            if ((err = zone_set_speed(&ctrl->zones[i], state->speed)))
                return err;
        }
    } else {
        err = -ENODEV;
    }

    return err;
}

static error_t aura_header_direction_read (
    void *data,
    struct lights_io *io
){
    struct aura_header_zone *zone = data;

    if (WARN_ON(NULL == data || NULL == io || io->type != LIGHTS_TYPE_CUSTOM))
        return -EINVAL;

    if (io->data.raw.offset == 1) {
        io->data.raw.length = 0;
    } else {
        spin_lock_irq(&zone->lock);
        io->data.raw.data[0] = zone->effect.direction + '0';
        io->data.raw.length = 1;
        io->data.raw.offset = 1;
        spin_unlock_irq(&zone->lock);
    }

    return 0;
}

static error_t aura_header_direction_write (
    void *data,
    const struct lights_io *io
){
    struct aura_header_zone *zone = data;

    if (WARN_ON(NULL == data || NULL == io || io->type != LIGHTS_TYPE_CUSTOM))
        return -EINVAL;

    if (io->data.raw.length) {
        switch (io->data.raw.data[0]) {
            case '0':
            case '1':
                return zone_set_direction(zone, io->data.raw.data[0] - '0');
        }
    }

    return -EINVAL;
}

static error_t aura_header_leds_write (
    void *data,
    const struct lights_io *io
){
    struct aura_header_zone *zone = data;

    if (WARN_ON(NULL == data || NULL == io || io->type != LIGHTS_TYPE_LEDS))
        return -EINVAL;

    return zone_set_direct(zone, &io->data.raw);
}


static void aura_header_zone_release (
    struct aura_header_zone *zone
){
    lights_device_unregister(&zone->lights);
}

static error_t aura_header_zone_init (
    struct aura_header_controller *ctrl,
    uint8_t index
){
    struct aura_header_zone *zone = &ctrl->zones[index];
    error_t err;

    if (index >= MAX_HEADER_COUNT)
        return -EINVAL;

    zone->id = index;
    zone->ctrl = ctrl;
    zone->led_count = header_led_count[index];

    spin_lock_init(&zone->lock);

    snprintf(zone->name, sizeof(zone->name), "argb-strip-%d", index);

    zone->lights.led_count = zone->led_count;
    zone->lights.update_mode = aura_header_mode_update;
    zone->lights.update_color = aura_header_color_update;
    zone->lights.name = zone->name;
    zone->lights.caps = aura_header_modes;

    err = lights_device_register(&zone->lights);
    if (err)
        return err;

    /* Create the attributes */
    err = lights_create_file(&zone->lights, &LIGHTS_MODE_ATTR(
        zone,
        aura_header_mode_read,
        aura_header_mode_write
    ));
    if (err)
        goto error_release;

    err = lights_create_file(&zone->lights, &LIGHTS_COLOR_ATTR(
        zone,
        aura_header_color_read,
        aura_header_color_write
    ));
    if (err)
        goto error_release;

    err = lights_create_file(&zone->lights, &LIGHTS_SPEED_ATTR(
        zone,
        aura_header_speed_read,
        aura_header_speed_write
    ));
    if (err)
        goto error_release;

    err = lights_create_file(&zone->lights, &LIGHTS_CUSTOM_ATTR(
        "direction",
        zone,
        aura_header_direction_read,
        aura_header_direction_write
    ));
    if (err)
        goto error_release;

    err = lights_create_file(&zone->lights, &LIGHTS_LEDS_ATTR(
        zone,
        aura_header_leds_write
    ));
    if (err)
        goto error_release;

    return 0;

error_release:
    aura_header_zone_release(zone);

    return err;
}

static void aura_header_ctrl_destroy (
    struct aura_header_controller *ctrl
){
    int i;

    if (ctrl->zones && ctrl->zone_count) {
        for (i = 0; i < ctrl->zone_count; i++)
            aura_header_zone_release(&ctrl->zones[i]);
        kfree(ctrl->zones);
    }

    kfree(ctrl);
}

static error_t aura_header_ctrl_create (
    struct usb_controller *usb_ctrl
){
    struct aura_header_controller *ctrl;
    uint8_t i;
    error_t err;

    ctrl = kzalloc(sizeof(*ctrl), GFP_KERNEL);
    if (!ctrl)
        return -ENOMEM;

    ctrl->usb_ctrl = usb_ctrl;

    err = usb_get_zone_count(usb_ctrl, &ctrl->zone_count);
    if (err < 0)
        goto error_free;

    err = usb_get_name(usb_ctrl, ctrl->name, sizeof(ctrl->name));
    if (err < 0)
        goto error_free;

    err = usb_detect_oled(usb_ctrl, &ctrl->oled_capable, &ctrl->oled_type);
    if (err < 0)
        goto error_free;

    ctrl->zones = kcalloc(sizeof(*ctrl->zones), ctrl->zone_count, GFP_KERNEL);
    if (!ctrl->zones)
        return -ENOMEM;

    for (i = 0; i < ctrl->zone_count; i++) {
        err = aura_header_zone_init(ctrl, i);
        if (err)
            goto error_free;
    }

    aura_header_ctrl = ctrl;

    /* Set device and all zones into a known state */
    err = usb_reset_device(ctrl);
    if (err)
        goto error_free;

    return 0;

error_free:
    aura_header_ctrl_destroy(ctrl);

    return err;
}



error_t aura_header_probe (
    const struct lights_state *state
){
    error_t err;

    struct usb_controller *usb_ctrl = usb_controller_create();
    if (IS_ERR(usb_ctrl))
        return PTR_ERR(usb_ctrl);

    err = aura_header_ctrl_create(usb_ctrl);
    if (err)
        usb_controller_destroy(usb_ctrl);

    return err;
}

void aura_header_release (
    void
){
    if (aura_header_ctrl) {
        usb_controller_destroy(aura_header_ctrl->usb_ctrl);
        aura_header_ctrl_destroy(aura_header_ctrl);
    }

    aura_header_ctrl = NULL;
}
