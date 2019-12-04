// SPDX-License-Identifier: GPL-2.0
#include <linux/slab.h>
#include <linux/kref.h>

#include <adapter/lights-adapter.h>
#include <adapter/lib/reserve.h>
#include "aura-controller.h"

#define AURA_APPLY_VAL 0x01

/**
 * enum aura_registers - [description]
 *
 */
enum aura_registers {
    AURA_REG_DEVICE_NAME        = 0x1000,   /* Device String 16 bytes               */
    AURA_REG_COLORS_DIRECT_EC1  = 0x8000,   /* Colors for Direct Mode 15 bytes      */
    AURA_REG_COLORS_EFFECT_EC1  = 0x8010,   /* Colors for Internal Effects 15 bytes */
    AURA_REG_DIRECT             = 0x8020,   /* "Direct Access" Selection Register   */
    AURA_REG_MODE               = 0x8021,   /* AURA Mode Selection Register         */
    AURA_REG_APPLY              = 0x80A0,   /* AURA Apply Changes Register          */
    AURA_REG_ZONE_ID            = 0x80E0,   /* An array of zone IDs                 */
    AURA_REG_SLOT_INDEX         = 0x80F8,   /* AURA Slot Index Register (RAM only)  */
    AURA_REG_I2C_ADDRESS        = 0x80F9,   /* AURA I2C Address Register (RAM only) */
    AURA_REG_COLORS_DIRECT_EC2  = 0x8100,   /* Direct Colors (v2) 30 bytes          */
    AURA_REG_COLORS_EFFECT_EC2  = 0x8160,   /* Internal Colors (v2) 30 bytes        */
};

/*
    NOTE - Don't change these values. They are written to the i2c controller.
    TODO - Remove the values not acually used.
 */
enum aura_mode {
    AURA_MODE_OFF                       = 0x00,
    AURA_MODE_STATIC                    = 0x01,
    AURA_MODE_BREATHING                 = 0x02,
    AURA_MODE_FLASHING                  = 0x03,
    AURA_MODE_CYCLE                     = 0x04,
    AURA_MODE_RAINBOW                   = 0x05,
    AURA_MODE_SPECTRUM_CYCLE_BREATHING  = 0x06,
    AURA_MODE_CHASE_FADE                = 0x07,
    AURA_MODE_SPECTRUM_CYCLE_CHASE_FADE = 0x08,
    AURA_MODE_CHASE                     = 0x09,
    AURA_MODE_SPECTRUM_CYCLE_CHASE      = 0x0A,
    AURA_MODE_SPECTRUM_CYCLE_WAVE       = 0x0B,
    AURA_MODE_CHASE_RAINBOW_PULSE       = 0x0C,
    AURA_MODE_RANDOM_FLICKER            = 0x0D,
    AURA_MODE_LAST                      = AURA_MODE_RANDOM_FLICKER,
    AURA_MODE_DIRECT                    = 0xFF,
};

enum aura_command {
    CMD_SET_ADDR    = 0x00,
    CMD_READ_BYTE   = 0x81,
    CMD_WRITE_BYTE  = 0x01,
    CMD_READ_WORD   = 0x82,
    CMD_WRITE_WORD  = 0x02,
    CMD_READ_BLOCK  = 0x80,
    CMD_WRITE_BLOCK = 0x03
};

/**
 * struct aura_colors - Representation of color array on the hardware
 *
 * @reg:   Starting register of hardware color array
 * @count: Number of 3 byte tuplets
 * @zone:  Color object for each tuplet
 */
struct aura_colors {
    uint16_t            reg;
    uint8_t             count;
    struct lights_color zone[];
};

/**
 * struct aura_zone_context - Storage for an individual zone
 *
 * @zone:     Public object
 * @offset:   Offset into context color arrays
 * @effect:   A pointer into the ctrl->effect_colors->zone array
 * @direct:   A pointer into the ctrl->direct_colors->zone array
 * @context:  Owning context
 */
struct aura_zone_context {
    struct aura_zone                zone;

    uint8_t                         offset;
    struct lights_color             *effect;
    struct lights_color             *direct;
    struct aura_controller_context  *context;
};

/**
 * struct aura_controller_context - Storage for a group of zones
 *
 * @ctrl:           Public object
 * @callback_pool:  Reserve of aura_callback_context
 * @lock:           Context spin lock
 * @mode:           Active mode
 * @zone_all:       One zone to rule them all
 * @zone_contexts:  Array of individual zones
 * @effect_colors:  Array of color objects for each zone
 * @direct_colors:  Array of color objects for each zone
 * @is_direct:      Flag to indicate which color array is in use
 * @zone_count:     Number of zones
 * @version:        Version of the control (determines some registers)
 * @lights_client:  Userland access
 * @name:           Chipset name
 */
