// SPDX-License-Identifier: GPL-2.0
/*
 * A usb driver for the aura argb headers on asus motherboards.
 *
 * Copyright (C) 2019 Owen Parry
 *
 * Authors:
 * Owen Parry <twifty@zoho.com>
 */
#include <linux/module.h>
#include <linux/slab.h>

#include <adapter/lights-adapter.h>
#include <adapter/lights-interface.h>
#include <aura/debug.h>

static uint16_t header_led_count[5] = {60, 60, 60, 60, 60};

module_param_array(header_led_count, short, NULL, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(header_led_count, "An array of numbers representing the count of leds on each header.");

static inline struct aura_header_controller *aura_header_controller_get (
    void
);
static inline void aura_header_controller_put (
    struct aura_header_controller *ctrl
);

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

__used
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
    PACKET_SIZE         = 65,
    PACKET_RAW_SIZE     = PACKET_SIZE - 2,
    PACKET_DIRECT_SIZE  = PACKET_SIZE - 5,
    PACKET_LED_COUNT    = 0x14,
    PACKET_LED_SIZE     = PACKET_LED_COUNT * 3,

    MAX_SPEED_VALUE     = 5,
    MAX_HEADER_COUNT    = 5,
};

enum HEADER_CONTROL {
    MSG_CMD_DISABLE         = 0x00,
    MSG_CMD_ENABLE          = 0x01,

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

#define packet_cast(_msg) ( \
    (void*)(_msg)->data.block \
)

#define packet_init(_msg, _cmd) \
({                                      \
    compiletime_assert(                 \
        (_msg)->length == PACKET_SIZE,  \
        "Incorrect packet size"         \
    );                                  \
    memcpy(                             \
        (_msg)->data.block,             \
        &(struct packet_data){          \
            .control = PACKET_CONTROL,  \
            .command = (_cmd),          \
        },                              \
        PACKET_SIZE                     \
    );                                  \
    packet_cast(_msg);                  \
})

#define packet_dump(msg, p) \
({                          \
    print_hex_dump_bytes(   \
        msg,                \
        DUMP_PREFIX_NONE,   \
        p,                  \
        PACKET_SIZE );      \
})

/**
 * struct aura_effect - All configurable settings
 *
 * @color:     Applied color
 * @mode:      Applied mode
 * @speed:     Applied speed
 * @direction: Applied direction
 */
struct aura_effect {
    struct lights_color color;
    struct lights_mode  mode;
    uint8_t             speed;
    uint8_t             direction;
};

#define effect_dump(_msg, _effect) ( \
    AURA_DBG( \
        "%s Mode: '%s', Color: 0x%06x, Speed: 0x%02x, Direction: %d", \
        (_msg), \
        (_effect)->mode.name, \
        (_effect)->color.value, \
        (_effect)->speed, \
        (_effect)->direction \
    ) \
)

/**
 * struct aura_header_zone - Single zone configuration
 *
 * @ctrl:        Owning controller
 * @lights:      Userland access
 * @effect:      Current settings
 * @msg_buffer:  Buffer for multi packet transfer
 * @effect_lock: Lock for reading/writing pplied effect
 * @buffer_lock: Lock for @msg_buffer
 * @led_count:   Number of LEDs configured for this zone
 * @name:        Name of the zone (argb-strip-X)
 * @id:          Zero based index of the zone
 */
struct aura_header_zone {
    struct aura_header_controller   *ctrl;

    struct lights_dev               lights;
    struct aura_effect              effect;
    struct lights_adapter_msg       *msg_buffer;

    spinlock_t                      effect_lock;
    spinlock_t                      buffer_lock;

    uint16_t                        led_count;
    char                            name[16]; // "argb-strip-00"
    uint8_t                         id;
};

/**
 * struct aura_header_controller - Storage for multiple zones
 *
 * @refs:         Reference counter
 * @oled_capable: Flag to indicate if USB is an oled screen
 * @oled_type:    Type of oled screen
 * @zone_count:   Number of zones
 * @zones:        Array of zones
 * @name:         Name of the controller, determined by device.
 */
struct aura_header_controller {
    // struct lights_adapter_client    *lights_client;
    struct kref                     refs;

    bool                            oled_capable;
    uint8_t                         oled_type;

    uint8_t                         zone_count;
    struct aura_header_zone         *zones;
    char                            name[PACKET_RAW_SIZE];
};

