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
static inline int aura_header_controller_put (
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

static struct lights_mode const aura_header_modes[] = {
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
    struct lights_mode const *mode,
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
    struct lights_mode const *mode
){
    enum aura_header_mode aura_mode;

    get_header_mode(mode, &aura_mode);

    return (uint8_t)aura_mode;
}

static struct lights_mode const *get_lights_mode (
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
    MSG_FLAG_ENABLE         = 0x00,
    MSG_FLAG_DISABLE        = 0x01,

    PACKET_CONTROL          = 0xEC,
    PACKET_CMD_READ         = 0x80,

    PACKET_CMD_NAME         = 0x02,
    PACKET_CMD_CAPS         = 0x30,
    PACKET_CMD_ENABLE       = 0x35,
    PACKET_CMD_EFFECT       = 0x3B,
    PACKET_CMD_SYNC         = 0x3C,
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
 * @active:      Current effect
 * @pending:     Effect in the process of being written
 * @msg_buffer:  Buffer for multi packet transfer
 * @thunk:       Magic member for callbacks
 * @lock:        Lock for reading/writing effects and buffer
 * @led_count:   Number of LEDs configured for this zone
 * @name:        Name of the zone (argb-strip-X)
 * @id:          Zero based index of the zone
 */
struct aura_header_zone {
    struct aura_header_controller   *ctrl;

    struct lights_dev               lights;
    struct aura_effect              active, pending;
    struct lights_adapter_msg       *msg_buffer;
    struct lights_thunk             thunk;
    spinlock_t                      lock;

    uint16_t                        led_count;
    char                            name[16]; // "argb-strip-00"
    uint8_t                         id;
};
#define ZONE_HASH 'ZONE'
#define zone_from_thunk(ptr) ( \
    lights_thunk_container(ptr, struct aura_header_zone, thunk, ZONE_HASH) \
)

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
    struct delayed_work             connect;
    struct delayed_work             disconnect;
    struct lights_adapter_client    client;
    struct aura_header_controller   *ctrl;
    spinlock_t                      lock;
    // bool                            connected;
};

/**
 * @driver_name:  - Name of usb driver
 *
 * If this name is changed, the udev rule for the device ids
 * must also be updated otherwise when the device reconnects
 * it will not be bound correctly.
 */
static const char *driver_name = "aura-argb-headers";
static struct usb_device_id const device_ids[] = {
    { USB_DEVICE(0x0b05, 0x1867) },
    { USB_DEVICE(0x0b05, 0x1872) },
    { } /* Terminating entry */
};

static struct aura_header_container global;

static struct aura_effect const effect_direct = {
    .mode = aura_header_modes[INDEX_MODE_DIRECT]
};

static struct aura_effect const effect_off = {
    .mode = aura_header_modes[AURA_MODE_OFF]
};