struct aura_controller_context {
    struct aura_controller          ctrl;
    reserve_t                       callback_pool;

    struct mutex                    lock;             /* Read/Write access lock */
    const struct lights_mode        *mode;            /* The current mode, only applied if non direct */
    struct aura_zone_context        *zone_all;        /* A special zone used to represent all the others */
    struct aura_zone_context        *zone_contexts;   /* Data for each available zone, plus an extra representing all */
    struct aura_colors              *effect_colors;   /* Each zones effect color cache */
    struct aura_colors              *direct_colors;   /* Each zones direct color cache */
//    struct aura_colors           *unknown_colors;    /* LightingService reads this array from 0x81C0 upon handshake */
    uint8_t                         is_direct;        /* Flag to indicate if direct mode is enabled */
    uint8_t                         zone_count;
    uint8_t                         version;

    struct lights_adapter_client    lights_client;
    char                            name[32];
};

#define context_from_ctrl(ptr) (\
    container_of(ptr, struct aura_controller_context, ctrl) \
)

#define zone_from_context(ptr) (\
    container_of(ptr, struct aura_zone_context, zone) \
)


struct aura_callback_context {
    struct aura_controller_context  *context;
    struct lights_color             *color;
};

inline struct aura_callback_context *aura_callback_cache_alloc (
    struct aura_controller_context *context
){
    struct aura_callback_context *cb;

    cb = reserve_alloc(context->callback_pool);
    if (IS_ERR(cb)) {
        AURA_ERR("Failed to allocate from reserve");
        return ERR_CAST(cb);
    }

    cb->context = context;

    return cb;
}

inline void aura_callback_cache_free (
    struct aura_callback_context const *data
){
    reserve_free(data->context->callback_pool, data);
}

struct lights_mode aura_available_modes[] = {
    LIGHTS_MODE(OFF),
    LIGHTS_MODE(STATIC),
    LIGHTS_MODE(BREATHING),
    LIGHTS_MODE(FLASHING),
    LIGHTS_MODE(CYCLE),
    LIGHTS_MODE(RAINBOW),

    LIGHTS_CUSTOM_MODE(AURA_MODE_SPECTRUM_CYCLE_BREATHING,  "spectrum_cycle_breathing"),
    LIGHTS_CUSTOM_MODE(AURA_MODE_CHASE_FADE,                "chase_fade"),
    LIGHTS_CUSTOM_MODE(AURA_MODE_SPECTRUM_CYCLE_CHASE_FADE, "spectrum_cycle_chase_fade"),
    LIGHTS_CUSTOM_MODE(AURA_MODE_CHASE,                     "chase"),
    LIGHTS_CUSTOM_MODE(AURA_MODE_SPECTRUM_CYCLE_CHASE,      "spectrum_cycle_chase"),
    LIGHTS_CUSTOM_MODE(AURA_MODE_SPECTRUM_CYCLE_WAVE,       "spectrum_cycle_wave"),
    LIGHTS_CUSTOM_MODE(AURA_MODE_CHASE_RAINBOW_PULSE,       "chase_rainbow_pulse"),
    LIGHTS_CUSTOM_MODE(AURA_MODE_RANDOM_FLICKER,            "random_flicker"),
    LIGHTS_CUSTOM_MODE(AURA_MODE_DIRECT,                    "direct"),

    LIGHTS_MODE_LAST_ENTRY()
};

/*
    NOTE: The order of these are important
 */
static const char *zone_names[] = {
   "cpu", "vrm", "center", "pch", "audio", "back_io", "rgb_strip_1",
   "rgb_strip_2", "back_plate", "io_cover", "memory", "pcie", "area",
   "pcb_surround", "dimm2", "light_bar", "odd", "rgb_strip", "m2",
   "rgb_header_1_2", "rgb_header_3_4", "start_retry_button",
   "edge_right", "logo",
};

/**
 * aura_controller_get_caps()
 *
 * @return: The array of capabilities
 */
const struct lights_mode *aura_controller_get_caps (
    void
){
    return aura_available_modes;
}

/**
 * aura_controller_read_byte() - Reads a single byte from hardware
 *
 * @client: Access to hardware
 * @reg:    Register to read
 * @value:  Buffer to write to
 *
 * @return: Error code
 *
 * This function is blocking.
 */
static error_t aura_controller_read_byte (
    struct lights_adapter_client * client,
    uint16_t reg,
    uint8_t * value
){
    error_t err;
    struct lights_adapter_msg msgs[] = {
        ADAPTER_WRITE_WORD_DATA_SWAPPED(CMD_SET_ADDR, reg),
        ADAPTER_READ_BYTE_DATA(CMD_READ_BYTE)
    };

    err = lights_adapter_xfer(client, msgs, ARRAY_SIZE(msgs));
    if (!err)
        *value = msgs[1].data.byte;

    return err;
}