/**
 * struct aura_header_container - Global values
 *
 * @worker:    Job to execute when disconnect is detected
 * @client:    Access point to the device
 * @ctrl:      Controller of the device
 * @lock:      Access lock
 * @connected: Flag to indicate if connected/disconnected
 */
struct aura_header_container {
    struct delayed_work             worker;
    struct lights_adapter_client    client;
    struct aura_header_controller   *ctrl;
    spinlock_t                      lock;
    bool                            connected;
};

/**
 * @driver_name:  - Name of usb driver
 *
 * If this name is changed, the udev rule for the device ids
 * must also be updated otherwise when the device reconnects
 * it will not be bound correctly.
 */
static const char *driver_name = "aura-argb-headers";
static const struct usb_device_id device_ids[] = {
    { USB_DEVICE(0x0b05, 0x1867) },
    { USB_DEVICE(0x0b05, 0x1872) },
    { } /* Terminating entry */
};

static struct aura_header_container global;

static const struct aura_effect effect_direct = {
    .mode = aura_header_modes[INDEX_MODE_DIRECT]
};

static const struct aura_effect effect_default = {
    .mode = aura_header_modes[AURA_MODE_RAINBOW]
};

static inline void zone_get_effect (
    struct aura_header_zone *zone,
    struct aura_effect *effect
){
    spin_lock(&zone->effect_lock);
    memcpy(effect, &zone->effect, sizeof(*effect));
    spin_unlock(&zone->effect_lock);
}

static inline void zone_set_effect (
    struct aura_header_zone *zone,
    const struct aura_effect *effect
){
    spin_lock(&zone->effect_lock);
    memcpy(&zone->effect, effect, sizeof(*effect));
    spin_unlock(&zone->effect_lock);
}

/**
 * usb_get_zone_count() - Fetches the number of available zones from device
 *
 * @zone_count: Buffer to write to
 *
 * @return: Error code
 *
 * The device returns an offset into a pre-calculated array of zone sizes.
 * Each zone represents a single argb header on the board.
 */
