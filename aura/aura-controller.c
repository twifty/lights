// SPDX-License-Identifier: GPL-2.0
#include "aura-controller.h"

#define AURA_APPLY_VAL              0x01

enum aura_registers {
    AURA_REG_DEVICE_NAME =                  0x1000,   /* Device String 16 bytes               */
    AURA_REG_COLORS_DIRECT_EC1 =            0x8000,   /* Colors for Direct Mode 15 bytes      */
    AURA_REG_COLORS_EFFECT_EC1 =            0x8010,   /* Colors for Internal Effects 15 bytes */
    AURA_REG_DIRECT =                       0x8020,   /* "Direct Access" Selection Register   */
    AURA_REG_MODE =                         0x8021,   /* AURA Mode Selection Register         */
    AURA_REG_APPLY =                        0x80A0,   /* AURA Apply Changes Register          */
    AURA_REG_ZONE_ID =                      0x80E0,   /* An array of zone IDs                 */
    AURA_REG_SLOT_INDEX =                   0x80F8,   /* AURA Slot Index Register (RAM only)  */
    AURA_REG_I2C_ADDRESS =                  0x80F9,   /* AURA I2C Address Register (RAM only) */
    AURA_REG_COLORS_DIRECT_EC2 =            0x8100,   /* Direct Colors (v2) 30 bytes          */
    AURA_REG_COLORS_EFFECT_EC2 =            0x8160,   /* Internal Colors (v2) 30 bytes        */
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

#define byte_swap(x) ((x << 8) & 0xFF00) | ((x >> 8) & 0x00FF)


struct aura_colors {
    uint16_t                        reg;              /* A chipset specific reg address for the color bytes */
    uint8_t                         count;            /* The count of zones */
    struct lights_color             zone[];           /* The currently configured colors for each zone */
};

struct aura_zone_context {
    struct aura_zone                zone;

    uint8_t                         offset;            /* The offset into the color arrays */
    struct lights_color             *effect;           /* A pointer into the ctrl->effect_colors->zone array */
    struct lights_color             *direct;           /* A pointer into the ctrl->direct_colors->zone array */
    struct aura_controller_context  *ctrl_ctx;         /* A pointer back to the owning controller */
};

struct aura_controller_context {
    struct aura_controller          ctrl;

    struct mutex                    lock;             /* Read/Write access lock */
    const struct lights_mode        *mode;            /* The current mode, only applied if non direct */
    struct aura_zone_context        *zone_all;        /* A special zone used to represent all the others */
    struct aura_zone_context        *zone_contexts;   /* Data for each available zone, plus an extra representing all */
    struct aura_colors              *effect_colors;   /* Each zones effect color cache */
    struct aura_colors              *direct_colors;   /* Each zones direct color cache */
//    struct aura_colors           *unknown_colors;    /* LightingService reads this array from 0x81C0 upon handshake */
    uint8_t                         *color_buffer;    /* A buffer for writing color bytes to for reading/writing to i2c */
    uint8_t                         is_direct;        /* Flag to indicate if direct mode is enabled */
    uint8_t                         zone_count;
    uint8_t                         version;