/**
 * aura_controller_read_block() - Reads @size number of bytes from hardware
 *
 * @client: Access to hardware
 * @reg:    Register to read
 * @data:   Buffer to write to
 * @size:   Number of bytes to read
 *
 * @return: Error code
 *
 * This function is blocking
 */
static error_t aura_controller_read_block (
    struct lights_adapter_client * client,
    uint16_t reg,
    uint8_t * data,
    uint8_t size
){
    error_t err;
    int i, j;
    struct lights_adapter_msg *msgs;

    msgs = (struct lights_adapter_msg[]){
        ADAPTER_WRITE_WORD_DATA_SWAPPED(CMD_SET_ADDR, reg),
        ADAPTER_READ_BLOCK_DATA(CMD_READ_BLOCK + size, size)
    };

    err = lights_adapter_xfer(client, msgs, 2);

    /*
        Some adapters don't support the I2C_FUNC_SMBUS_READ_BLOCK_DATA
        protocol, so we will resort to single byte reads. Creating a
        buffer here will prevent block read from being called again.
    */
    if (err) {
        AURA_DBG("I2C_FUNC_SMBUS_READ_BLOCK_DATA not supported");

        msgs = kcalloc(sizeof(*msgs), size * 2, GFP_KERNEL);
        if (!msgs)
            return -ENOMEM;

        for (i = 0, j = 0; j < size; i += 2, j++) {
            msgs[i]      = ADAPTER_WRITE_WORD_DATA_SWAPPED(CMD_SET_ADDR, reg + j);
            msgs[i + 1 ] = ADAPTER_READ_BYTE_DATA(CMD_READ_BYTE);
        }

        err = lights_adapter_xfer(client, msgs, size * 2);
        if (!err) {
            for (i = 0, j = 0; j < size; i += 2, j++) {
                data[j] = msgs[i].data.byte;
            }
        }
        kfree(msgs);
    } else {
        memcpy(data, msgs[1].data.block, msgs[1].length);
    }

    return err;
}

/**
 * lights_mode_to_aura_mode() - Converts global to local mode
 *
 * @mode:      Global mode
 * @aura_mode: Target buffer
 *
 * @return: Error code
 */
static error_t lights_mode_to_aura_mode (
    const struct lights_mode *mode,
    enum aura_mode *aura_mode
){
    if (lights_is_custom_mode(mode)) {
        *aura_mode = lights_custom_mode_id(mode);
        if (*aura_mode <= AURA_MODE_LAST || *aura_mode == AURA_MODE_DIRECT)
            return 0;

        return -ENODATA;
    }

    switch (lights_mode_id(mode)) {
        case LIGHTS_MODE_OFF:
            *aura_mode = AURA_MODE_OFF;
            return 0;
        case LIGHTS_MODE_STATIC:
            *aura_mode = AURA_MODE_STATIC;
            return 0;
        case LIGHTS_MODE_BREATHING:
            *aura_mode = AURA_MODE_BREATHING;
            return 0;
        case LIGHTS_MODE_FLASHING:
            *aura_mode = AURA_MODE_FLASHING;
            return 0;
        case LIGHTS_MODE_CYCLE:
            *aura_mode = AURA_MODE_CYCLE;
            return 0;
        case LIGHTS_MODE_RAINBOW:
            *aura_mode = AURA_MODE_RAINBOW;
            return 0;
    }

    return -ENODATA;
}

/**
 * chipset_mode_to_lights_mode() - Converts local to global mode
 *
 * @id: Local mode
 *
 * @return: Global Mode
 */
static const struct lights_mode *chipset_mode_to_lights_mode (
    uint8_t id
){
    const struct lights_mode *p;
    enum aura_mode aura_mode;

    lights_for_each_mode(p, aura_available_modes) {
        lights_mode_to_aura_mode(p, &aura_mode);
        if (aura_mode == id)
            return p;
    }

    return NULL;
}


/**
 * aura_colors_create() - Creates and optionally populates a color cache object
 *
 * @context: Owning context
 * @count:   Number of zones
 * @reg:     Starting register of hardware color array
 * @read:    Flag to also populate values
 *
 * @return: Created object or an error code
 */