static struct aura_effect const effect_default = {
    .mode = aura_header_modes[AURA_MODE_RAINBOW]
};

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
    struct lights_adapter_msg msg = ADAPTER_READ_BLOCK_DATA(MSG_FLAG_ENABLE, PACKET_SIZE);
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
    struct lights_adapter_msg msg = ADAPTER_READ_BLOCK_DATA(MSG_FLAG_ENABLE, PACKET_SIZE);
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
    struct lights_adapter_msg msg = ADAPTER_READ_BLOCK_DATA(MSG_FLAG_ENABLE, PACKET_SIZE);
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
    struct lights_adapter_msg msg = ADAPTER_WRITE_BLOCK_DATA(MSG_FLAG_ENABLE, PACKET_SIZE);
    struct packet_data *packet;
    error_t err;
    int i;

    packet = packet_init(&msg, PACKET_CMD_RESET);
    packet->data.raw[0] = 0xAA;

    err = lights_adapter_xfer(&global.client, &msg, 1);
    if (err) {
        AURA_DBG("lights_adapter_xfer() failed with %s", ERR_NAME(err));
        return err;
    }

    for (i = 0; i < ctrl->zone_count; i++) {
        spin_lock(&ctrl->zones[i].lock);

        ctrl->zones[i].active  = effect_default;
        ctrl->zones[i].pending = effect_default;

        spin_unlock(&ctrl->zones[i].lock);
    }

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
    struct aura_effect const *effect
){
    /*
        The speed given should be an int between 0 (slowest) and 5 (fastest)
     */
    struct packet_data *packet;

    *msg = ADAPTER_WRITE_BLOCK_DATA(MSG_FLAG_ENABLE, PACKET_SIZE);
    packet = packet_init(msg, PACKET_CMD_EFFECT);

    packet->data.effect.header       = zone->id;
    packet->data.effect.mode         = get_aura_mode(&effect->mode);
    packet->data.effect.red          = effect->color.r;
    packet->data.effect.green        = effect->color.g;
    packet->data.effect.blue         = effect->color.b;
    packet->data.effect.direction    = effect->direction & 0x01;
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
__used
static int _transfer_add_direct (
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
        msg[curr_loop] = ADAPTER_WRITE_BLOCK_DATA(MSG_FLAG_ENABLE, PACKET_SIZE);
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
static int transfer_add_direct (
   struct lights_adapter_msg *msg,
   struct aura_header_zone *zone,
   uint8_t command,
   struct lights_color const *colors,
   uint8_t color_count
){
   struct packet_data *packet;
   struct data_direct *direct;
   size_t bytes_to_transfer = 3 * color_count;
   size_t max_loops = (bytes_to_transfer / PACKET_DIRECT_SIZE) + 1;
   size_t max_items_per_packet;
   uint16_t src_offset = 0;
   int curr_loop, i;

   if (!color_count)
       return -EINVAL;

   max_items_per_packet = PACKET_DIRECT_SIZE / 3;

   for (curr_loop = 0; curr_loop < max_loops; curr_loop++) {
       msg[curr_loop] = ADAPTER_WRITE_BLOCK_DATA(MSG_FLAG_ENABLE, PACKET_SIZE);
       packet = packet_init(&msg[curr_loop], command);

       direct = &packet->data.direct;
       direct->flags = zone->id;

       /* NOTE - LightsService has this as only greater than */
       if (src_offset >= 0x100)
           direct->flags = (src_offset >> 8) & 0xf;

       if (curr_loop + 1 == max_loops)
           direct->flags |= 0x80;

       direct->offset = (uint8_t)src_offset;
       direct->count  = min_t(uint8_t, color_count, max_items_per_packet);

       for (i = 0; i < direct->count; i++) {
           if (colors) {
               lights_color_write_rgb(colors, &direct->value[i * 3]);
           } else {
               memset(&direct->value[i * 3], 0, 3);
           }
           // direct->value[i] = data ? ((uint8_t*)data)[src_offset] : 0;
           src_offset += 3;
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

    *msg = ADAPTER_WRITE_BLOCK_DATA(MSG_FLAG_ENABLE, PACKET_SIZE);
    packet = packet_init(msg, PACKET_CMD_ENABLE);

    packet->data.raw[0] = zone->id;
    packet->data.raw[1] = (uint8_t)enable;

    return 1;
}

/**
 * transfer_add_sync() - Creates a packet to synchronize a mode with a global
 *                       stepping value.
 *
 * @msg:    Target message
 * @zone:   Zone being synced
 * @enable: The stepping value
 *
 * @return: Number of packets created
 */
static int transfer_add_sync (
    struct lights_adapter_msg *msg,
    struct aura_header_zone *zone,
    uint8_t byte
){
    struct packet_data *packet;

    *msg = ADAPTER_WRITE_BLOCK_DATA(MSG_FLAG_ENABLE, PACKET_SIZE);
    packet = packet_init(msg, PACKET_CMD_SYNC);

    packet->data.raw[0] = zone->id;
    packet->data.raw[2] = get_aura_mode(&zone->pending.mode);
    packet->data.raw[3] = byte;

    return 1;
}


/**
 * aura_header_zone_update_callback() - Async completion handler
 *
 * @result: Packets sent
 * @thunk:  Context
 * @error:  Error encountered while transfering data
 */
static void aura_header_zone_update_callback (
    struct lights_adapter_msg const * const result,
    struct lights_thunk *thunk,
    error_t error
){
    struct aura_header_zone *zone = zone_from_thunk(thunk);
    struct lights_adapter_msg const *iter = result;
    struct packet_data const *packet;
    struct lights_mode const *mode;
    struct aura_effect effect = {0};
    bool disable = false;
    int i;

    AURA_DBG("in callback");

    if (IS_NULL(result, thunk, zone))
        return;

    if (error) {
        AURA_DBG("Failed to apply update: %s", ERR_NAME(error));
        return;
    }

    packet = packet_cast(iter);
    if (MSG_FLAG_DISABLE == lights_adapter_msg_read_flags(iter))
        disable = true;

    if (PACKET_CMD_ENABLE == packet->command) {
        iter = iter->next;
        if (!iter) {
            AURA_ERR("Expected second message following 'PACKET_CMD_ENABLE'");
            return;
        }
        packet = packet_cast(iter);
    }

    if (PACKET_CMD_EFFECT == packet->command) {
        mode = get_lights_mode(disable ? AURA_MODE_OFF : packet->data.effect.mode);

        if (disable || AURA_MODE_DIRECT == get_aura_mode(mode)) {
            AURA_DBG("Applying mode only: %s", mode->name);

            spin_lock(&zone->lock);
            zone->active.mode = *mode;
            spin_unlock(&zone->lock);
        } else {
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

            spin_lock(&zone->lock);
            zone->active = effect;
            spin_unlock(&zone->lock);
        }
    } else {
        AURA_ERR("Unexpected packet type: %x", packet->command);
        packet_dump("packet 2 post:", packet);
    }
}

/**
 * aura_header_zone_update() - Begins an async transaction
 *
 * @zone:   Zone being updated
 * @effect: Optional new effect to apply
 * @colors: Optional colors for direct mode
 *
 * @return: Error code
 *
 * This function expects a zone lock to already be in place.
 */
static error_t aura_header_zone_update (
    struct aura_header_zone *zone,
    struct aura_effect const *effect,
    struct lights_color const *colors
){
    bool update_colors = false;
    size_t count = 0;
    error_t err;
    int i;

    if (IS_NULL(zone))
        return -EINVAL;

    if (effect) {
        effect_dump("aura_header_zone_update() ", effect);

        /* If pending.mode is off, send enable */
        if (AURA_MODE_OFF == get_aura_mode(&zone->pending.mode)) {
            count += transfer_add_enable(
                &zone->msg_buffer[count],
                zone,
                true
            );
        }

        /*
         * If new mode is off, A special packet needs to be sent which applies
         * direct mode with no colors set. But, it's important that the callback
         * only updates the mode. When a direct mode is being applied, only
         * the mode should update (all other values remain as a cache).
         */
        switch (get_aura_mode(&effect->mode)) {
            case AURA_MODE_OFF:
                lights_adapter_msg_write_flags(&zone->msg_buffer[0], MSG_FLAG_DISABLE);
                // Fall-through
            case AURA_MODE_DIRECT:
                count += transfer_add_effect(
                    &zone->msg_buffer[count],
                    zone,
                    &effect_direct
                );

                update_colors = true;
                break;
            default:
                count += transfer_add_effect(
                    &zone->msg_buffer[count],
                    zone,
                    effect
                );
                break;
        }
    }

    if (colors || update_colors) {
        count += transfer_add_direct(
            &zone->msg_buffer[count],
            zone,
            PACKET_CMD_DIRECT,
            colors,
            zone->led_count
        );
    }

    if (count) {
        AURA_DBG("Transfering %ld packets", count);
        for (i = 0; i < count; i++)
            packet_dump("packet:", &zone->msg_buffer[i]);

        err = lights_adapter_xfer_async(
            &global.client,
            zone->msg_buffer,
            count,
            &zone->thunk,
            aura_header_zone_update_callback
        );
    } else {
        err = -EINVAL;
    }

    /* Update the pending effect */
    if (!err && effect)
        zone->pending = *effect;

    return err;
}

/**
 * aura_header_zone_write() - Userland write handler
 *
 * @thunk: Zone being updated
 * @state: New state to apply to the zone
 *
 * @return: Error code
 *
 * This function simply validates and sometimes fixes incorrect values
 * before calling aura_header_zone_update().
 */
static error_t aura_header_zone_write (
    struct lights_thunk *thunk,
    struct lights_state const *state
){
    struct aura_header_zone *zone = zone_from_thunk(thunk);
    struct lights_color const *colors = NULL;
    struct aura_effect effect;
    enum aura_header_mode header_mode;
    uint8_t speed, direction;
    bool update_effect = false;
    bool update_colors = false;
    error_t err;

    if (IS_NULL(thunk, state, zone))
        return -EINVAL;

    spin_lock(&zone->lock);

    effect = zone->pending;

    if (state->type & LIGHTS_TYPE_COLOR) {
        if (!lights_color_equal(&state->color, &effect.color)) {
            effect.color = state->color;
            update_effect = true;
        }
    }

    if (state->type & LIGHTS_TYPE_SPEED) {
        speed = min_t(uint8_t, state->speed, 5);

        AURA_DBG("LIGHTS_TYPE_SPEED detected: new %x old %x", speed, effect.speed);
        if (speed != effect.speed) {
            effect.speed = speed;
            update_effect = true;
        }
    }

    if (state->type & LIGHTS_TYPE_DIRECTION) {
        direction = max_t(uint8_t, state->direction, 1);

        if (direction != effect.direction) {
            effect.direction = direction;
            update_effect = true;
        }
    }

    if (state->type & LIGHTS_TYPE_MODE) {
        err = get_header_mode(&state->mode, &header_mode);
        if (err) {
            AURA_ERR("state.mode is invalid");
            goto exit;
        }

        /* Return early if mode isn't changing */
        if (header_mode != get_aura_mode(&zone->pending.mode)) {
            /* The given mode contains a pointer which may not be ours */
            effect.mode = *get_lights_mode(header_mode);
            update_effect = true;
        }
    }

    if (state->type & LIGHTS_TYPE_LEDS) {
        if (AURA_MODE_DIRECT != get_aura_mode(&effect.mode)) {
            AURA_ERR("LED colors cannot be applied to mode '%s'", effect.mode.name);
            err = -EPERM;
            goto exit;
        }
        if (IS_TRUE(state->raw.length != zone->led_count)) {
            err = -EINVAL;
            goto exit;
        }
        if (IS_NULL(state->raw.data)) {
            err = -EINVAL;
            goto exit;
        }
        colors = state->raw.data;
        update_colors = true;
    }

    if (update_effect || update_colors) {
        err = aura_header_zone_update(
            zone,
            update_effect ? &effect : NULL,
            update_colors ? colors : NULL
        );
    } else {
        /* Nothing to update is not an error */
        err = 0;
    }

exit:
    spin_unlock(&zone->lock);

    return err;
}

/**
 * aura_header_zone_read() - Userland read handler
 *
 * @thunk: Zone being read
 * @state: Buffer to write effect values into
 *
 * @return: Error code
 */
static error_t aura_header_zone_read (
    struct lights_thunk *thunk,
    struct lights_state *state
){
    struct aura_header_zone *zone = zone_from_thunk(thunk);

    if (IS_NULL(thunk, state, zone) || IS_FALSE(state->type & LIGHTS_TYPE_MODE))
        return -EINVAL;

    spin_lock(&zone->lock);

    if (state->type & LIGHTS_TYPE_MODE)
        state->mode = zone->active.mode;

    if (state->type & LIGHTS_TYPE_COLOR)
        state->color = zone->active.color;

    if (state->type & LIGHTS_TYPE_SPEED)
        state->speed = zone->active.speed;

    if (state->type & LIGHTS_TYPE_DIRECTION)
        state->direction = zone->active.direction;

    spin_unlock(&zone->lock);

    return 0;
}

/**
 * aura_header_zone_sync() - Userland write handler
 *
 * @thunk: Zone being updated
 * @state: Container of the new sync value
 *
 * @return: Error code
 *
 * This function is blocking.
 */
static error_t aura_header_zone_sync (
    struct lights_thunk *thunk,
    struct lights_state const *state
){
    struct aura_header_zone *zone = zone_from_thunk(thunk);
    struct lights_adapter_msg msg;

    if (IS_NULL(thunk, state, zone) || IS_FALSE(state->type & LIGHTS_TYPE_SYNC))
        return -EINVAL;

    /* Do we need to check if pending mode supports sync? */

    transfer_add_sync(&msg, zone, state->sync);

    /* Should we send this as a blocking call */
    return lights_adapter_xfer(&global.client, &msg, 1);
}

/**
 * aura_header_controller_update() - Applies global state to all zones
 *
 * @ctrl: Ctrl to update
 *
 * @return: Error code
 */
static error_t aura_header_controller_update (
    struct aura_header_controller *ctrl
){
    struct lights_adapter_msg msg;
    struct lights_state state;
    struct aura_effect effect;
    enum aura_header_mode header_mode;
    error_t err = 0;
    int i;

    if (IS_NULL(ctrl))
        return -EINVAL;

    lights_get_state(&state);

    for (i = 0; i < ctrl->zone_count && !err; i++) {
        effect = ctrl->zones[i].pending;

        if (state.type & LIGHTS_TYPE_COLOR)
            effect.color = state.color;

        if (state.type & LIGHTS_TYPE_SPEED)
            effect.speed = max_t(uint8_t, state.speed, 5);

        if (state.type & LIGHTS_TYPE_DIRECTION)
            effect.direction = max_t(uint8_t, state.direction, 1);

        if (state.type & LIGHTS_TYPE_MODE) {
            err = get_header_mode(&state.mode, &header_mode);
            if (err) {
                AURA_ERR("state.mode is invalid");
                return err;
            }

            switch (header_mode) {
                case AURA_MODE_OFF:
                    /* Overwrite above changes */
                    effect = effect_off;
                    break;

                case AURA_MODE_DIRECT:
                    /* Overwrite above changes */
                    effect = effect_direct;
                    break;

                default:
                    effect.mode = state.mode;
                    break;
            }
        }

        transfer_add_effect(
            &msg,
            &ctrl->zones[i],
            &effect
        );

        err = lights_adapter_xfer(&global.client, &msg, 1);
        if (err) {
            AURA_DBG("read failed with %d", err);
            return err;
        }

        if (!err) {
            ctrl->zones[i].active = effect;
            ctrl->zones[i].pending = effect;
        }
    }

    return 0;
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
    struct lights_io_attribute const attrs[] = {
        LIGHTS_MODE_ATTR(
            &zone->thunk,
            aura_header_zone_read,
            aura_header_zone_write
        ),
        LIGHTS_COLOR_ATTR(
            &zone->thunk,
            aura_header_zone_read,
            aura_header_zone_write
        ),
        LIGHTS_SPEED_ATTR(
            &zone->thunk,
            aura_header_zone_read,
            aura_header_zone_write
        ),
        LIGHTS_DIRECTION_ATTR(
            &zone->thunk,
            aura_header_zone_read,
            aura_header_zone_write
        ),
        LIGHTS_LEDS_ATTR(
            &zone->thunk,
            aura_header_zone_write
        ),
        LIGHTS_UPDATE_ATTR(
            &zone->thunk,
            aura_header_zone_write
        ),
        LIGHTS_SYNC_ATTR(
            &zone->thunk,
            aura_header_zone_sync
        )
    };
    error_t err;

    if (index >= MAX_HEADER_COUNT)
        return -EINVAL;

    lights_thunk_init(&zone->thunk, ZONE_HASH);
    spin_lock_init(&zone->lock);

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

    snprintf(zone->name, sizeof(zone->name), "argb-strip-%d", index);
    AURA_DBG("Creating sysfs for '%s'", zone->name);

    zone->lights.led_count = zone->led_count;
    zone->lights.name = zone->name;
    zone->lights.caps = aura_header_modes;

    err = lights_device_register(&zone->lights);
    if (err)
        return err;

    /* Create the attributes */
    return lights_create_files(&zone->lights, attrs, ARRAY_SIZE(attrs));
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
    // struct lights_state state;
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

    /*
     * Set device and all zones into a known state. If this is the first
     * call on a fresh boot, the underlying device will disconnect and
     * reconnect with a new device number. But, we have no way of knowing
     * when that will happen.
     */
    err = usb_device_reset(ctrl);
    if (err)
        goto error_free;

    /* Apply any global state, ASYNC cannot be used */
    // lights_get_state(&state);
    // aura_header_controller_update(ctrl, &state);

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
    if (global.ctrl) {
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
 *
 * @return: 1 if ctrl was destroyed. 0 otherwise
 */
static inline int aura_header_controller_put (
    struct aura_header_controller *ctrl
){
    return kref_put(&ctrl->refs, aura_header_controller_put_kref);
}

/**
 * aura_header_driver_connect_worker() - Updates a connected controller
 *
 * @work: Delayed work job
 */
static void aura_header_driver_connect_worker (
    struct work_struct *work
){
    struct aura_header_controller *ctrl;
    error_t err;

    ctrl = aura_header_controller_get();
    if (ctrl) {
        err = aura_header_controller_update(ctrl);
        if (err) {
            AURA_ERR("Failed to apply state to controller: %s", ERR_NAME(err));
        }

        aura_header_controller_put(ctrl);
    }
}

/**
 * aura_header_driver_disconnect_worker() - Destroys a disconnected controller
 *
 * @work: Delayed work job
 */
static void aura_header_driver_disconnect_worker (
    struct work_struct *work
){
    spin_lock(&global.lock);

    if (global.ctrl && aura_header_controller_put(global.ctrl)) {
        AURA_INFO("Destroyed global controller");
        global.ctrl = NULL;
    } else if (global.ctrl) {
        AURA_INFO("Released handle (refs %d)", kref_read(&global.ctrl->refs));
    }

    spin_unlock(&global.lock);
}

/**
 * aura_header_driver_on_connect() - Device connection callback
 *
 * @client: Registered USB client
 */
static void aura_header_driver_on_connect (
    struct usb_client *client
){
    struct aura_header_controller *ctrl;
    bool create = false;

    spin_lock(&global.lock);
    if (global.ctrl) {
        /*
         * A previous device disconnected but the 5 second
         * destructor has not yet been invoked. Increase
         * the ref count to cancel the destruction.
         */
         kref_get(&global.ctrl->refs);
         AURA_INFO("Using existing USB controller (refs: %d)", kref_read(&global.ctrl->refs));
    } else {
        /*
         * A previous controller was either destroyed or
         * not yet created. Since both connect/disconnect
         * are mutually exclusive, we don't need to worry
         * about contention
         */
         create = true;
    }
    spin_unlock(&global.lock);

    /*
     * When a controller is created it needs the global state applying to it.
     * The problem is that when it's created on a fresh boot, the device
     * disconnects, and upon reconnection it is set back to its default state.
     *
     * Applying a state in this instance, causes it to be removed upon
     * reconnection. Applying again after reconnections makes for an
     * unappealing sight. Race conditions also exist.
     *
     * There is no way to detect if the device will disconnect, nor can we
     * sleep the current thread. We need to delay the state update until
     * either enough time has passed or a reconnect event is detected.
     */
    if (create) {
        ctrl = aura_header_controller_create();
        if (IS_ERR(ctrl)) {
            CLEAR_ERR(ctrl);
            return;
        }

        global.ctrl = ctrl;
        AURA_INFO("Created global USB controller (refs: %d)", kref_read(&global.ctrl->refs));

        queue_delayed_work(system_wq, &global.connect, 1 * HZ);
    } else {
        mod_delayed_work(system_wq, &global.connect, 0);
    }
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
    schedule = global.ctrl;
    spin_unlock(&global.lock);

    if (schedule) {
        AURA_INFO("Scheduling destruction");
        schedule_delayed_work(&global.disconnect, 5 * HZ);
    } else {
        AURA_INFO("No controller to destruct");
    }
}

/**
 * aura_header_probe() - Entry point
 *
 * @state: Initial state of all zones
 *
 * @return: Error code
 */
error_t aura_header_probe (
    struct lights_state const *state
){
    struct usb_client usb = {
        .name = driver_name,
        .packet_size = PACKET_SIZE,
        .ids = device_ids,
        .on_connect = aura_header_driver_on_connect,
        .on_disconnect = aura_header_driver_on_disconnect,
    };

    LIGHTS_USB_CLIENT_INIT(&global.client, &usb);
    INIT_DELAYED_WORK(&global.connect, aura_header_driver_connect_worker);
    INIT_DELAYED_WORK(&global.disconnect, aura_header_driver_disconnect_worker);
    spin_lock_init(&global.lock);
    global.ctrl = NULL;

    return lights_adapter_register(&global.client, 32);
}

/**
 * aura_header_release() - Exit point
 */
void aura_header_release (
    void
){
    struct aura_header_controller *ctrl = NULL;

    /* Remove here to prevent 5 second delay */
    spin_lock(&global.lock);
    ctrl = global.ctrl;
    global.ctrl = NULL;
    spin_unlock(&global.lock);

    if (ctrl && aura_header_controller_put(ctrl))
        AURA_INFO("Destroyed global controller");

    if (lights_adapter_is_registered(&global.client))
        /* This should cause an on_disconnect event */
        lights_adapter_unregister(&global.client);

    cancel_delayed_work_sync(&global.connect);
    cancel_delayed_work_sync(&global.disconnect);
}
