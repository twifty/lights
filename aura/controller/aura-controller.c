// SPDX-License-Identifier: GPL-2.0
#include <linux/slab.h>
#include <linux/kref.h>

#include <adapter/lights-adapter.h>
#include "aura-controller.h"

#define AURA_APPLY_VAL 0x01

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

    struct lights_thunk             thunk;
};
#define AURA_ZONE_HASH 'ZONE'
#define zone_from_thunk(ptr) ( \
    lights_thunk_container(ptr, struct aura_zone_context, thunk, AURA_ZONE_HASH) \
)
#define zone_from_public(ptr) ( \
    container_of(ptr, struct aura_zone_context, zone) \
)

/**
 * struct aura_controller_context - Storage for a group of zones
 *
 * @ctrl:           Public object
 * @callback_pool:  Reserve of aura_callback_context
 * @lock:           Context spin lock
 * @effect:         Active effect
 * @zone_all:       One zone to rule them all
 * @zone_contexts:  Array of individual zones
 * @effect_colors:  Array of color objects for each zone
 * @direct_colors:  Array of color objects for each zone
 * @is_direct:      Flag to indicate which color array is in use
 * @zone_count:     Number of zones
 * @version:        Version of the control (determines some registers)
 * @lights_client:  Userland access
 * @name:           Interface name
 * @firmware:       Chipset name
 */
struct aura_controller_context {
    struct aura_controller          ctrl;

    struct mutex                    lock;
    struct lights_effect const      *effect;
    struct aura_zone_context        *zone_all;
    struct aura_zone_context        *zone_contexts;
    struct aura_colors              *effect_colors;
    struct aura_colors              *direct_colors;

    /* LightingService reads this array from 0x81C0 upon handshake */
//    struct aura_colors           *unknown_colors;

    uint8_t                         is_direct;
    uint8_t                         zone_count;
    uint8_t                         version;

    struct lights_adapter_client    lights_client;
    struct lights_thunk             thunk;
    char                            *name;
    char                            firmware[32];
};
#define AURA_CTRL_HASH 'CTX'
#define ctrl_from_thunk(ptr) ( \
    lights_thunk_container(ptr, struct aura_controller_context, thunk, AURA_CTRL_HASH) \
)
#define ctrl_from_public(ptr) ( \
    container_of(ptr, struct aura_controller_context, ctrl) \
)

struct lights_effect aura_available_effects[] = {
    LIGHTS_EFFECT_VALUE(AURA_MODE_OFF,       OFF),
    LIGHTS_EFFECT_VALUE(AURA_MODE_STATIC,    STATIC),
    LIGHTS_EFFECT_VALUE(AURA_MODE_BREATHING, BREATHING),
    LIGHTS_EFFECT_VALUE(AURA_MODE_FLASHING,  FLASHING),
    LIGHTS_EFFECT_VALUE(AURA_MODE_CYCLE,     CYCLE),
    LIGHTS_EFFECT_VALUE(AURA_MODE_RAINBOW,   RAINBOW),

    LIGHTS_EFFECT_CUSTOM(AURA_MODE_SPECTRUM_CYCLE_BREATHING,  "spectrum_cycle_breathing"),
    LIGHTS_EFFECT_CUSTOM(AURA_MODE_CHASE_FADE,                "chase_fade"),
    LIGHTS_EFFECT_CUSTOM(AURA_MODE_SPECTRUM_CYCLE_CHASE_FADE, "spectrum_cycle_chase_fade"),
    LIGHTS_EFFECT_CUSTOM(AURA_MODE_CHASE,                     "chase"),
    LIGHTS_EFFECT_CUSTOM(AURA_MODE_SPECTRUM_CYCLE_CHASE,      "spectrum_cycle_chase"),
    LIGHTS_EFFECT_CUSTOM(AURA_MODE_SPECTRUM_CYCLE_WAVE,       "spectrum_cycle_wave"),
    LIGHTS_EFFECT_CUSTOM(AURA_MODE_CHASE_RAINBOW_PULSE,       "chase_rainbow_pulse"),
    LIGHTS_EFFECT_CUSTOM(AURA_MODE_RANDOM_FLICKER,            "random_flicker"),
    LIGHTS_EFFECT_CUSTOM(AURA_MODE_DIRECT,                    "direct"),