static struct aura_colors *aura_colors_create (
    struct aura_controller_context *context,
    uint8_t count,
    uint16_t reg,
    bool read
){
    struct aura_colors * colors;
    uint8_t * buffer = NULL;
    error_t err = 0;
    int i, j;

    if (IS_NULL(context) || IS_TRUE(0 == count))
        return ERR_PTR(-EINVAL);

    colors = kmalloc(sizeof(struct aura_colors) + sizeof(struct lights_color) * count, GFP_KERNEL);
    if (!colors)
        return ERR_PTR(-ENOMEM);

    colors->count = count;
    colors->reg   = reg;

    if (read) {
        count = count * 3;
        buffer = kmalloc(count, GFP_KERNEL);
        if (!buffer) {
            err = -ENOMEM;
            goto error;
        }

        err = aura_controller_read_block(&context->lights_client, reg, buffer, count);
        if (err) {
            AURA_TRACE("%s failed with %d", "aura_controller_read_block()", err);
            goto error;
        }

        for (i = 0, j = 0; i < count; i += 3, j++) {
            lights_color_read(&colors->zone[j], &buffer[i]);
        }
    }

error:
    kfree(buffer);

    return err ? ERR_PTR(err) : colors;
}

/**
 * aura_zones_create() - Populates context with zones
 *
 * @context: Owning context
 * @version: Chipset version
 * @count:   Number of zones
 *
 * @return: Error code
 */
static error_t aura_zones_create (
    struct aura_controller_context *context,
    uint8_t version,
    uint8_t count
){
    struct aura_zone_context *zone;
    struct aura_colors *effect_colors;
    struct aura_colors *direct_colors;
    int size, i;
    error_t err;
    uint8_t zone_id;

    if (IS_NULL(context) || IS_TRUE(0 == count))
        return -EINVAL;

    // Read the effect colors
    effect_colors = aura_colors_create(
        context,
        count,
        version == 1 ? AURA_REG_COLORS_EFFECT_EC1 : AURA_REG_COLORS_EFFECT_EC2,
        true
    );
    if (IS_ERR(effect_colors)) {
        AURA_TRACE("%s failed with %ld", "aura_colors_create()", PTR_ERR(effect_colors));
        return PTR_ERR(effect_colors);
    }

    // Allocate the direct colors
    direct_colors = aura_colors_create(
        context,
        count,
        version == 1 ? AURA_REG_COLORS_DIRECT_EC1 : AURA_REG_COLORS_DIRECT_EC2,
        false
    );
    if (IS_ERR(direct_colors)) {
        AURA_TRACE("%s failed with %ld", "aura_colors_create()", PTR_ERR(effect_colors));
        kfree(effect_colors);
        return PTR_ERR(direct_colors);
    }

    // There is another array of colors read at 0x81C0, but it's unknown how they are used.

    size = sizeof(struct aura_zone_context) * (count + 1);
    zone = kzalloc(size , GFP_KERNEL);
    if (!zone) {
        err = -ENOMEM;
        goto error_free_colors;
    }

    // Create a zone which represents all the others
    context->zone_all = &zone[count];

    zone[count].zone.id     = ZONE_ID_ALL;
    zone[count].zone.name   = "all";
    zone[count].zone.ctrl   = &context->ctrl;
    zone[count].context     = context;
    zone[count].offset      = 0;
    zone[count].effect      = &effect_colors->zone[0];
    zone[count].direct      = &direct_colors->zone[0];

    for (i = 0; i < count; i++) {
        err = aura_controller_read_byte(&context->lights_client, AURA_REG_ZONE_ID + i, &zone_id);
        if (err || zone_id >= ARRAY_SIZE(zone_names))
            goto error_free_zone;

        zone[i].zone.id     = zone_id;
        zone[i].zone.name   = zone_names[zone_id];
        zone[i].zone.ctrl   = &context->ctrl;
        zone[i].context     = context;
        zone[i].offset      = i;
        zone[i].effect      = &effect_colors->zone[i];
        zone[i].direct      = &direct_colors->zone[i];
    }

    context->effect_colors  = effect_colors;
    context->direct_colors  = direct_colors;
    context->zone_count     = count;
    context->zone_contexts  = zone;

    return 0;

error_free_zone:
    kfree(zone);
error_free_colors:
    kfree(effect_colors);
    kfree(direct_colors);

    return err;
}

/**
 * aura_controller_context_destroy() - Releases all memory associated with a controller
 *
 * @context: The context to free
 */
void aura_controller_context_destroy (
    struct aura_controller_context *context
){
    if (context->callback_pool)
        reserve_put(context->callback_pool);

    lights_adapter_unregister(&context->lights_client);
    kfree(context->zone_contexts);
    kfree(context->direct_colors);
    kfree(context->effect_colors);
    kfree(context);
}

/**
 * is_printable() - Checks a string contains only printable characters
 *
 * @str: String to test
 *
 * @return: True or False
 */
static inline bool is_printable (
    const char *str
){
    const int len = str ? strlen(str) : 0;
    int i;

    for (i = 0; i < len; i++)
        if (!isprint(str[i]))
            return false;

    return i > 0;
}