    struct i2c_client               i2c_client;
    char                            name[32];
};

#define to_aura_context(ptr)\
    container_of(ptr, struct aura_controller_context, ctrl)

#define to_zone_context(ptr)\
    container_of(ptr, struct aura_zone_context, zone)

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

enum {
    CMD_SET_ADDR    = 0x00,
    CMD_READ_BYTE   = 0x81,
    CMD_WRITE_BYTE  = 0x01,
    CMD_READ_WORD   = 0x82,
    CMD_WRITE_WORD  = 0x02,
    CMD_READ_BLOCK  = 0x80,
    CMD_WRITE_BLOCK = 0x03 // TODO: This value looks very wrong
};

const struct lights_mode *aura_controller_get_caps (
    void
){
    return aura_available_modes;
}

static error_t aura_controller_read_byte_locked (
    struct aura_controller_context *ctx,
    uint16_t reg,
    uint8_t *value
){
    error_t err;

    err = i2c_smbus_write_word_swapped(&ctx->i2c_client, CMD_SET_ADDR, reg);
    if (!err) {
        err = i2c_smbus_read_byte_data(&ctx->i2c_client, CMD_READ_BYTE);
        if (err >= 0) {
            *value = (err & 0xFF);
            return 0;
        }
    }

    return err;
}

static error_t aura_controller_write_byte_locked (
    struct aura_controller_context *ctx,
    uint16_t reg,
    uint8_t value
){
    error_t err;

    err = i2c_smbus_write_word_swapped(&ctx->i2c_client, CMD_SET_ADDR, reg);
    if (!err)
        err = i2c_smbus_write_byte_data(&ctx->i2c_client, CMD_WRITE_BYTE, value);

    return err;
}

static error_t aura_controller_read_word_locked (
    struct aura_controller_context *ctx,
    uint16_t reg,
    uint16_t *value
){
    error_t err;

    err = i2c_smbus_write_word_swapped(&ctx->i2c_client, CMD_SET_ADDR, reg);
    if (!err) {
        err = i2c_smbus_read_word_swapped(&ctx->i2c_client, CMD_READ_WORD);
        if (err >= 0) {
            *value = (err & 0xFFFF);
            return 0;
        }
    }

    return err;
}

static error_t aura_controller_write_word_locked (
    struct aura_controller_context *ctx,
    uint16_t reg,
    uint8_t value
){
    error_t err;

    err = i2c_smbus_write_word_swapped(&ctx->i2c_client, CMD_SET_ADDR, reg);
    if (!err)
        err = i2c_smbus_write_word_swapped(&ctx->i2c_client, CMD_WRITE_WORD, value);

    return err;
}

static error_t aura_controller_read_block_locked (
    struct aura_controller_context *ctx,
    uint16_t reg,
    uint8_t *data,
    uint8_t size
){
    error_t err;
    uint8_t i;

    if (size == 0 || size > I2C_SMBUS_BLOCK_MAX)
        return -EINVAL;

    err = i2c_smbus_write_word_swapped(&ctx->i2c_client, CMD_SET_ADDR, reg);
    if (!err) {
        err = i2c_smbus_read_block_data(&ctx->i2c_client, CMD_READ_BLOCK + size, data);
        if (err > 0) {
            return 0;
        } else if (err < 0) {
            /*
                Some adapters don't support the I2C_FUNC_SMBUS_READ_BLOCK_DATA
                protocol, so we will resort to single byte reads.
            */
           err = 0;
           for (i = 0; i < size && err > 0; i++) {
               err = aura_controller_read_byte_locked(ctx, reg + i, &data[i]);
           }
       } else {
           /* 0 bytes read */
           return -EIO;
       }
    }

    return err;
}

static error_t aura_controller_write_block_locked (
    struct aura_controller_context *ctx,
    uint16_t reg,
    uint8_t *data,
    uint8_t size
){
    error_t err;

    if (size == 0 || size > I2C_SMBUS_BLOCK_MAX)
        return -EINVAL;

    err = i2c_smbus_write_word_swapped(&ctx->i2c_client, CMD_SET_ADDR, reg);
    if (!err)
        err = i2c_smbus_write_block_data(&ctx->i2c_client, CMD_WRITE_BLOCK, size, data);

    return err;
}


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

inline void apply_color_to_buffer (
    uint8_t *buffer,
    const struct lights_color *color
){
    buffer[0] = color->r;
    buffer[1] = color->b;
    buffer[2] = color->g;
}

static error_t aura_controller_init_colors (
    struct aura_controller_context *ctx,
    uint8_t count,
    struct aura_colors *colors
){
    uint8_t *buf, size;
    int i, j;
    error_t err;

    size = count * 3;
    buf = kmalloc(size, GFP_KERNEL);
    if (!buf)
        return -ENOMEM;

    err = aura_controller_read_block_locked(ctx, colors->reg, buf, size);
    if (!err) {
        for (i = 0, j = 0; i < size; i += 3, j++) {
            colors->zone[j].r = buf[i + 0];
            colors->zone[j].b = buf[i + 1];
            colors->zone[j].g = buf[i + 2];
        }
    } else {
        AURA_DBG("aura_controller_read_block() failed with %d", err);
    }

    kfree(buf);

    return err;
}

static struct aura_colors *create_colors (
    struct aura_controller_context *ctx,
    uint8_t count,
    uint16_t reg,
    uint8_t read
){
    struct aura_colors *colors;
    error_t err;

    if (!ctx || 0 == count) {
        AURA_ERR("create_colors() called with NULL pointer(s)");
        return ERR_PTR(-EINVAL);
    }

    colors = kmalloc(sizeof(struct aura_colors) + sizeof(struct lights_color) * count, GFP_KERNEL);
    if (!colors)
        return ERR_PTR(-ENOMEM);

    colors->count = count;
    colors->reg = reg;

    if (read) {
        err = aura_controller_init_colors(ctx, count, colors);
        if (err) {
            AURA_DBG("aura_controller_init_colors() failed with %d", err);
            kfree(colors);
            return ERR_PTR(err);
        }
    }

    return colors;
}

static error_t aura_create_zones (
    struct aura_controller_context *ctx,
    uint8_t version,
    uint8_t count
){
    struct aura_zone_context *zone;
    struct aura_colors *effect_colors;
    struct aura_colors *direct_colors;
    int size, i;
    error_t err;
    uint8_t zone_id;

    if (!ctx || 0 == count) {
        AURA_ERR("aura_create_zones() called with NULL pointer(s)");
        return -EINVAL;
    }

    // Read the effect colors
    effect_colors = create_colors(ctx, count, version == 1 ? AURA_REG_COLORS_EFFECT_EC1 : AURA_REG_COLORS_EFFECT_EC2, 1);
    if (IS_ERR(effect_colors)) {
        AURA_DBG("create_colors() for effect failed with %ld", PTR_ERR(effect_colors));
        return PTR_ERR(effect_colors);
    }

    // Allocate the direct colors
    direct_colors = create_colors(ctx, count, version == 1 ? AURA_REG_COLORS_DIRECT_EC1 : AURA_REG_COLORS_DIRECT_EC2, 0);
    if (IS_ERR(direct_colors)) {
        AURA_DBG("create_colors() for direct failed with %ld", PTR_ERR(effect_colors));
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
    ctx->zone_all = &zone[count];
    zone[count].zone.id     = ZONE_ID_ALL;
    zone[count].zone.name   = "all";
    zone[count].zone.ctrl   = &ctx->ctrl;
    zone[count].ctrl_ctx    = ctx;
    zone[count].offset      = 0;
    zone[count].effect      = &effect_colors->zone[0];
    zone[count].direct      = &direct_colors->zone[0];

    for (i = 0; i < count; i++) {
        err = aura_controller_read_byte_locked(ctx, AURA_REG_ZONE_ID + i, &zone_id);
        if (err || zone_id >= ARRAY_SIZE(zone_names))
            goto error_free_zone;

        zone[i].zone.id     = zone_id;
        zone[i].zone.name   = zone_names[zone_id];
        zone[i].zone.ctrl   = &ctx->ctrl;
        zone[i].ctrl_ctx    = ctx;
        zone[i].offset      = i;
        zone[i].effect      = &effect_colors->zone[i];
        zone[i].direct      = &direct_colors->zone[i];

        AURA_DBG("detected zone: %s", zone[i].zone.name);
    }

    ctx->effect_colors  = effect_colors;
    ctx->direct_colors  = direct_colors;
    ctx->zone_count     = count;
    ctx->zone_contexts = zone;

    return 0;

error_free_zone:
    kfree(zone);
error_free_colors:
    kfree(effect_colors);
    kfree(direct_colors);

    return err;
}


struct aura_controller *aura_controller_create (
    struct i2c_adapter *adap,
    uint8_t addr
){
    struct aura_controller_context *context;
    uint8_t zone_count, mode_id;
    error_t err;

    if (WARN_ON(NULL == adap))
        return ERR_PTR(-ENODEV);

    context = kzalloc(sizeof(*context), GFP_KERNEL);
    if (!context)
        return ERR_PTR(-ENOMEM);

    // Setup the client
    mutex_init(&context->lock);
    context->i2c_client.adapter = adap;
    context->i2c_client.addr = addr;

    // AURA_DBG("Reading ZoneCount");
    zone_count = 0;
    err = aura_controller_read_byte_locked(context, 0x80C1, &zone_count);
    if (err)
        goto error;

    if (zone_count == 0 || zone_count >= 8) {
        AURA_DBG("Invalid zone count (%d)", zone_count);
        err = -ENODEV;
        goto error;
    }

    // AURA_DBG("Allocating color buffer for %d zones", zone_count);
    context->zone_count = zone_count;
    context->color_buffer = kmalloc(zone_count * 3, GFP_KERNEL);
    if (!context->color_buffer) {
        err = -ENOMEM;
        goto error;
    }

    // Read the device name and EC version
    // AURA_DBG("Reading ControllerName");
    err = aura_controller_read_block_locked(context, AURA_REG_DEVICE_NAME, context->name, 16);
    if (err) {
        AURA_DBG("Failed to read device name");
        goto error;
    }

    if (strncmp(context->name, "AUMA0-E6K5", 10) == 0 || strncmp(context->name, "AUDA0-E6K5", 10) == 0)
        context->version = 2;
    else
        context->version = 1;

    AURA_DBG("device '%s' has an %s controller.", context->name, context->version == 1 ? "EC1" : "EC2");

    // Build the zones
    // AURA_DBG("Creating Zones");
    err = aura_create_zones(context, context->version, zone_count);
    if (err) {
        AURA_ERR("Failed to create zones");
        goto error;
    }

    // Read the configured mode
    err = aura_controller_read_byte_locked(context, AURA_REG_MODE, &mode_id);
    if (err) {
        AURA_DBG("Failed to read device mode");
        goto error;
    }

    if (NULL == (context->mode = chipset_mode_to_lights_mode(mode_id))) {
        AURA_DBG("Failed to translate device mode: 0x%02x", mode_id);
        err = -EIO;
        goto error;
    }

    // Detect if direct mode is applied
    err = aura_controller_read_byte_locked(context, AURA_REG_DIRECT, &context->is_direct);
    if (err) {
        AURA_DBG("Failed to read device is_direct");
        goto error;
    }

    context->ctrl.name       = context->name;
    context->ctrl.version    = context->version;
    context->ctrl.zone_count = context->zone_count;

    return &context->ctrl;

error:
    if (context->color_buffer)
        kfree(context->color_buffer);
    if (context->zone_contexts)
        kfree(context->zone_contexts);
    if (context->direct_colors)
        kfree(context->direct_colors);
    if (context->effect_colors)
        kfree(context->effect_colors);
    kfree(context);

    return ERR_PTR(err);
}

struct aura_zone *aura_controller_get_zone (
    struct aura_controller *ctrl,
    uint8_t id
){
    struct aura_controller_context *ctx = to_aura_context(ctrl);

    if (WARN_ON(NULL == ctrl)) {
        AURA_ERR("NULL ptr detected");
        return ERR_PTR(-EINVAL);
    }

    if (id < ctx->zone_count)
        return &ctx->zone_contexts[id].zone;

    if (id == ZONE_ID_ALL)
        return &ctx->zone_all->zone;

    return ERR_PTR(-EINVAL);
}

void aura_controller_destroy (
    struct aura_controller *ctrl
){
    struct aura_controller_context *ctx = to_aura_context(ctrl);

    if (WARN_ON(NULL == ctrl))
        return;

    if (ctx->color_buffer)
        kfree(ctx->color_buffer);
    if (ctx->zone_contexts)
        kfree(ctx->zone_contexts);
    if (ctx->direct_colors)
        kfree(ctx->direct_colors);
    if (ctx->effect_colors)
        kfree(ctx->effect_colors);
    kfree(ctx);
}


error_t aura_controller_read_byte (
    struct aura_controller *ctrl,
    uint16_t reg,
    uint8_t *value
){
    struct aura_controller_context *ctx = to_aura_context(ctrl);
    error_t err;

    if (WARN_ON(NULL == ctrl || NULL == value)) {
        AURA_ERR("NULL ptr detected");
        return -EINVAL;
    }

    mutex_lock(&ctx->lock);
    err = aura_controller_read_byte_locked(ctx, reg, value);
    mutex_unlock(&ctx->lock);

    return err;
}

error_t aura_controller_write_byte (
    struct aura_controller *ctrl,
    uint16_t reg,
    uint8_t value
){
    struct aura_controller_context *ctx = to_aura_context(ctrl);
    error_t err;

    if (WARN_ON(NULL == ctrl)) {
        AURA_ERR("NULL ptr detected");
        return -EINVAL;
    }

    mutex_lock(&ctx->lock);
    err = aura_controller_write_byte_locked(ctx, reg, value);
    mutex_unlock(&ctx->lock);

    return err;
}

error_t aura_controller_read_word (
    struct aura_controller *ctrl,
    uint16_t reg,
    uint16_t *value
){
    struct aura_controller_context *ctx = to_aura_context(ctrl);
    error_t err;

    if (WARN_ON(NULL == ctrl || NULL == value)) {
        AURA_ERR("NULL ptr detected");
        return -EINVAL;
    }

    mutex_lock(&ctx->lock);
    err = aura_controller_read_word_locked(ctx, reg, value);
    mutex_unlock(&ctx->lock);

    return err;
}

error_t aura_controller_write_word (
    struct aura_controller *ctrl,
    uint16_t reg,
    uint8_t value
){
    struct aura_controller_context *ctx = to_aura_context(ctrl);
    error_t err;

    if (WARN_ON(NULL == ctrl)) {
        AURA_ERR("NULL ptr detected");
        return -EINVAL;
    }

    mutex_lock(&ctx->lock);
    err = aura_controller_write_word_locked(ctx, reg, value);
    mutex_unlock(&ctx->lock);

    return err;
}

error_t aura_controller_read_block (
    struct aura_controller *ctrl,
    uint16_t reg,
    uint8_t *data,
    uint8_t size
){
    struct aura_controller_context *ctx = to_aura_context(ctrl);
    error_t err;

    if (WARN_ON(NULL == ctrl || NULL == data)) {
        AURA_ERR("NULL ptr detected");
        return -EINVAL;
    }

    mutex_lock(&ctx->lock);
    err = aura_controller_read_block_locked(ctx, reg, data, size);
    mutex_unlock(&ctx->lock);

    return err;
}

error_t aura_controller_write_block (
    struct aura_controller *ctrl,
    uint16_t reg,
    uint8_t *data,
    uint8_t size
){
    struct aura_controller_context *ctx = to_aura_context(ctrl);
    error_t err;

    if (WARN_ON(NULL == ctrl || NULL == data)) {
        AURA_ERR("NULL ptr detected");
        return -EINVAL;
    }

    mutex_lock(&ctx->lock);
    err = aura_controller_write_block_locked(ctx, reg, data, size);
    mutex_unlock(&ctx->lock);

    return err;
}


error_t aura_controller_set_zone_color (
    struct aura_zone *zone,
    const struct lights_color *color
){
    struct aura_zone_context *zone_ctx = to_zone_context(zone);
    struct aura_controller_context *ctrl_ctx;
    struct aura_colors *target;
    error_t err;

    if (WARN_ON(NULL == zone || NULL == color)) {
        AURA_ERR("NULL ptr detected");
        return -EINVAL;
    }

    ctrl_ctx = zone_ctx->ctrl_ctx;
    // if (zone_id >= ctrl_ctx->zone_count) {
    //     AURA_ERR("Invalid zone id");
    //     return -EINVAL;
    // }

    // zone = &ctrl_ctx->zones[zone_id];

    mutex_lock(&ctrl_ctx->lock);

    if (ctrl_ctx->is_direct)
        target = ctrl_ctx->direct_colors;
    else
        target = ctrl_ctx->effect_colors;

    apply_color_to_buffer(ctrl_ctx->color_buffer, color);
    err = aura_controller_write_block_locked(ctrl_ctx, target->reg + (3 * zone_ctx->offset), ctrl_ctx->color_buffer, 3);
    if (err)
        return err;

    if (!ctrl_ctx->is_direct) {
        err = aura_controller_write_byte_locked(ctrl_ctx, AURA_REG_APPLY, AURA_APPLY_VAL);
        if (err)
            return err;
    }

    target->zone[zone_ctx->offset] = *color;
    mutex_unlock(&ctrl_ctx->lock);

    return 0;
}

error_t aura_controller_get_zone_color (
    struct aura_zone *zone,
    struct lights_color *color
){
    struct aura_zone_context *zone_ctx = to_zone_context(zone);
    struct aura_controller_context *ctrl_ctx;
    // struct aura_zone_context *zone;

    if (WARN_ON(NULL == zone || NULL == color)) {
        AURA_ERR("NULL ptr detected");
        return -EINVAL;
    }

    ctrl_ctx = zone_ctx->ctrl_ctx;
    // if (zone_id >= ctx->zone_count) {
    //     AURA_ERR("Invalid zone id");
    //     return -EINVAL;
    // }

    // zone = &ctx->zones[zone_id];

    if (zone_ctx == ctrl_ctx->zone_all) {
        AURA_DBG("zone 'all' cannot return a color");
        return -EIO;
    }

    mutex_lock(&ctrl_ctx->lock);

    if (ctrl_ctx->is_direct) {
        *color = *zone_ctx->direct;
    } else {
        *color = *zone_ctx->effect;
    }

    mutex_unlock(&ctrl_ctx->lock);

    return 0;
}

error_t aura_controller_set_color (
    struct aura_controller *ctrl,
    const struct lights_color *color
){
    struct aura_controller_context *ctx = to_aura_context(ctrl);
    struct aura_colors *target;
    error_t err, i;

    if (WARN_ON(NULL == ctrl || NULL == color)) {
        AURA_ERR("NULL ptr detected");
        return -EINVAL;
    }

    AURA_DBG("setting color: 0x%02x%02x%02x to all zones", color->r, color->g, color->b);

    mutex_lock(&ctx->lock);

    if (ctx->is_direct)
        target = ctx->direct_colors;
    else
        target = ctx->effect_colors;

    for (i = 0; i < ctx->zone_count; i++)
        apply_color_to_buffer(&ctx->color_buffer[i * 3], color);

    AURA_DBG("reg: 0x%04x", target->reg);
    err = aura_controller_write_block_locked(ctx, target->reg, ctx->color_buffer, ctx->zone_count * 3);
    if (err)
        return err;

    if (!ctx->is_direct) {
        err = aura_controller_write_byte_locked(ctx, AURA_REG_APPLY, AURA_APPLY_VAL);
        if (err)
            return err;
    }

    // Also apply to zone_all
    for (i = 0; i <= ctx->zone_count; i++)
        target->zone[i] = *color;

    mutex_unlock(&ctx->lock);

    return 0;
}

error_t aura_controller_set_mode (
    struct aura_controller *ctrl,
    const struct lights_mode *mode
){
    struct aura_controller_context *ctx = to_aura_context(ctrl);
    enum aura_mode aura_mode;
    bool changed;
    error_t err;

    if (WARN_ON(NULL == ctrl || NULL == mode)) {
        AURA_ERR("NULL ptr detected");
        return -EINVAL;
    }

    err = lights_mode_to_aura_mode(mode, &aura_mode);
    if (err)
        return err;

    AURA_DBG("setting mode: '%s' to all zones", mode->name);

    err = 0;
    changed = false;

    mutex_lock(&ctx->lock);

    if (aura_mode == AURA_MODE_DIRECT) {
        if (!ctx->is_direct) {
            changed = true;
            err = aura_controller_write_byte_locked(ctx, AURA_REG_DIRECT, 0x01);
            if (err)
                goto error;
        }
    } else {
        if (ctx->is_direct) {
            changed = true;
            err = aura_controller_write_byte_locked(ctx, AURA_REG_DIRECT, 0x00);
            if (err)
                goto error;
        }
        if (mode->id != ctx->mode->id) {
            changed = true;
            err = aura_controller_write_byte_locked(ctx, AURA_REG_MODE, aura_mode);
            if (err)
                goto error;
        }
    }

    if (changed) {
        // TODO: If this apply fails, what happens to the states of the above?
        err = aura_controller_write_byte_locked(ctx, AURA_REG_APPLY, AURA_APPLY_VAL);
        if (err)
            goto error;

        if (aura_mode == AURA_MODE_DIRECT) {
            ctx->is_direct = true;
        } else {
            ctx->mode = mode;
        }
    }

error:
    mutex_unlock(&ctx->lock);

    return err;
}

error_t aura_controller_get_mode (
    struct aura_controller *ctrl,
    struct lights_mode *mode
){
    struct aura_controller_context *ctx = to_aura_context(ctrl);

    if (WARN_ON(NULL == ctrl || NULL == mode)) {
        AURA_ERR("NULL ptr detected");
        return -EINVAL;
    }

    mutex_lock(&ctx->lock);

    *mode = *ctx->mode;

    mutex_unlock(&ctx->lock);

    return 0;
}