static int usb_get_zone_count (
    uint8_t *zone_count
){
    const uint8_t map[0x1E] = {
        00, 05, 01, 05, 05, 05, 02, 05, 05, 05,
        05, 05, 05, 05, 03, 05, 05, 05, 05, 05,
        05, 05, 05, 05, 05, 05, 05, 05, 05, 05//, 04
    };
    struct lights_adapter_msg msg = ADAPTER_READ_BLOCK_DATA(MSG_CMD_ENABLE, PACKET_SIZE);
    struct packet_data *packet;
    int count;
    error_t err = -EIO;

    packet = packet_init(&msg, PACKET_CMD_CAPS | PACKET_CMD_READ);

    // packet_dump("Packet: ", packet);

    err = lights_adapter_xfer(&global.client, &msg, 1);
    if (err) {
        AURA_DBG("read failed with %d", err);
        return err;
    }

    if (packet->command != PACKET_CMD_CAPS) {
        AURA_DBG("Unexpected reply while handshaking");
        packet_dump("PACKET_CMD_CAPS: ", packet);

        return -EBADMSG;
    }

    count = packet->data.raw[5] - 1;
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

/**
 * usb_get_name() - Reads the chpset name from the device
 *
 * @name: Output buffer
 * @len:  Length of @name
 *
 * @return: Error code
 */
static error_t usb_get_name (
    char *name,
    size_t len
){
    struct lights_adapter_msg msg = ADAPTER_READ_BLOCK_DATA(MSG_CMD_ENABLE, PACKET_SIZE);
    struct packet_data *packet;
    error_t err;

    packet = packet_init(&msg, PACKET_CMD_READ | PACKET_CMD_NAME);

    err = lights_adapter_xfer(&global.client, &msg, 1);
    if (err) {
        AURA_DBG("read failed with %d", err);
        return err;
    }

    if (packet->command != PACKET_CMD_NAME) {
        AURA_DBG("Unexpected reply while handshaking");
        packet_dump("0xB0 packet: ", packet);

        return -EBADMSG;
    }

    memcpy(name, packet->data.raw, min_t(size_t, len, PACKET_RAW_SIZE));

    AURA_INFO("Discovered aura controller '%s'", name);

    return 0;
}

/**
 * usb_detect_oled() - Checks if device is an oled screen
 *
 * @oled_capable: Output flag
 * @oled_type:    Output type
 *
 * @return: Error code
 *
 * NOTE - This needs testing. (ROG MAXIMUS XI EXTREME/FORMULA)
 */
static error_t usb_detect_oled (
    bool *oled_capable,
    uint8_t *oled_type
){
    struct lights_adapter_msg msg = ADAPTER_READ_BLOCK_DATA(MSG_CMD_ENABLE, PACKET_SIZE);
    struct packet_data *packet;
    error_t err;

    packet = packet_init(&msg, PACKET_CMD_READ | PACKET_CMD_OLED_CAPS);

    err = lights_adapter_xfer(&global.client, &msg, 1);
    if (err) {
        AURA_DBG("read failed with %d", err);
        return err;
    }

    if (packet->command != PACKET_CMD_OLED_CAPS) {
        AURA_DBG("Unexpected reply while handshaking");
        packet_dump("0x30 packet: ", packet);

        return -EBADMSG;
    }

    *oled_capable = packet->data.raw[0];
    *oled_type = packet->data.raw[2];

    AURA_INFO(
        "Oled capable: %s, type: %d",
        *oled_capable ? "true" : "false",
        *oled_type
    );

    return 0;
}

/**
 * usb_device_reset() - Resets device to default state
 *
 * @ctrl: Owning controller
 *
 * @return: Error code
 *
 * Sending this packet on a freshly booted system will cause the
 * device to reconnect. Upon its return it may have a different devnum.
 * The udev rule needs to be in place for the newly connected device
 * to be bound to this driver.
 */
static error_t usb_device_reset (
    struct aura_header_controller *ctrl
){
    struct lights_adapter_msg msg = ADAPTER_WRITE_BLOCK_DATA(MSG_CMD_ENABLE, PACKET_SIZE);
    struct packet_data *packet;
    error_t err;
    int i;

    packet = packet_init(&msg, PACKET_CMD_RESET);
    packet->data.raw[0] = 0xAA;

    err = lights_adapter_xfer(&global.client, &msg, 1);
    if (err) {
        AURA_DBG("usb_write_packet() failed with %s", ERR_NAME(err));
        return err;
    }

    for (i = 0; i < ctrl->zone_count; i++)
        zone_set_effect(&ctrl->zones[i], &effect_default);

    return err;
}

uint8_t aura_speeds[] = {0xFF, 0xCC, 0x99, 0x66, 0x33, 0x00};

/**
 * transfer_add_effect() - Creates an apply effect packet
 *
 * @msg:    Target message
 * @zone:   Zone being written
 * @effect: Settings to apply
 *
 * @return: Number of packets created
 */
static int transfer_add_effect (
    struct lights_adapter_msg *msg,
    struct aura_header_zone *zone,
    const struct aura_effect *effect
){
    /*
        The speed given should be an int between 0 (slowest) and 5 (fastest)
     */
    struct packet_data *packet;

    *msg = ADAPTER_WRITE_BLOCK_DATA(MSG_CMD_ENABLE, PACKET_SIZE);
    packet = packet_init(msg, PACKET_CMD_EFFECT);

    packet->data.effect.header       = zone->id;
    packet->data.effect.mode         = get_aura_mode(&effect->mode);
    packet->data.effect.red          = effect->color.r;
    packet->data.effect.green        = effect->color.g;
    packet->data.effect.blue         = effect->color.b;
    packet->data.effect.direction    = effect->direction;
    packet->data.effect.speed        = aura_speeds[effect->speed];

    return 1;
}

/**
 * transfer_add_direct() - Creates packets to update all LEDs
 *
 * @msg:        Target Message array
 * @zone:       Zone being updated
 * @command:    Packet command byte
 * @data:       Array of values to send
 * @data_size:  Size of each @data element
 * @data_count: Number of @data elements
 *
 * @return: Number of packets created
 */
static int transfer_add_direct (
    struct lights_adapter_msg *msg,
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
        msg[curr_loop] = ADAPTER_WRITE_BLOCK_DATA(MSG_CMD_ENABLE, PACKET_SIZE);
        packet = packet_init(&msg[curr_loop], command);

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

    return max_loops;
}

/**
 * transfer_add_enable() - Creates a packet to enable/disable device
 *
 * @msg:    Target message
 * @zone:   Zone being updated
 * @enable: Enable/Disable flag
 *
 * @return: Number of packets created
 */
static int transfer_add_enable (
    struct lights_adapter_msg *msg,
    struct aura_header_zone *zone,
    bool enable
){
    /*
        This packet actually pauses the leds (when on rainbow)
     */
    struct packet_data *packet;

    *msg = ADAPTER_WRITE_BLOCK_DATA(MSG_CMD_ENABLE, PACKET_SIZE);
    packet = packet_init(msg, PACKET_CMD_ENABLE);

    packet->data.raw[0] = zone->id;
    packet->data.raw[1] = (uint8_t)enable;

    return 1;
}

/**
 * zone_set_callback() - Async completion handler
 *
 * @result: Packets sent
 * @data:   Context
 * @error:  Error encountered while transfering data
 */
static void zone_set_callback (
    struct lights_adapter_msg const * const result,
    void *data,
    error_t error
){
    struct aura_header_zone *zone = data;
    const struct lights_adapter_msg *iter = result;
    const struct packet_data *packet;
    const struct lights_mode *mode;
    struct aura_effect effect = {0};
    int i;

    if (error) {
        AURA_DBG("Failed to apply update: %s", ERR_NAME(error));
        return;
    }

    packet = packet_cast(iter);

    if (PACKET_CMD_ENABLE == packet->command) {
        iter = packet_cast(iter->next);
        if (!iter) {
            AURA_ERR("Expected second message following 'PACKET_CMD_ENABLE'");
            return;
        }
        packet = packet_cast(iter);
    }

    if (PACKET_CMD_EFFECT == packet->command) {
        if (MSG_CMD_DISABLE == iter->command)
            mode = get_lights_mode(AURA_MODE_OFF);
        else
            mode = get_lights_mode(packet->data.effect.mode);

        if (!mode) {
            AURA_ERR("Message contains an invalid mode: 0x%02x", packet->data.effect.mode);
            return;
        }

        for (i = 0; i < ARRAY_SIZE(aura_speeds); i++) {
            if (packet->data.effect.speed + 0x1A > aura_speeds[i]) {
                effect.speed = i;
                break;
            }
        }

        effect.mode      = *mode;
        effect.direction = packet->data.effect.direction;
        effect.color.r   = packet->data.effect.red;
        effect.color.g   = packet->data.effect.green;
        effect.color.b   = packet->data.effect.blue;

        effect_dump("Applying effect: ", &effect);
        zone_set_effect(zone, &effect);
    }
}

/**
 * zone_set_mode() - Applies a mode to a zone
 *
 * @zone: Zone to update
 * @mode: Mode to apply
 *
 * @return: Error code
 */
static error_t zone_set_mode (
    struct aura_header_zone *zone,
    const struct lights_mode *mode
){
    enum aura_header_mode header_mode;
    struct aura_effect effect;
    error_t err;
    int count = 0;

    err = get_header_mode(mode, &header_mode);
    if (err)
        return err;

    zone_get_effect(zone, &effect);

    if (header_mode == get_aura_mode(&effect.mode))
        return 0;

    spin_lock(&zone->buffer_lock);

    switch (header_mode) {
        case AURA_MODE_OFF:
        case AURA_MODE_DIRECT:
            count += transfer_add_effect(&zone->msg_buffer[count], zone, &effect_direct);
            count += transfer_add_direct(&zone->msg_buffer[count], zone, PACKET_CMD_DIRECT, NULL, 3, zone->led_count);

            /*
             * The command field is not used by the usb driver, so we can use
             * it as a flag. Here we let the callback know the mode is OFF.
             */
            if (AURA_MODE_OFF == header_mode)
                zone->msg_buffer[0].command = MSG_CMD_DISABLE;
            break;

        default:
            if (AURA_MODE_OFF == get_aura_mode(&effect.mode))
                count += transfer_add_enable(&zone->msg_buffer[count], zone, true);

            effect.mode = *mode;
            count += transfer_add_effect(&zone->msg_buffer[count], zone, &effect);
            break;
    }

    err = lights_adapter_xfer_async(
        &global.client,
        zone->msg_buffer,
        count,
        zone,
        zone_set_callback
    );

    spin_unlock(&zone->buffer_lock);

    return err;
}

/**
 * zone_set_color() - Applies a color to a zone
 *
 * @zone:  Zone to update
 * @color: Color to apply
 *
 * @return: Error code
 */
static error_t zone_set_color (
    struct aura_header_zone *zone,
    const struct lights_color *color
){
    struct lights_adapter_msg msg;
    struct aura_effect effect;
    error_t err = 0;

    zone_get_effect(zone, &effect);

    if (lights_color_equal(color, &effect.color))
        return 0;

    effect.color = *color;
    transfer_add_effect(&msg, zone, &effect);

    err = lights_adapter_xfer_async(
        &global.client,
        &msg,
        1,
        zone,
        zone_set_callback
    );

    return err;
}

/**
 * zone_set_speed() - Applies a speed to a zone
 *
 * @zone:  Zone to update
 * @speed: Speed to apply (value between 0 and 5 inclusive)
 *
 * @return: Error code
 */
static error_t zone_set_speed (
    struct aura_header_zone *zone,
    uint8_t speed
){
    struct lights_adapter_msg msg;
    struct aura_effect effect;
    error_t err = 0;

    zone_get_effect(zone, &effect);

    if (speed > MAX_SPEED_VALUE)
        speed = 5;

    if (speed == effect.speed)
        return 0;

    effect.speed = speed;
    transfer_add_effect(&msg, zone, &effect);

    err = lights_adapter_xfer_async(
        &global.client,
        &msg,
        1,
        zone,
        zone_set_callback
    );

    return err;
}

/**
 * zone_set_direction() - Applies a direction to a zone
 *
 * @zone:      Zone to update
 * @direction: 0 or 1 indicating direction
 *
 * @return: Error code
 */
static error_t zone_set_direction (
    struct aura_header_zone *zone,
    uint8_t direction
){
    struct lights_adapter_msg msg;
    struct aura_effect effect;
    error_t err = 0;

    zone_get_effect(zone, &effect);

    if (direction > 1)
        direction = 1;
    if (direction == effect.direction)
        return 0;

    effect.direction = direction;
    transfer_add_effect(&msg, zone, &effect);

    err = lights_adapter_xfer_async(
        &global.client,
        &msg,
        1,
        zone,
        zone_set_callback
    );

    return err;
}

/**
 * zone_set_direct() - Applies color to all LEDs in a zone
 *
 * @zone:   Zone to update
 * @buffer: Array of RGB triplets
 *
 * @return: Error code
 */
static error_t zone_set_direct (
    struct aura_header_zone *zone,
    const struct lights_buffer *buffer
){
    error_t err = 0;
    int count;

    if (buffer->length != zone->led_count * 3)
        return -EINVAL;

    spin_lock(&zone->buffer_lock);

    count = transfer_add_direct(
        zone->msg_buffer,
        zone,
        PACKET_CMD_DIRECT,
        buffer->data,
        3,
        zone->led_count
    );

    err = lights_adapter_xfer_async(
        &global.client,
        zone->msg_buffer,
        count,
        zone,
        zone_set_callback
    );

    spin_unlock(&zone->buffer_lock);

    return err;
}

/**
 * aura_header_color_read() - Reads a zones color
 *
 * @data: Zone to read
 * @io:   Buffer to write value
 *
 * @return: Error code
 */
static error_t aura_header_color_read (
    void *data,
    struct lights_io *io
){
    struct aura_header_zone *zone = data;

    if (IS_NULL(data, io) || IS_FALSE(io->type == LIGHTS_TYPE_COLOR))
        return -EINVAL;

    spin_lock(&zone->effect_lock);
    io->data.color = zone->effect.color;
    spin_unlock(&zone->effect_lock);

    return 0;
}

/**
 * aura_header_color_write() - Writes a zones color
 *
 * @data: Zone to write
 * @io:   Buffer containing new value
 *
 * @return: Error code
 */
static error_t aura_header_color_write (
    void *data,
    const struct lights_io *io
){
    struct aura_header_zone *zone = data;

    if (IS_NULL(data, io) || IS_FALSE(io->type == LIGHTS_TYPE_COLOR))
        return -EINVAL;

    return zone_set_color(zone, &io->data.color);
}

/**
 * aura_header_color_update() - Writes all zones color
 *
 * @state: Buffer containing color
 *
 * @return: Error code
 */
static error_t aura_header_color_update (
    const struct lights_state *state
){
    struct aura_header_controller *ctrl = aura_header_controller_get();
    int i;
    error_t err = 0;

    if (ctrl) {
        for (i = 0; i < ctrl->zone_count; i++) {
            if ((err = zone_set_color(&ctrl->zones[i], &state->color)))
                break;
        }

        aura_header_controller_put(ctrl);
    } else {
        err = -ENODEV;
    }

    return err;
}

/**
 * aura_header_mode_read() - Reads a zones mode
 *
 * @data: Zone to read
 * @io:   Buffer to write value
 *
 * @return: Error code
 */
static error_t aura_header_mode_read (
    void *data,
    struct lights_io *io
){
    struct aura_header_zone *zone = data;

    if (IS_NULL(data, io) || IS_FALSE(io->type == LIGHTS_TYPE_MODE))
        return -EINVAL;

    spin_lock(&zone->effect_lock);
    io->data.mode = zone->effect.mode;
    spin_unlock(&zone->effect_lock);

    return 0;
}

/**
 * aura_header_mode_write() - Writes a zones mode
 *
 * @data: Zone to write
 * @io:   Buffer containing new value
 *
 * @return: Error code
 */
static error_t aura_header_mode_write (
    void *data,
    const struct lights_io *io
){
    struct aura_header_zone *zone = data;

    if (IS_NULL(data, io) || IS_FALSE(io->type == LIGHTS_TYPE_MODE))
        return -EINVAL;

    return zone_set_mode(zone, &io->data.mode);
}

/**
 * aura_header_mode_update() - Writes a mode to all zones
 *
 * @state: Buffer containing mode
 *
 * @return: Error code
 */
static error_t aura_header_mode_update (
    const struct lights_state *state
){
    struct aura_header_controller *ctrl = aura_header_controller_get();
    int i;
    error_t err = 0;

    if (ctrl) {
        for (i = 0; i < ctrl->zone_count; i++) {
            if ((err = zone_set_mode(&ctrl->zones[i], &state->mode)))
                break;
        }

        aura_header_controller_put(ctrl);
    } else {
        err = -ENODEV;
    }

    return err;
}

/**
 * aura_header_speed_read() - Reads a zones speed
 *
 * @data: Zone to read
 * @io:   Buffer to write value
 *
 * @return: Error code
 */
static error_t aura_header_speed_read (
    void *data,
    struct lights_io *io
){
    struct aura_header_zone *zone = data;

    if (IS_NULL(data, io) || IS_FALSE(io->type == LIGHTS_TYPE_SPEED))
        return -EINVAL;

    spin_lock(&zone->effect_lock);
    io->data.speed = zone->effect.speed;
    spin_unlock(&zone->effect_lock);

    return 0;
}

/**
 * aura_header_speed_write() - Writes a zones speed
 *
 * @data: Zone to write
 * @io:   Buffer containing new value
 *
 * @return: Error code
 */
static error_t aura_header_speed_write (
    void *data,
    const struct lights_io *io
){
    struct aura_header_zone *zone = data;

    if (IS_NULL(data, io) || IS_FALSE(io->type == LIGHTS_TYPE_SPEED))
        return -EINVAL;

    return zone_set_speed(zone, io->data.speed);
}

/**
 * aura_header_speed_update() - Writes a speed to all zones
 *
 * @state: Buffer containing color
 *
 * @return: Error code
 */
 __used
static error_t aura_header_speed_update (
    const struct lights_state *state
){
    struct aura_header_controller *ctrl = aura_header_controller_get();
    int i;
    error_t err = 0;

    if (ctrl) {
        for (i = 0; i < ctrl->zone_count; i++) {
            if ((err = zone_set_speed(&ctrl->zones[i], state->speed)))
                break;
        }

        aura_header_controller_put(ctrl);
    } else {
        err = -ENODEV;
    }

    return err;
}

/**
 * aura_header_direction_read() - Reads a zones direction
 *
 * @data: Zone to read
 * @io:   Buffer to write value
 *
 * @return: Error code
 */
static error_t aura_header_direction_read (
    void *data,
    struct lights_io *io
){
    struct aura_header_zone *zone = data;

    if (IS_NULL(data, io) || IS_FALSE(io->type == LIGHTS_TYPE_CUSTOM))
        return -EINVAL;

    if (io->data.raw.offset == 1) {
        io->data.raw.length = 0;
    } else {
        spin_lock(&zone->effect_lock);
        io->data.raw.data[0] = zone->effect.direction + '0';
        io->data.raw.length = 1;
        io->data.raw.offset = 1;
        spin_unlock(&zone->effect_lock);
    }

    return 0;
}

/**
 * aura_header_direction_write() - Writes a zones direction
 *
 * @data: Zone to write
 * @io:   Buffer containing new value
 *
 * @return: Error code
 */
static error_t aura_header_direction_write (
    void *data,
    const struct lights_io *io
){
    struct aura_header_zone *zone = data;

    if (IS_NULL(data, io) || IS_FALSE(io->type == LIGHTS_TYPE_CUSTOM))
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

/**
 * aura_header_leds_write() - Writes raw RGB values to a zone
 *
 * @data: Zone to update
 * @io:   Buffer containing RGB values
 *
 * @return: Error code
 */
static error_t aura_header_leds_write (
    void *data,
    const struct lights_io *io
){
    struct aura_header_zone *zone = data;

    if (IS_NULL(data, io) || IS_FALSE(io->type == LIGHTS_TYPE_LEDS))
        return -EINVAL;

    return zone_set_direct(zone, &io->data.raw);
}


/**
 * aura_header_zone_release() - Releases memory contained within a zone
 *
 * @zone: Zone being freed
 */
static void aura_header_zone_release (
    struct aura_header_zone *zone
){
    lights_device_unregister(&zone->lights);

    kfree(zone->msg_buffer);
    zone->msg_buffer = NULL;
}

/**
 * aura_header_zone_init() - Initializes a zones values
 *
 * @ctrl:  Owning controller
 * @index: Zone offset
 *
 * @return: Error code
 */
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

    /* 20 leds per packet, plus one additional */
    zone->msg_buffer = kmalloc_array(
        (zone->led_count / PACKET_LED_COUNT) + 2,
        sizeof(*zone->msg_buffer),
        GFP_KERNEL
    );
    if (!zone->msg_buffer)
        return -ENOMEM;

    spin_lock_init(&zone->effect_lock);
    spin_lock_init(&zone->buffer_lock);

    snprintf(zone->name, sizeof(zone->name), "argb-strip-%d", index);
    AURA_DBG("Creating sysfs for '%s'", zone->name);

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
        goto error_out;

    err = lights_create_file(&zone->lights, &LIGHTS_COLOR_ATTR(
        zone,
        aura_header_color_read,
        aura_header_color_write
    ));
    if (err)
        goto error_out;

    err = lights_create_file(&zone->lights, &LIGHTS_SPEED_ATTR(
        zone,
        aura_header_speed_read,
        aura_header_speed_write
    ));
    if (err)
        goto error_out;

    err = lights_create_file(&zone->lights, &LIGHTS_CUSTOM_ATTR(
        "direction",
        zone,
        aura_header_direction_read,
        aura_header_direction_write
    ));
    if (err)
        goto error_out;

    err = lights_create_file(&zone->lights, &LIGHTS_LEDS_ATTR(
        zone,
        aura_header_leds_write
    ));

error_out:
    return err;
}

/**
 * aura_header_controller_destroy() - Destroys a controller and all zones
 *
 * @ctrl: Controller to destroy
 */
static void aura_header_controller_destroy (
    struct aura_header_controller *ctrl
){
    int i;

    if (ctrl->zones && ctrl->zone_count) {
        for (i = 0; i < ctrl->zone_count; i++)
            aura_header_zone_release(&ctrl->zones[i]);
        kfree(ctrl->zones);
    }

    AURA_DBG("Destroyed AURA header controller");

    kfree(ctrl);
}

/**
 * aura_header_controller_create() - Creates a controller and all zones
 *
 * @return: Error code or created controller
 */
static struct aura_header_controller *aura_header_controller_create (
    void
){
    struct aura_header_controller *ctrl;
    uint8_t i;
    error_t err;

    ctrl = kzalloc(sizeof(*ctrl), GFP_KERNEL);
    if (!ctrl)
        return ERR_PTR(-ENOMEM);

    kref_init(&ctrl->refs);

    err = usb_get_zone_count(&ctrl->zone_count);
    if (err < 0)
        goto error_free;

    err = usb_get_name(ctrl->name, sizeof(ctrl->name));
    if (err < 0)
        goto error_free;

    err = usb_detect_oled(&ctrl->oled_capable, &ctrl->oled_type);
    if (err < 0)
        goto error_free;

    ctrl->zones = kcalloc(ctrl->zone_count, sizeof(struct aura_header_zone), GFP_KERNEL);
    if (!ctrl->zones)
        goto error_free;

    for (i = 0; i < ctrl->zone_count; i++) {
        err = aura_header_zone_init(ctrl, i);
        if (err)
            goto error_free;
    }

    /* Set device and all zones into a known state */
    err = usb_device_reset(ctrl);
    if (err)
        goto error_free;

    AURA_DBG("Created AURA header controller");

    return ctrl;

error_free:
    aura_header_controller_destroy(ctrl);

    return ERR_PTR(err);
}

/**
 * aura_header_controller_get() - Fetches a reference counted handle
 *
 * @return: Controller
 */
static inline struct aura_header_controller *aura_header_controller_get (
    void
){
    struct aura_header_controller *ctrl = NULL;

    spin_lock(&global.lock);
    if (global.connected && global.ctrl) {
        ctrl = global.ctrl;
        kref_get(&ctrl->refs);
    }
    spin_unlock(&global.lock);

    return ctrl;
}

/**
 * aura_header_controller_put_kref() - kref_put callback
 *
 * @ref: Reference counter
 */
static void aura_header_controller_put_kref (
    struct kref *ref
){
    struct aura_header_controller *ctrl = container_of(ref, struct aura_header_controller, refs);

    aura_header_controller_destroy(ctrl);
}

/**
 * aura_header_controller_put() - Returns a reference counted handle
 *
 * @ctrl: Controller
 */
static inline void aura_header_controller_put (
    struct aura_header_controller *ctrl
){
    kref_put(&ctrl->refs, aura_header_controller_put_kref);
}

/**
 * aura_header_driver_on_connect() - Device connection callback
 *
 * @client: Registered USB client
 */
static void aura_header_driver_on_connect (
    struct usb_client *client
){
    bool create = false;

    spin_lock(&global.lock);
    if (!global.connected) {
        create = !global.ctrl;
        global.connected = true;
    }
    spin_unlock(&global.lock);

    if (create) {
        global.ctrl = aura_header_controller_create();
        if (IS_ERR(global.ctrl)) {
            CLEAR_ERR(global.ctrl);
            global.connected = false;
        }
    }
}

/**
 * aura_header_driver_work_callback() - Destroyes a disconnected controller
 *
 * @work: Delayed work job
 */
static void aura_header_driver_work_callback (
    struct work_struct *work
){
    struct aura_header_controller *ctrl = NULL;

    spin_lock(&global.lock);
    if (false == global.connected) {
        ctrl = global.ctrl;
        global.ctrl = NULL;
    }
    spin_unlock(&global.lock);

    if (ctrl)
        aura_header_controller_put(ctrl);
}

/**
 * aura_header_driver_on_disconnect() - Device disconnect callback
 *
 * @client: Registered USB client
 *
 * The controller is destroyed after a 5 second delay within a worker thread.
 * If a device is reconnected within this time frame, the controller is reused.
 */
static void aura_header_driver_on_disconnect (
    struct usb_client *client
){
    bool schedule = false;

    spin_lock(&global.lock);
    if (true == global.connected) {
        global.connected = false;
        schedule = true;
    }
    spin_unlock(&global.lock);

    if (schedule)
        schedule_delayed_work(&global.worker, 5 * HZ);
}

/**
 * aura_header_probe() - Entry point
 *
 * @state: Initial state of all zones
 *
 * @return: Error code
 */
error_t aura_header_probe (
    const struct lights_state *state
){
    struct usb_client usb = {
        .name = driver_name,
        .packet_size = PACKET_SIZE,
        .ids = device_ids,
        .onConnect = aura_header_driver_on_connect,
        .onDisconnect = aura_header_driver_on_disconnect,
    };

    LIGHTS_USB_CLIENT_INIT(&global.client, &usb);
    INIT_DELAYED_WORK(&global.worker, aura_header_driver_work_callback);
    spin_lock_init(&global.lock);
    global.ctrl = NULL;
    global.connected = false;

    return lights_adapter_register(&global.client, 32);
}

/**
 * aura_header_release() - Exit point
 */
void aura_header_release (
    void
){
    struct aura_header_controller *ctrl = NULL;

    spin_lock(&global.lock);
    global.connected = false;
    ctrl = global.ctrl;
    global.ctrl = NULL;
    spin_unlock(&global.lock);

    if (ctrl)
        aura_header_controller_put(ctrl);

    lights_adapter_unregister(&global.client);
}