/**
 * aura_controller_create() - Tests for and creates an interface
 *
 * @client: The lights managed i2c_adapter
 *
 * @return: The controller, NULL if not found or a negative error
 */
struct aura_controller *aura_controller_create (
    struct lights_adapter_client *client
){
    struct aura_controller_context *context;
    struct lights_color * color;
    char ctrl_name[17] = {0};
    uint8_t zone_count, mode_id;
    error_t err;
    int i;

    if (IS_NULL(client))
        return ERR_PTR(-ENODEV);

    // AURA_DBG("Reading ZoneCount");
    zone_count = 0;
    err = aura_controller_read_byte(client, 0x80C1, &zone_count);
    if (err) {
        AURA_DBG(
            "Failed to read zone count from '0x%02x' with error %s",
            client->i2c_client.addr,
            ERR_NAME(err)
        );
        /* Return NULL rather an error code */
        return NULL;
    }

    if (zone_count == 0 || zone_count >= 8) {
        AURA_DBG("Invalid zone count (%d)", zone_count);
        return NULL;
    }

    // Read the device name and EC version
    // AURA_DBG("Reading ControllerName");
    err = aura_controller_read_block(client, AURA_REG_DEVICE_NAME, ctrl_name, 16);
    if (err) {
        AURA_DBG("Failed to read device name");
        return NULL;
    }

    if (!is_printable(ctrl_name)) {
        AURA_DBG("Device name appears invalid");
        return NULL;
    }

    context = kzalloc(sizeof(*context), GFP_KERNEL);
    if (!context)
        return ERR_PTR(-ENOMEM);

    memcpy(context->name, ctrl_name, 16);
    memcpy(&context->lights_client, client, sizeof(*client));
    mutex_init(&context->lock);

    err = lights_adapter_register(&context->lights_client, 32);
    if (err) {
        AURA_DBG("Failed to register lights_adapter: %s", ERR_NAME(err));
        goto error;
    }

    context->zone_count = zone_count;

    if (strncmp(context->name, "AUMA0-E6K5", 10) == 0 || strncmp(context->name, "AUDA0-E6K5", 10) == 0)
        context->version = 2;
    else
        context->version = 1;

    AURA_INFO("device '%s' has an %s controller.", context->name, context->version == 1 ? "EC1" : "EC2");

    // Build the zones
    // AURA_DBG("Creating Zones");
    err = aura_zones_create(context, context->version, zone_count);
    if (err) {
        AURA_ERR("Failed to create zones");
        goto error;
    }

    // Read the configured mode
    err = aura_controller_read_byte(client, AURA_REG_MODE, &mode_id);
    if (err) {
        AURA_ERR("Failed to read device mode");
        goto error;
    }

    if (NULL == (context->mode = chipset_mode_to_lights_mode(mode_id))) {
        AURA_ERR("Failed to translate device mode: 0x%02x", mode_id);
        err = -EIO;
        goto error;
    }

    // Detect if direct mode is applied
    err = aura_controller_read_byte(client, AURA_REG_DIRECT, &context->is_direct);
    if (err) {
        AURA_ERR("Failed to read device is_direct");
        goto error;
    }

    context->callback_pool = reserve_get(aura_callback_context, 32, SLAB_POISON, GFP_KERNEL);
    if (IS_ERR(context->callback_pool)) {
        err = CLEAR_ERR(context->callback_pool);
        goto error;
    }

    context->ctrl.name       = context->name;
    context->ctrl.version    = context->version;
    context->ctrl.zone_count = context->zone_count;

    for (i = 0; i < context->zone_count; i++) {
        color = context->is_direct
            ? context->zone_contexts[i].direct
            : context->zone_contexts[i].effect;
        AURA_DBG(
            "Detected zone: %s, color: 0x%06x, mode: %s",
            context->zone_contexts[i].zone.name,
            color->value,
            context->mode->name
        );
    }

    return &context->ctrl;

error:
    aura_controller_context_destroy(context);

    return ERR_PTR(err);
}

/**
 * aura_controller_destroy() - Releases the controller
 *
 * @ctrl: Previously allocated with @aura_controller_create
 */
void aura_controller_destroy (
    struct aura_controller *ctrl
){
    struct aura_controller_context *context = context_from_ctrl(ctrl);

    if (IS_NULL(ctrl))
        return;

    aura_controller_context_destroy(context);
}

/**
 * aura_controller_create_slaves() - Creates slave controllers
 *
 * @ctrl:   Previously allocated with @aura_controller_create
 * @slaves: Array to hold found slave controllers
 * @count:  Length of @slaves array (MUST be 4)
 *
 * @return: The number of found slaves
 */