    {}
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
struct lights_effect const *aura_controller_get_caps (
    void
){
    return aura_available_effects;
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
 * lights_effect_to_aura_mode() - Converts global to local mode
 *
 * @effect:    Global effect
 * @aura_mode: Target buffer
 *
 * @return: Error code
 */
static inline error_t lights_effect_to_aura_mode (
    struct lights_effect const *effect,
    enum aura_mode *mode
){
    struct lights_effect const *found;

    found = lights_effect_find_by_id(aura_available_effects, effect->id);
    if (found) {
        *mode = found->value;
        return 0;
    }

    return -ENODATA;
    // if (lights_is_custom_mode(mode)) {
    //     *aura_mode = lights_custom_mode_id(mode);
    //     if (*aura_mode <= AURA_MODE_LAST || *aura_mode == AURA_MODE_DIRECT)
    //         return 0;
    //
    //     return -ENODATA;
    // }
    //
    // switch (lights_mode_id(mode)) {
    //     case LIGHTS_MODE_OFF:
    //         *aura_mode = AURA_MODE_OFF;
    //         return 0;
    //     case LIGHTS_MODE_STATIC:
    //         *aura_mode = AURA_MODE_STATIC;
    //         return 0;
    //     case LIGHTS_MODE_BREATHING:
    //         *aura_mode = AURA_MODE_BREATHING;
    //         return 0;
    //     case LIGHTS_MODE_FLASHING:
    //         *aura_mode = AURA_MODE_FLASHING;
    //         return 0;
    //     case LIGHTS_MODE_CYCLE:
    //         *aura_mode = AURA_MODE_CYCLE;
    //         return 0;
    //     case LIGHTS_MODE_RAINBOW:
    //         *aura_mode = AURA_MODE_RAINBOW;
    //         return 0;
    // }
    //
    // return -ENODATA;
}

/**
 * chipset_mode_to_lights_mode() - Converts local to global mode
 *
 * @id: Local mode
 *
 * @return: Global Mode
 */
static inline error_t aura_mode_to_lights_effect (
    enum aura_mode mode,
    struct lights_effect const **effect
){
    *effect = lights_effect_find_by_value(aura_available_effects, mode);
    if (*effect)
        return 0;

    return -ENODATA;

    // struct lights_effect const *found;
    // enum aura_mode aura_mode;
    //
    // lights_for_each_mode(p, aura_available_modes) {
    //     lights_mode_to_aura_mode(p, &aura_mode);
    //     if (aura_mode == id)
    //         return p;
    // }
    //
    // return NULL;
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
            lights_color_read_rbg(&colors->zone[j], &buffer[i]);
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

    lights_thunk_init(&zone[count].thunk, AURA_ZONE_HASH);
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

        lights_thunk_init(&zone[i].thunk, AURA_ZONE_HASH);
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
static void aura_controller_context_destroy (
    struct aura_controller_context *context
){
    lights_adapter_unregister(&context->lights_client);
    kfree(context->zone_contexts);
    kfree(context->direct_colors);
    kfree(context->effect_colors);
    kfree(context->name);
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
struct aura_controller const *aura_controller_create (
    struct lights_adapter_client *client,
    const char *name
){
    struct aura_controller_context *context;
    struct lights_color * color;
    char firmware[17] = {0};
    uint8_t zone_count, mode_id;
    error_t err;
    int i;

    if (IS_NULL(client, name) || IS_TRUE(name[0] == '\0'))
        return ERR_PTR(-EINVAL);

    // AURA_DBG("Reading ZoneCount");
    zone_count = 0;
    err = aura_controller_read_byte(client, 0x80C1, &zone_count);
    if (err) {
        // AURA_DBG(
        //     "Failed to read zone count from '0x%02x' with error %s",
        //     client->i2c_client.addr,
        //     ERR_NAME(err)
        // );
        /* Return NULL rather an error code */
        return NULL;
    }

    if (zone_count == 0 || zone_count >= 8) {
        AURA_DBG("Invalid zone count (%d)", zone_count);
        return NULL;
    }

    // Read the device name and EC version
    // AURA_DBG("Reading ControllerName");
    err = aura_controller_read_block(client, AURA_REG_DEVICE_NAME, firmware, 16);
    if (err) {
        AURA_DBG("Failed to read device firmware name");
        return NULL;
    }

    if (!is_printable(firmware)) {
        AURA_DBG("Device firmware name appears invalid");
        return NULL;
    }

    context = kzalloc(sizeof(*context), GFP_KERNEL);
    if (!context)
        return ERR_PTR(-ENOMEM);

    lights_thunk_init(&context->thunk, AURA_CTRL_HASH);
    memcpy(context->firmware, firmware, 16);
    memcpy(&context->lights_client, client, sizeof(*client));
    mutex_init(&context->lock);

    err = lights_adapter_register(&context->lights_client, 32);
    if (err) {
        AURA_DBG("Failed to register lights_adapter: %s", ERR_NAME(err));
        goto error;
    }

    context->zone_count = zone_count;

    if (strncmp(context->firmware, "AUMA0-E6K5", 10) == 0 || strncmp(context->firmware, "AUDA0-E6K5", 10) == 0)
        context->version = 2;
    else
        context->version = 1;

    AURA_INFO("device '%s' has an %s controller.", name, context->version == 1 ? "EC1" : "EC2");

    // Build the zones
    // AURA_DBG("Creating Zones");
    err = aura_zones_create(context, context->version, zone_count);
    if (err) {
        AURA_ERR("Failed to create zones");
        goto error;
    }

    // Read the configured effect
    err = aura_controller_read_byte(client, AURA_REG_MODE, &mode_id);
    if (err) {
        AURA_ERR("Failed to read device effect");
        goto error;
    }

    err = aura_mode_to_lights_effect(mode_id, &context->effect);
    if (err) {
        AURA_ERR("Failed to translate device effect: 0x%02x", mode_id);
        err = -EIO;
        goto error;
    }

    // Detect if direct effect is applied
    err = aura_controller_read_byte(client, AURA_REG_DIRECT, &context->is_direct);
    if (err) {
        AURA_ERR("Failed to read device is_direct");
        goto error;
    }

    context->name            = kstrdup(name, GFP_KERNEL);
    context->ctrl.name       = context->name;
    context->ctrl.version    = context->version;
    context->ctrl.zone_count = context->zone_count;

    for (i = 0; i < context->zone_count; i++) {
        color = context->is_direct
            ? context->zone_contexts[i].direct
            : context->zone_contexts[i].effect;
        AURA_DBG(
            "Detected zone: %s, color: 0x%06x, effect: %s",
            context->zone_contexts[i].zone.name,
            color->value,
            context->effect->name
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
    struct aura_controller const *ctrl
){
    struct aura_controller_context *context = ctrl_from_public(ctrl);

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
    struct aura_controller const *ctrl,
    struct aura_controller const **slaves,
    size_t count
){
    struct aura_controller_context *context = ctrl_from_public(ctrl);
    struct lights_adapter_client client;
    struct aura_controller const *slave;
    char name[LIGHTS_MAX_FILENAME_LENGTH];
    uint8_t addr, next;
    int found = 0;
    error_t err;

    if (IS_NULL(ctrl, slaves) || IS_TRUE(4 != count))
        return 0;

    if (strlen(context->name) > LIGHTS_MAX_FILENAME_LENGTH - 3) {
        AURA_ERR("Interface name too long");
        return 0;
    }

    snprintf(name, sizeof(name), "%s-%d", context->name, found + 1);
    memcpy(&client, &context->lights_client, sizeof(client));

    for (addr = 0xAA; addr <= 0xAD; ++addr) {
        err = aura_controller_read_byte(&context->lights_client, 0x8000 | addr, &next);
        if (!err && next) {
            LIGHTS_SMBUS_CLIENT_UPDATE(&client, next >> 1);
            slave = aura_controller_create(&client, name);
            if (IS_NULL(slave)) {
                err = -EIO;
                goto error;
            }
            if (!IS_ERR(slave)) {
                slaves[found] = slave;
                found++;
                snprintf(name, sizeof(name), "%s-%d", context->name, found + 1);
                continue;
            }
        }
        break;
    }

    /* Change name of main controller */
    if (found) {
        kfree(context->name);
        snprintf(name, sizeof(name), "%s-%d", context->name, 0);
        context->name = kstrdup(name, GFP_KERNEL);
        context->ctrl.name = context->name;
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
 * aura_controller_color_read() - Reads the color of a single zone
 *
 * @thunk: A struct aura_zone_context
 * @state: Buffer to read into
 *
 * @return: Error code
 */
static error_t aura_controller_color_read (
    struct lights_thunk *thunk,
    struct lights_state *state
){
    struct aura_zone_context *ctx = zone_from_thunk(thunk);

    if (IS_NULL(thunk, state, ctx) || IS_FALSE(state->type & LIGHTS_TYPE_COLOR))
        return -EINVAL;

    return aura_controller_get_zone_color(&ctx->zone, &state->color);
}

/**
 * aura_controller_color_write() - Writes a single zone color
 *
 * @thunk: A struct aura_zone_context
 * @state: Buffer to read from
 *
 * @return: Error code
 */
static error_t aura_controller_color_write (
    struct lights_thunk *thunk,
    struct lights_state const *state
){
    struct aura_zone_context *ctx = zone_from_thunk(thunk);

    if (IS_NULL(thunk, state, ctx) || IS_FALSE(state->type & LIGHTS_TYPE_COLOR))
        return -EINVAL;

    return aura_controller_set_zone_color(&ctx->zone, &state->color);
}

/**
 * aura_controller_effect_read() - Reads the mode of a single zone
 *
 * @thunk: A struct aura_controller_context
 * @state: Buffer to read into
 *
 * @return: Error code
 */
static error_t aura_controller_effect_read (
    struct lights_thunk *thunk,
    struct lights_state *state
){
    struct aura_controller_context *ctx = ctrl_from_thunk(thunk);

    if (IS_NULL(thunk, state, ctx) || IS_FALSE(state->type & LIGHTS_TYPE_EFFECT))
        return -EINVAL;

    return aura_controller_get_effect(&ctx->ctrl, &state->effect);
}

/**
 * aura_controller_effect_write() - Writes a single zone mode
 *
 * @thunk: A struct aura_controller_context
 * @state: Buffer to read from
 *
 * @return: Error code
 */
static error_t aura_controller_effect_write (
    struct lights_thunk *thunk,
    struct lights_state const *state
){
    struct aura_controller_context *ctx = ctrl_from_thunk(thunk);

    if (IS_NULL(thunk, state, ctx) || IS_FALSE(state->type & LIGHTS_TYPE_EFFECT))
        return -EINVAL;

    return aura_controller_set_effect(&ctx->ctrl, &state->effect);
}

/**
 * aura_memory_leds_write() - Writes color values to all zones
 *
 * @thunk: Memory controller
 * @state: Buffer to read from
 *
 * @return: Error code
 */
static error_t aura_controller_leds_write (
    struct lights_thunk *thunk,
    struct lights_state const *state
){
    struct aura_controller_context *ctx = ctrl_from_thunk(thunk);

    if (IS_NULL(thunk, state, ctx) || IS_FALSE(state->type & LIGHTS_TYPE_LEDS))
        return -EINVAL;

    if (state->raw.length != ctx->zone_count)
        return -EINVAL;

    return aura_controller_set_colors(&ctx->ctrl, state->raw.data, ctx->zone_count);
}

/**
 * aura_controller_update_write() - Writes color and mode values to all zones
 *
 * @thunk: Memory controller
 * @state: Buffer to read from
 *
 * @return: Error code
 */
static error_t aura_controller_update_write (
    struct lights_thunk *thunk,
    struct lights_state const *state
){
    struct aura_controller_context *ctx = ctrl_from_thunk(thunk);
    struct lights_effect const *effect = NULL;
    struct lights_color const *color = NULL;

    if (IS_NULL(thunk, state, ctx))
        return -EINVAL;

    if (state->type & LIGHTS_TYPE_EFFECT)
        effect = &state->effect;

    if (state->type & LIGHTS_TYPE_COLOR)
        color = &state->color;

    if (effect) {
        if (color)
            return aura_controller_update(&ctx->ctrl, effect, color);
        return aura_controller_set_effect(&ctx->ctrl, effect);
    } else if (color) {
        return aura_controller_set_colors(&ctx->ctrl, color, 0);
    }

    return -EINVAL;
}

/**
 * aura_controller_register_ctrl() - Creates a lights_fs for a controller
 *
 * @ctrl:   Controller to register
 * @lights: Instance to register
 * @name:   Name of the lights interface
 *
 * @return: Error code
 */
error_t aura_controller_register_ctrl (
    struct aura_controller const *ctrl,
    struct lights_dev *lights,
    const char *name
){
    struct aura_controller_context *ctx = ctrl_from_public(ctrl);
    struct lights_attribute attrs[4];
    error_t err;

    if (IS_NULL(ctrl, lights))
        return -EINVAL;

    lights->name = name ? name : ctx->name;
    lights->caps = aura_controller_get_caps();
    lights->led_count = ctx->zone_count;

    err = lights_device_register(lights);
    if (err)
        return err;

    attrs[0] = LIGHTS_EFFECT_ATTR(
        &ctx->thunk,
        aura_controller_effect_read,
        aura_controller_effect_write
    );
    attrs[1] = LIGHTS_COLOR_ATTR(
        &ctx->zone_all->thunk,
        aura_controller_color_read,
        aura_controller_color_write
    );
    attrs[2] = LIGHTS_LEDS_ATTR(
        &ctx->thunk,
        aura_controller_leds_write
    );
    attrs[3] = LIGHTS_UPDATE_ATTR(
        &ctx->thunk,
        aura_controller_update_write
    );

    err = lights_device_create_files(lights, attrs, ARRAY_SIZE(attrs));
    if (!err)
        return 0;

    lights_device_unregister(lights);

    return err;
}

/**
 * aura_controller_register_zone() - Creates a lights_fs for a zone
 *
 * @zone:   The zone for which to allow userland access
 * @lights: Instance to register
 * @name:   Name of lights interface
 *
 * @return: Error code
 *
 * If @name is given as NULL, the default name of the zone will be used.
 */
error_t aura_controller_register_zone (
    struct aura_zone const *_zone,
    struct lights_dev *lights,
    const char *name
){
    struct aura_zone_context *zone = zone_from_public(_zone);
    struct lights_attribute attrs[3];
    error_t err;

    if (IS_NULL(_zone, lights, zone))
        return -EINVAL;

    lights->name = name ? name : zone->zone.name;
    lights->caps = aura_controller_get_caps();

    err = lights_device_register(lights);
    if (err)
        return err;

    attrs[0] = LIGHTS_EFFECT_ATTR(
        &zone->context->thunk,
        aura_controller_effect_read,
        aura_controller_effect_write
    );
    attrs[1] = LIGHTS_COLOR_ATTR(
        &zone->thunk,
        aura_controller_color_read,
        aura_controller_color_write
    );
    attrs[2] = LIGHTS_UPDATE_ATTR(
        &zone->context->thunk,
        aura_controller_update_write
    );

    err = lights_device_create_files(lights, attrs, ARRAY_SIZE(attrs));
    if (!err)
        return 0;

    lights_device_unregister(lights);

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
struct aura_zone const *aura_controller_get_zone (
    struct aura_controller const *ctrl,
    uint8_t index
){
    struct aura_controller_context *ctx = ctrl_from_public(ctrl);

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
 * @thunk:  Context of the call
 * @error:  A negative error number if async failed
 */
static void aura_controller_set_zone_color_callback (
    struct lights_adapter_msg const * const result,
    struct lights_thunk *thunk,
    error_t error
){
    struct aura_zone_context *zone = zone_from_thunk(thunk);
    struct lights_adapter_msg const * color_msg;
    struct lights_color *target;
    uint16_t delta;
    int i;

    if (IS_NULL(result, thunk, zone))
        return;

    if (error) {
        AURA_DBG("Failed to set color");
        return;
    }

    delta = zone->offset * 3;
    if (zone->context->direct_colors->reg + delta == result->data.word) {
        target = &zone->context->direct_colors->zone[zone->offset];
    } else if (zone->context->effect_colors->reg + delta == result->data.word) {
        target = &zone->context->effect_colors->zone[zone->offset];
    } else {
        AURA_ERR("Failed to detect color target");
        return;
    }

    color_msg = adapter_seek_msg(result, 1);
    if (!color_msg) {
        AURA_ERR("Failed to seek message");
        return;
    }

    if (zone->zone.id == ZONE_ID_ALL) {
        if (color_msg->length != zone->context->zone_count * 3) {
            AURA_ERR("Message has an invalid length '%d'", color_msg->length);
            return;
        }
    } else if (color_msg->length != 3) {
        AURA_ERR("Message has an invalid length '%d'", color_msg->length);
        return;
    }

    mutex_lock(&zone->context->lock);

    if (zone->zone.id == ZONE_ID_ALL) {
        for (i = 0; i < zone->context->zone_count; i++)
            lights_color_read_rbg(&target[i], &color_msg->data.block[i * 3]);
    } else {
        lights_color_read_rbg(target, color_msg->data.block);
    }

    mutex_unlock(&zone->context->lock);
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
    struct aura_zone const *_zone,
    struct lights_color const *color
){
    struct aura_zone_context *zone = zone_from_public(_zone);
    struct aura_controller_context *context;
    struct lights_adapter_msg msgs[4];
    uint16_t target;
    error_t err;
    int count = 0, i;

    if (IS_NULL(_zone, color, zone))
        return -EINVAL;

    context = zone->context;

    if (context->is_direct)
        target = context->direct_colors->reg;
    else
        target = context->effect_colors->reg;

    if (zone->zone.id == ZONE_ID_ALL) {
        msgs[0] = ADAPTER_WRITE_WORD_DATA_SWAPPED(CMD_SET_ADDR, target);
        msgs[1] = ADAPTER_WRITE_BLOCK_DATA(CMD_WRITE_BLOCK, zone->context->zone_count * 3);
        for (i = 0; i < zone->context->zone_count; i++)
            lights_color_write_rbg(color, &msgs[1].data.block[i * 3]);
    } else {
        msgs[0] = ADAPTER_WRITE_WORD_DATA_SWAPPED(CMD_SET_ADDR, target + (3 * zone->offset));
        msgs[1] = ADAPTER_WRITE_BLOCK_DATA(CMD_WRITE_BLOCK, 3);
        lights_color_write_rbg(color, msgs[1].data.block);
    }

    AURA_DBG("Applying color 0x%06x to '%s' zone '%s'", color->value, context->name, zone->zone.name);

    if (!context->is_direct) {
        msgs[2] = ADAPTER_WRITE_WORD_DATA_SWAPPED(CMD_SET_ADDR, AURA_REG_APPLY);
        msgs[3] = ADAPTER_WRITE_BYTE_DATA(CMD_WRITE_BYTE, AURA_APPLY_VAL);
        count = 4;
    } else {
        count = 2;
    }

    err = lights_adapter_xfer_async(
        &context->lights_client,
        msgs,
        count,
        &zone->thunk,
        aura_controller_set_zone_color_callback
    );

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
    struct aura_zone const *zone,
    struct lights_color *color
){
    struct aura_zone_context *zone_ctx = zone_from_public(zone);
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
 * @thunk:  Context of the call
 * @error:  A negative error number if async failed
 */
static void aura_controller_set_color_callback (
    struct lights_adapter_msg const * const result,
    struct lights_thunk *thunk,
    error_t error
){
    struct aura_controller_context *ctrl = ctrl_from_thunk(thunk);
    struct lights_adapter_msg const * color_msg;
    struct lights_color *target;
    int i;

    if (IS_NULL(result, thunk, ctrl))
        return;

    if (error) {
        AURA_DBG("Failed to set color");
        return;
    }

    if (result->data.word == ctrl->direct_colors->reg) {
        target = ctrl->direct_colors->zone;
    } else if (result->data.word == ctrl->effect_colors->reg) {
        target = ctrl->effect_colors->zone;
    } else {
        AURA_ERR("Failed to detect color target");
        return;
    }

    color_msg = adapter_seek_msg(result, 1);
    if (!color_msg) {
        AURA_ERR("Failed to seek message");
        return;
    }

    if (color_msg->length != ctrl->zone_count * 3) {
        AURA_ERR("Message has an invalid length '%d'", color_msg->length);
        return;
    }

    mutex_lock(&ctrl->lock);

    for (i = 0; i <= ctrl->zone_count; i++) {
        lights_color_read_rbg(&target[i], &color_msg->data.block[i * 3]);
    }

    mutex_unlock(&ctrl->lock);
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
    struct aura_controller const *_ctrl,
    struct lights_color const * const colors,
    uint8_t count
){
    struct aura_controller_context *ctrl = ctrl_from_public(_ctrl);
    struct lights_adapter_msg msgs[4];
    struct lights_color const *color;
    uint16_t target;
    int i;
    error_t err;

    if (IS_NULL(_ctrl, colors, ctrl) || IS_FALSE(count == 1 || count == ctrl->zone_count))
        return -EINVAL;

    if (ctrl->is_direct)
        target = ctrl->direct_colors->reg;
    else
        target = ctrl->effect_colors->reg;

    msgs[0] = ADAPTER_WRITE_WORD_DATA_SWAPPED(CMD_SET_ADDR, target);
    msgs[1] = ADAPTER_WRITE_BLOCK_DATA(CMD_WRITE_BLOCK, ctrl->zone_count * 3);

    color = colors;
    for (i = 0; i < ctrl->zone_count; i++) {
        lights_color_write_rbg(color, &msgs[1].data.block[i * 3]);

        if (count > 1)
            color++;
    }

    if (!ctrl->is_direct) {
        msgs[2] = ADAPTER_WRITE_WORD_DATA_SWAPPED(CMD_SET_ADDR, AURA_REG_APPLY);
        msgs[3] = ADAPTER_WRITE_BYTE_DATA(CMD_WRITE_BYTE, AURA_APPLY_VAL);
        count = 4;
    } else {
        count = 2;
    }

    AURA_DBG("Applying color 0x%06x to '%s' all zones", color->value, ctrl->name);

    err = lights_adapter_xfer_async(
        &ctrl->lights_client,
        msgs,
        count,
        &ctrl->thunk,
        aura_controller_set_color_callback
    );

    return err;
}


/**
 * aura_controller_set_zeffect_callback() - Async handler for mode setting
 *
 * @result: Messages sent to the device
 * @thunk:  Context of the call
 * @error:  A negative error number if async failed
 */
static void aura_controller_set_effect_callback (
    struct lights_adapter_msg const * const result,
    struct lights_thunk *thunk,
    error_t error
){
    struct aura_controller_context *ctrl = ctrl_from_thunk(thunk);
    struct lights_adapter_msg const *mode_msg;
    struct lights_effect const *lights_effect;
    enum aura_mode aura_mode;

    if (IS_NULL(result, thunk, ctrl))
        return;

    if (error) {
        AURA_DBG("Failed to set mode");
        return;
    }

    mode_msg = adapter_seek_msg(result, 1);
    if (!mode_msg) {
        AURA_ERR("Failed to seek message");
        return;
    }

    aura_mode = mode_msg->data.byte;
    if (aura_mode_to_lights_effect(aura_mode, &lights_effect)) {
        AURA_ERR("Message contains an invalid mode '0x%02x'", aura_mode);
        return;
    }

    mutex_lock(&ctrl->lock);

    if (aura_mode == AURA_MODE_DIRECT) {
        ctrl->is_direct = true;
    } else {
        ctrl->effect = lights_effect;
    }

    mutex_unlock(&ctrl->lock);
}

/**
 * aura_controller_set_effect() - Applies a mode to all zones
 *
 * @ctrl:  Previously allocated with @aura_controller_create
 * @color: Mode to apply
 *
 * @return: Zero or negative error number
 *
 * NOTE: A single zone cannot have its own mode.
 */
error_t aura_controller_set_effect (
    struct aura_controller const *ctrl,
    struct lights_effect const *effect
){
    struct aura_controller_context *context = ctrl_from_public(ctrl);
    enum aura_mode aura_mode;
    error_t err;
    int count = 0;

    struct lights_adapter_msg msgs[4];

    if (IS_NULL(ctrl, effect))
        return -EINVAL;
    if (IS_NULL(context, context->effect))
        return -EINVAL;

    err = lights_effect_to_aura_mode(effect, &aura_mode);
    if (err)
        return err;

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
        if (effect->id != context->effect->id) {
            msgs[count]     = ADAPTER_WRITE_WORD_DATA_SWAPPED(CMD_SET_ADDR, AURA_REG_MODE);
            msgs[count + 1] = ADAPTER_WRITE_BYTE_DATA(CMD_WRITE_BYTE, aura_mode);
            count += 2;
        }
    }

    if (count) {
        msgs[count]     = ADAPTER_WRITE_WORD_DATA_SWAPPED(CMD_SET_ADDR, AURA_REG_APPLY);
        msgs[count + 1] = ADAPTER_WRITE_BYTE_DATA(CMD_WRITE_BYTE, AURA_APPLY_VAL);
        count += 2;

        // AURA_DBG("Queing %d messages to update mode", count);
        err = lights_adapter_xfer_async(
            &context->lights_client,
            msgs,
            count,
            &context->thunk,
            aura_controller_set_effect_callback
        );
    }

    return err;
}

/**
 * aura_controller_get_effect() - Reads the mode of all zones
 *
 * @ctrl:   Previously allocated with @aura_controller_create
 * @effect: Buffer to read into
 *
 * @return: Zero or negative error number
 */
error_t aura_controller_get_effect (
    struct aura_controller const *ctrl,
    struct lights_effect *effect
){
    struct aura_controller_context *ctx = ctrl_from_public(ctrl);

    if (IS_NULL(ctrl, effect))
        return -EINVAL;

    mutex_lock(&ctx->lock);

    *effect = *ctx->effect;

    mutex_unlock(&ctx->lock);

    return 0;
}


/**
 * aura_controller_update_callback() - Async handler for update
 *
 * @result: Messages sent to the device
 * @thunk:  Context of the call
 * @error:  A negative error number if async failed
 */
static void aura_controller_update_callback (
    struct lights_adapter_msg const * const result,
    struct lights_thunk *thunk,
    error_t error
){
    struct aura_controller_context *context = ctrl_from_thunk(thunk);
    struct lights_adapter_msg const *msg;
    struct aura_colors *target;
    struct lights_effect const *lights_effect = NULL;
    enum aura_mode aura_mode;
    bool is_direct = false;
    int i;

    if (IS_NULL(result, thunk, context))
        return;

    if (error) {
        AURA_DBG("Failed to update");
        return;
    }

    msg = result;
    if (msg->data.word == AURA_REG_DIRECT) {
        msg = msg->next;
        if (msg) {
            is_direct = msg->data.byte;
            msg = msg->next;
        }
    }

    if (!is_direct) {
        target = context->effect_colors;

        if (msg && msg->data.word == AURA_REG_MODE) {
            msg = msg->next;
            if (msg) {
                aura_mode = msg->data.byte;
                msg = msg->next;

                if (aura_mode_to_lights_effect(aura_mode, &lights_effect)) {
                    AURA_ERR("Message contains an invalid effect '0x%02x'", aura_mode);
                    return;
                }
            }
        }
    } else {
        target = context->direct_colors;
    }

    msg = adapter_seek_msg(msg, 1);
    if (msg && msg->length == context->zone_count * 3) {
        mutex_lock(&context->lock);

        context->is_direct = is_direct;

        for (i = 0; i <= context->zone_count; i++)
            lights_color_read_rbg(&target->zone[i], &msg->data.block[i * 3]);

        if (lights_effect)
            context->effect = lights_effect;

        mutex_unlock(&context->lock);
    } else {
        AURA_ERR("Failed to find color array in messages");
    }
}

/**
 * aura_controller_update() - Writes a mode and color to all zones
 *
 * @ctrl:   Previously allocated with aura_controller_create()
 * @effect: New mode to apply
 * @color:  New color to apply
 *
 * @return: Error code
 */
error_t aura_controller_update (
    struct aura_controller const *ctrl,
    struct lights_effect const *effect,
    struct lights_color const *color
){
    struct aura_controller_context *context = ctrl_from_public(ctrl);
    enum aura_mode aura_mode;
    struct lights_adapter_msg msgs[8];
    uint16_t target;
    size_t count = 0;
    int i;
    error_t err;

    if (IS_NULL(ctrl, effect, color))
        return -EINVAL;

    err = lights_effect_to_aura_mode(effect, &aura_mode);
    if (err)
        return err;

    if (aura_mode == AURA_MODE_DIRECT) {
        target = context->direct_colors->reg;
        if (!context->is_direct) {
            count = 2;
            msgs[0] = ADAPTER_WRITE_WORD_DATA_SWAPPED(CMD_SET_ADDR, AURA_REG_DIRECT);
            msgs[1] = ADAPTER_WRITE_BYTE_DATA(CMD_WRITE_BYTE, 0x01);
        }
    } else {
        target = context->effect_colors->reg;
        if (context->is_direct) {
            count = 2;
            msgs[0] = ADAPTER_WRITE_WORD_DATA_SWAPPED(CMD_SET_ADDR, AURA_REG_DIRECT);
            msgs[1] = ADAPTER_WRITE_BYTE_DATA(CMD_WRITE_BYTE, 0x00);
        }
        if (effect->id != context->effect->id) {
            msgs[count]     = ADAPTER_WRITE_WORD_DATA_SWAPPED(CMD_SET_ADDR, AURA_REG_MODE);
            msgs[count + 1] = ADAPTER_WRITE_BYTE_DATA(CMD_WRITE_BYTE, aura_mode);
            count += 2;
        }
    }

    msgs[count    ] = ADAPTER_WRITE_WORD_DATA_SWAPPED(CMD_SET_ADDR, target);
    msgs[count + 1] = ADAPTER_WRITE_BLOCK_DATA(CMD_WRITE_BLOCK, context->zone_count * 3);
    for (i = 0; i < context->zone_count; i++) {
        lights_color_write_rbg(color, &msgs[count + 1].data.block[i * 3]);
    }
    count += 2;

    if (!context->is_direct) {
        msgs[count    ] = ADAPTER_WRITE_WORD_DATA_SWAPPED(CMD_SET_ADDR, AURA_REG_APPLY);
        msgs[count + 1] = ADAPTER_WRITE_BYTE_DATA(CMD_WRITE_BYTE, AURA_APPLY_VAL);
        count += 2;
    }

    err = lights_adapter_xfer_async(
        &context->lights_client,
        msgs,
        count,
        &context->thunk,
        aura_controller_update_callback
    );

    return err;
}