int aura_controller_create_slaves (
    struct aura_controller * ctrl,
    struct aura_controller ** slaves,
    size_t count
){
    struct aura_controller_context * context = context_from_ctrl(ctrl);
    struct lights_adapter_client client;
    struct aura_controller * slave;
    uint8_t addr, next;
    int found = 0;
    error_t err;

    if (IS_NULL(ctrl, slaves) || IS_TRUE(4 != count))
        return 0;

    memcpy(&client, &context->lights_client, sizeof(client));

    for (addr = 0xAA; addr <= 0xAD; ++addr) {
        err = aura_controller_read_byte(&context->lights_client, 0x8000 | addr, &next);
        if (!err && next) {
            LIGHTS_SMBUS_CLIENT_UPDATE(&client, next >> 1);
            // client.smbus_client.addr = next >> 1;
            slave = aura_controller_create(&client);
            if (IS_NULL(slave)) {
                err = -EIO;
                goto error;
            }
            if (!IS_ERR(slave)) {
                slaves[found] = slave;
                found++;
                continue;
            }
        }
        break;
    }

    return found;

error:
    while (found > 0) {
        aura_controller_destroy(slaves[found]);
        found--;
    }

    return err;
}



/**
 * aura_controller_get_zone() - Fetches a zone by its index
 *
 * @ctrl:  Previously allocated with @aura_controller_create
 * @index: Zero based index of the zone
 *
 * @return: The zone or a negative error number
 */
struct aura_zone *aura_controller_get_zone (
    struct aura_controller *ctrl,
    uint8_t index
){
    struct aura_controller_context *ctx = context_from_ctrl(ctrl);

    if (IS_NULL(ctrl))
        return ERR_PTR(-EINVAL);

    if (index < ctx->zone_count)
        return &ctx->zone_contexts[index].zone;

    if (index == ZONE_ID_ALL)
        return &ctx->zone_all->zone;

    return ERR_PTR(-EINVAL);
}

/**
 * aura_controller_set_zone_color_callback() - Async handler for color setting
 *
 * @result: Messages sent to the device
 * @_data:  Context of the call
 * @error:  A negative error number if async failed
 */
static void aura_controller_set_zone_color_callback (
    struct lights_adapter_msg const * const result,
    void *_data,
    error_t error
){
    struct aura_callback_context *data = _data;
    struct lights_adapter_msg const * color_msg;
    struct lights_color color;

    if (IS_NULL(result, _data))
        return;

    if (error) {
        AURA_DBG("Failed to set color");
        goto exit;
    }

    color_msg = adapter_seek_msg(result, 1);
    if (!color_msg) {
        AURA_ERR("Failed to seek message");
        goto exit;
    }

    if (color_msg->length != 3) {
        AURA_ERR("Message has an invalid length '%d'", color_msg->length);
        goto exit;
    }

    lights_color_read(&color, color_msg->data.block);

    mutex_lock(&data->context->lock);

    *data->color = color;

    mutex_unlock(&data->context->lock);

exit:
    aura_callback_cache_free(data);
}

/**
 * aura_controller_set_zone_color() - Applies a color to a single zone
 *
 * @zone:  Returned from @aura_controller_get_zone
 * @color: Color to apply
 *
 * @return: Zero or negative error number
 *
 * The color is applied asynchronously
 */
error_t aura_controller_set_zone_color (
    struct aura_zone *zone,
    const struct lights_color *color
){
    struct aura_zone_context *zone_ctx = zone_from_context(zone);
    struct aura_controller_context *context;
    struct aura_colors *target;
    struct aura_callback_context *cb_data;
    struct lights_adapter_msg msgs[4];

    error_t err;
    int count = 0;

    if (IS_NULL(zone, color))
        return -EINVAL;

    context = zone_ctx->context;

    if (context->is_direct)
        target = context->direct_colors;
    else
        target = context->effect_colors;

    msgs[0] = ADAPTER_WRITE_WORD_DATA_SWAPPED(CMD_SET_ADDR, target->reg + (3 * zone_ctx->offset));
    msgs[1] = ADAPTER_WRITE_BLOCK_DATA(CMD_WRITE_BLOCK, 3);

    lights_color_write(color, msgs[1].data.block);

    if (!context->is_direct) {
        msgs[2] = ADAPTER_WRITE_WORD_DATA_SWAPPED(CMD_SET_ADDR, AURA_REG_APPLY);
        msgs[3] = ADAPTER_WRITE_BYTE_DATA(CMD_WRITE_BYTE, AURA_APPLY_VAL);
        count = 4;
    } else {
        count = 2;
    }

    cb_data = aura_callback_cache_alloc(context);
    if (IS_ERR(cb_data))
        return PTR_ERR(cb_data);

    cb_data->color = &target->zone[zone_ctx->offset];

    err = lights_adapter_xfer_async(
        &context->lights_client,
        msgs,
        count,
        cb_data,
        aura_controller_set_zone_color_callback
    );

    if (err)
        aura_callback_cache_free(cb_data);

    return err;
}

/**
 * aura_controller_get_zone_color() - Reads the color of a zone
 *
 * @zone:  Returned from @aura_controller_get_zone
 * @color: Buffer to read into
 *
 * @return: Zero or negative error number
 *
 * This method returns the local color value which may differ from that of
 * the device when an async set_color is pending.
 */
error_t aura_controller_get_zone_color (
    struct aura_zone *zone,
    struct lights_color *color
){
    struct aura_zone_context *zone_ctx = zone_from_context(zone);
    struct aura_controller_context *context;

    if (IS_NULL(zone, color))
        return -EINVAL;

    context = zone_ctx->context;

    if (zone_ctx == context->zone_all) {
        AURA_DBG("zone 'all' cannot return a color");
        return -EIO;
    }

    mutex_lock(&context->lock);

    if (context->is_direct) {
        *color = *zone_ctx->direct;
    } else {
        *color = *zone_ctx->effect;
    }

    mutex_unlock(&context->lock);

    return 0;
}


/**
 * aura_controller_set_zone_color_callback() - Async handler for color setting
 *
 * @result: Messages sent to the device
 * @_data:  Context of the call
 * @error:  A negative error number if async failed
 */
static void aura_controller_set_color_callback (
    struct lights_adapter_msg const * const result,
    void *_data,
    error_t error
){
    struct aura_callback_context *data = _data;
    struct lights_adapter_msg const * color_msg;
    int i;

    if (IS_NULL(result, _data))
        return;

    if (error) {
        AURA_DBG("Failed to set color");
        goto exit;
    }

    color_msg = adapter_seek_msg(result, 1);
    if (!color_msg) {
        AURA_ERR("Failed to seek message");
        goto exit;
    }

    if (color_msg->length != data->context->zone_count * 3) {
        AURA_ERR("Message has an invalid length '%d'", color_msg->length);
        goto exit;
    }

    mutex_lock(&data->context->lock);

    for (i = 0; i <= data->context->zone_count; i++) {
        lights_color_read(&data->color[i], &color_msg->data.block[i * 3]);
    }

    mutex_unlock(&data->context->lock);

exit:
    aura_callback_cache_free(data);
}

/**
 * aura_controller_set_colors() - Applies a color to all zones
 *
 * @ctrl:  Previously allocated with @aura_controller_create
 * @color: Color to apply
 * @count: Number of colors
 *
 * @return: Zero or negative error number
 *
 * If @count is 1, the same color is applied to all zones, otherwise
 * @count must be equal to the zone count.
 */
error_t aura_controller_set_colors (
    struct aura_controller *ctrl,
    const struct lights_color *colors,
    uint8_t count
){
    struct aura_controller_context *context = context_from_ctrl(ctrl);
    struct aura_colors *target;
    struct aura_callback_context *cb_data;
    struct lights_adapter_msg msgs[4];
    int i;
    error_t err;

    if (IS_NULL(ctrl, colors) || IS_FALSE(count == 1 || count == context->zone_count))
        return -EINVAL;

    if (context->is_direct)
        target = context->direct_colors;
    else
        target = context->effect_colors;

    msgs[0] = ADAPTER_WRITE_WORD_DATA_SWAPPED(CMD_SET_ADDR, target->reg);
    msgs[1] = ADAPTER_WRITE_BLOCK_DATA(CMD_WRITE_BLOCK, context->zone_count * 3);

    for (i = 0; i < context->zone_count; i++) {
        lights_color_write(colors, &msgs[1].data.block[i * 3]);

        if (count > 1)
            colors++;
    }

    if (!context->is_direct) {
        msgs[2] = ADAPTER_WRITE_WORD_DATA_SWAPPED(CMD_SET_ADDR, AURA_REG_APPLY);
        msgs[3] = ADAPTER_WRITE_BYTE_DATA(CMD_WRITE_BYTE, AURA_APPLY_VAL);
        count = 4;
    } else {
        count = 2;
    }

    cb_data = aura_callback_cache_alloc(context);
    if (IS_ERR(cb_data))
        return PTR_ERR(cb_data);

    cb_data->color = target->zone;

    AURA_DBG("Queing %d messages to update color", count);
    err = lights_adapter_xfer_async(
        &context->lights_client,
        msgs,
        count,
        cb_data,
        aura_controller_set_color_callback
    );

    if (err)
        aura_callback_cache_free(cb_data);

    return err;
}


/**
 * aura_controller_set_zone_color_callback() - Async handler for mode setting
 *
 * @result: Messages sent to the device
 * @_data:  Context of the call
 * @error:  A negative error number if async failed
 */
static void aura_controller_set_mode_callback (
    struct lights_adapter_msg const * const result,
    void *_data,
    error_t error
){
    struct aura_callback_context *data = _data;
    struct lights_adapter_msg const * mode_msg;
    const struct lights_mode *lights_mode;
    enum aura_mode aura_mode;

    if (IS_NULL(result, _data))
        return;

    if (error) {
        AURA_DBG("Failed to set mode");
        goto exit;
    }

    mode_msg = adapter_seek_msg(result, 1);
    if (!mode_msg) {
        AURA_ERR("Failed to seek message");
        goto exit;
    }

    aura_mode = mode_msg->data.byte;
    lights_mode = chipset_mode_to_lights_mode(aura_mode);
    if (!lights_mode) {
        AURA_ERR("Message contains an invalid mode '0x%02x'", aura_mode);
        goto exit;
    }

    mutex_lock(&data->context->lock);

    if (aura_mode == AURA_MODE_DIRECT) {
        data->context->is_direct = true;
    } else {
        data->context->mode = lights_mode;
    }

    mutex_unlock(&data->context->lock);

exit:
    aura_callback_cache_free(data);
}

/**
 * aura_controller_set_mode() - Applies a mode to all zones
 *
 * @ctrl:  Previously allocated with @aura_controller_create
 * @color: Mode to apply
 *
 * @return: Zero or negative error number
 *
 * NOTE: A single zone cannot have its own mode.
 */
error_t aura_controller_set_mode (
    struct aura_controller *ctrl,
    const struct lights_mode *mode
){
    struct aura_controller_context *context = context_from_ctrl(ctrl);
    enum aura_mode aura_mode;
    struct aura_callback_context *cb_data;
    error_t err;
    int count = 0;

    struct lights_adapter_msg msgs[4];

    if (IS_NULL(ctrl, mode))
        return -EINVAL;

    err = lights_mode_to_aura_mode(mode, &aura_mode);
    if (err)
        return err;

    AURA_DBG("setting mode: '%s' to all zones", mode->name);

    if (aura_mode == AURA_MODE_DIRECT) {
        if (!context->is_direct) {
            count = 2;
            msgs[0] = ADAPTER_WRITE_WORD_DATA_SWAPPED(CMD_SET_ADDR, AURA_REG_DIRECT);
            msgs[1] = ADAPTER_WRITE_BYTE_DATA(CMD_WRITE_BYTE, 0x01);
        }
    } else {
        if (context->is_direct) {
            count = 2;
            msgs[0] = ADAPTER_WRITE_WORD_DATA_SWAPPED(CMD_SET_ADDR, AURA_REG_DIRECT);
            msgs[1] = ADAPTER_WRITE_BYTE_DATA(CMD_WRITE_BYTE, 0x00);
        }
        if (mode->id != context->mode->id) {
            msgs[count]     = ADAPTER_WRITE_WORD_DATA_SWAPPED(CMD_SET_ADDR, AURA_REG_MODE);
            msgs[count + 1] = ADAPTER_WRITE_BYTE_DATA(CMD_WRITE_BYTE, aura_mode);
            count += 2;
        }
    }

    if (count) {
        msgs[count]     = ADAPTER_WRITE_WORD_DATA_SWAPPED(CMD_SET_ADDR, AURA_REG_APPLY);
        msgs[count + 1] = ADAPTER_WRITE_BYTE_DATA(CMD_WRITE_BYTE, AURA_APPLY_VAL);
        count += 2;

        cb_data = aura_callback_cache_alloc(context);
        if (IS_ERR(cb_data))
            return PTR_ERR(cb_data);

        AURA_DBG("Queing %d messages to update mode", count);
        err = lights_adapter_xfer_async(
            &context->lights_client,
            msgs,
            count,
            cb_data,
            aura_controller_set_mode_callback
        );

        if (err)
            aura_callback_cache_free(cb_data);
    }

    return err;
}

/**
 * aura_controller_get_mode() - Reads the mode of all zones
 *
 * @ctrl: Previously allocated with @aura_controller_create
 * @mode: Buffer to read into
 *
 * @return: Zero or negative error number
 */
error_t aura_controller_get_mode (
    struct aura_controller *ctrl,
    struct lights_mode *mode
){
    struct aura_controller_context *ctx = context_from_ctrl(ctrl);

    if (IS_NULL(ctrl, mode))
        return -EINVAL;

    mutex_lock(&ctx->lock);

    *mode = *ctx->mode;

    mutex_unlock(&ctx->lock);

    return 0;
}
