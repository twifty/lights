// SPDX-License-Identifier: GPL-2.0
#include <linux/i2c.h>
#include <adapter/lights-interface.h>
#include <adapter/lights-adapter.h>

#include <aura/debug.h>
#include "asic/asic-types.h"
#include "aura-gpu-i2c.h"

#define MAX_SUPPORTED_GPUS 2

static LIST_HEAD(aura_gpu_adapter_list);
static LIST_HEAD(aura_gpu_ctrl_list);

typedef void (*i2c_destroy_t)(struct i2c_adapter*);

/**
 * struct aura_smbus_adapter - Storage for i2c/async
 *
 * @siblings:       Next and Prev pointers
 * @i2c_adapter:    Physical access point to the hardware
 * @lights_adapter: Async access point to the hardware
 * @i2c_destroy:    Destructor for @i2c_adapter
 *
 * The @i2c_destroy method is only valid if an adapter was created
 * by this module.
 */
struct aura_gpu_adapter {
    struct list_head        siblings;
    struct i2c_adapter      *i2c_adapter;
    i2c_destroy_t           i2c_destroy;
};

/* TODO - test both 100hz and 20hz i2c speed */
static const uint8_t chipset_addresses[] = { 0x29, 0x2A, 0x60 };

/*
    NOTE - If anybody has a datasheet for this ITE IT8915FN chip, please pass
    it on to me so that I can correctly configure these registers.
 */
enum AURA_GPU_CONSTS {
    // AURA_GPU_CHIPSET_ADDR           = 0x29,
    AURA_GPU_CHIPSET_MAGIC_HI       = 0x20,
    AURA_GPU_CHIPSET_MAGIC_LO       = 0x21,
    AURA_GPU_CHIPSET_MAGIC_VALUE    = 0x1589,

    AURA_GPU_RED_ADDR               = 0x04,
    AURA_GPU_GREEN_ADDR             = 0x05,
    AURA_GPU_BLUE_ADDR              = 0x06,
    AURA_GPU_MODE_ADDR              = 0x07,

    /*
        The chip loops an internal buffer (0xFF bytes long) for the non
        static modes. This r/w register is the offet into that buffer.

        TODO - For some reason a value of 0x02 is written when the leds
        are set to off.
     */
    AURA_GPU_SYNC_ADDR              = 0x0C,

    /*
        A value of 1 is written here after every change, but the card will
        use any set color/mode without writing this.
        Power cycling without applying will result in the colors and zone
        being reset to the original values.
     */
    AURA_GPU_APPLY_ADDR             = 0x0E,

    /*
        TODO - Figure out how these are used
     */
    AURA_GPU_SECONDARY_RED_ADDR     = 0x2F,
    AURA_GPU_SECONDARY_GREEN_ADDR   = 0x30,
    AURA_GPU_SECONDARY_BLUE_ADDR    = 0x31,
    /*
        TODO - This may not be a mode
     */
    AURA_GPU_SECONDARY_MODE_ADDR    = 0x32,

    AURA_GPU_DISABLE                = 0x01,
};

/*
    NOTE - Don't change the values, they are used as i2c values
 */
enum aura_mode {
    /*
        There is no off mode, all colors are set to 0. The LightingService
        stores the previous values in its xml file.
     */
    AURA_MODE_OFF           = 0x00,
    AURA_MODE_STATIC        = 0x01,
    AURA_MODE_BREATHING     = 0x02,

    /*
        NOTE - All the card does is toggle between on/off every second.

        The random effect is software controlled.
     */
    AURA_MODE_FLASHING      = 0x03,
    AURA_MODE_CYCLE         = 0x04,

    AURA_MODE_LAST          = AURA_MODE_CYCLE,

    AURA_INDEX_DIRECT       = 0x05,
    /*
        This is not an actual mode. The chip imitiates direct
        mode by using STATIC mode not applying (setting 0x01
        to 0x0E). However, the color of the previous mode is
        lost until a full power cycle (reboot doesn't work).
     */
    AURA_MODE_DIRECT        = 0xFF,
};

/*
    NOTE - Don't change the order, aura_mode val used as array index
 */
struct lights_effect aura_gpu_effects[] = {
    LIGHTS_EFFECT_VALUE(AURA_MODE_OFF,       OFF),
    LIGHTS_EFFECT_VALUE(AURA_MODE_STATIC,    STATIC),
    LIGHTS_EFFECT_VALUE(AURA_MODE_BREATHING, BREATHING),
    LIGHTS_EFFECT_VALUE(AURA_MODE_FLASHING,  FLASHING),
    LIGHTS_EFFECT_VALUE(AURA_MODE_CYCLE,     CYCLE),

    LIGHTS_EFFECT_CUSTOM(AURA_MODE_DIRECT,   "direct"),

    {}
};

struct zone_reg {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t mode;
    uint8_t apply;
};

#define state_dump(_msg, _state) ( \
    AURA_DBG( \
        "%s Effect: '%s', Color: 0x%06x", \
        (_msg), \
        (_state)->effect.name, \
        (_state)->color.value \
    ) \
)

/**
 * struct aura_gpu_zone - Storage for a single zone
 *
 * @ctrl:   Owning controller
 * @lock:   Read/Write lock
 * @thunk:  Magic member
 * @effect: Active Color and Mode
 * @reg:    Register offsets
 */
struct aura_gpu_zone {
    struct aura_gpu_controller  *ctrl;
    spinlock_t                  lock;
    struct lights_thunk         thunk;
    struct lights_state         state;
    struct zone_reg const       reg;
};
#define ZONE_HASH 'ZONE'
#define zone_from_thunk(ptr) ( \
    lights_thunk_container(ptr, struct aura_gpu_zone, thunk, ZONE_HASH) \
)

/**
 * struct aura_gpu_controller - Single GPU storage
 *
 * @siblings:      Next and prev pointers
 * @lights_client: Async hardware access
 * @lock:          Read/Write lock of this object
 * @id:            The numeric of the GPU (As seen in /dev/lights/gpu-ID)
 * @zone_count:    Number of lighting zones (Currently only one)
 * @aura_gpu_zones: Array of zone data
 * @lights:        Userland access
 * @lights_name:   Name as seen in /dev/lights/
 */
struct aura_gpu_controller {
    struct list_head                siblings;
    struct lights_adapter_client    lights_client;
    uint8_t                         id;

    /* Allow multiple zones for future */
    uint8_t                         zone_count;
    struct aura_gpu_zone            *zones;
    struct lights_dev               lights;
    char                            lights_name[6];
};

/**
 * lights_mode_to_aura_mode() - Mode convertion
 *
 * @effect:   Globally recognizable mode
 * @gpu_mode: GPU recognizable mode
 *
 * @return: Zero or a negative error number
 */
static error_t lights_effect_to_aura_mode (
    struct lights_effect const *effect,
    enum aura_mode *gpu_mode
){
    effect = lights_effect_find_by_id(aura_gpu_effects, effect->id);
    if (effect) {
        *gpu_mode = effect->value;
        return 0;
    }

    return -ENODATA;
    // if (lights_is_custom_mode(mode)) {
    //     *gpu_mode = lights_custom_mode_id(mode);
    //     if (*gpu_mode <= AURA_MODE_LAST || *gpu_mode == AURA_MODE_DIRECT)
    //         return 0;
    //
    //     return -ENODATA;
    // }
    //
    // switch (lights_mode_id(mode)) {
    //     case LIGHTS_MODE_OFF:
    //         *gpu_mode = AURA_MODE_OFF;
    //         return 0;
    //     case LIGHTS_MODE_STATIC:
    //         *gpu_mode = AURA_MODE_STATIC;
    //         return 0;
    //     case LIGHTS_MODE_BREATHING:
    //         *gpu_mode = AURA_MODE_BREATHING;
    //         return 0;
    //     case LIGHTS_MODE_FLASHING:
    //         *gpu_mode = AURA_MODE_FLASHING;
    //         return 0;
    //     case LIGHTS_MODE_CYCLE:
    //         *gpu_mode = AURA_MODE_CYCLE;
    //         return 0;
    // }
    //
    // return -ENODATA;
}

/**
 * aura_mode_to_lights_mode() - Mode convertion
 *
 * @gpu_mode: GPU recognizable mode
 * @effect:   Buffer to write result
 *
 * @return: Error code
 */
static error_t aura_mode_to_lights_effect (
    enum aura_mode gpu_mode,
    struct lights_effect const **effect
){
    *effect = lights_effect_find_by_value(aura_gpu_effects, gpu_mode);
    if (*effect)
        return 0;

    return -ENODATA;
    // if (gpu_mode == AURA_MODE_DIRECT) {
    //     return &aura_modes[AURA_INDEX_DIRECT];
    // } else if (gpu_mode >= 0 && gpu_mode <= AURA_MODE_LAST) {
    //     return &aura_modes[gpu_mode];
    // }
    //
    // return NULL;
}

/**
 * aura_gpu_i2c_read_byte() - Reads a single byte from the hardware
 *
 * @client: Adapter and address of device
 * @reg:    Offset to read from
 * @value:  Buffer to read into
 *
 * @return: Zero or a negative error code
 *
 * This function is blocking.
 */
static error_t aura_gpu_i2c_read_byte (
    struct lights_adapter_client *client,
    uint8_t reg,
    uint8_t *value
){
    error_t err;
    struct lights_adapter_msg msgs[] = {
        ADAPTER_READ_BYTE_DATA(reg)
    };

    err = lights_adapter_xfer(client, msgs, ARRAY_SIZE(msgs));
    if (!err)
        *value = msgs[0].data.byte;

    return err;
}

/**
 * aura_gpu_i2c_write_byte() - Writes a single byte to the hardware
 *
 * @client: Adapter and address of device
 * @reg:    Offset to read from
 * @value:  Buffer to read from
 *
 * @return: Zero or a negative error code
 *
 * This function is blocking.
 */
__used
static error_t aura_gpu_i2c_write_byte (
    struct lights_adapter_client *client,
    uint8_t reg,
    uint8_t value
){
    error_t err;
    struct lights_adapter_msg msgs[] = {
        ADAPTER_WRITE_BYTE_DATA(reg, value)
    };

    err = lights_adapter_xfer(client, msgs, ARRAY_SIZE(msgs));

    return err;
}

/**
 * aura_gpu_discover() - Checks if the adapter has an AURA capable chip
 *
 * @i2c_adapter: Hardware device
 * @client:      Buffer to write address of chip
 *
 * @return: Zero or a negative error number
 */
static error_t aura_gpu_discover (
    struct i2c_adapter *i2c_adapter,
    struct lights_adapter_client *client
){
    uint8_t offset[2] = { AURA_GPU_CHIPSET_MAGIC_HI, AURA_GPU_CHIPSET_MAGIC_LO };
    uint8_t value[2] = {0};
    error_t err;
    int i, j;

    AURA_DBG("Scanning '%s' for GPU controller", i2c_adapter->name);

    for (j = 0; j < ARRAY_SIZE(chipset_addresses); j++) {

        *client = LIGHTS_I2C_CLIENT(i2c_adapter, chipset_addresses[j], 0);

        for (i = 0; i < 2; i++) {
            err = aura_gpu_i2c_read_byte(client, offset[i], &value[i]);
            if (err) {
                // AURA_DBG("Failed to read offset 0x%02x on address 0x%02x: %d", offset[i], *addr, err);
                break;
            }
        }

        if (!err && ((value[0] << 8) | value[1]) == AURA_GPU_CHIPSET_MAGIC_VALUE) {
            AURA_DBG(
                "Discovered aura chip at address %x on '%s'",
                chipset_addresses[j],
                i2c_adapter->name
            );
            return 0;
        }
    }

    return -ENODEV;
}

/**
 * aura_gpu_fetch_zone() - Populates the colors and mode of the zone
 *
 * @zone: Zone to populate
 *
 * @return: Zero or a negative error number
 *
 * This function is blocking.
 */
static error_t aura_gpu_fetch_zone (
    struct aura_gpu_zone *zone
){
    enum aura_mode gpu_mode;
    struct lights_effect const *effect;
    struct lights_color color;
    struct lights_adapter_client *client = &zone->ctrl->lights_client;
    uint8_t mode_raw;
    error_t err;

    spin_lock(&zone->lock);

    err = aura_gpu_i2c_read_byte(client, zone->reg.red, &color.r);
    if (err)
        goto error;

    err = aura_gpu_i2c_read_byte(client, zone->reg.green, &color.g);
    if (err)
        goto error;

    err = aura_gpu_i2c_read_byte(client, zone->reg.blue, &color.b);
    if (err)
        goto error;

    err = aura_gpu_i2c_read_byte(client, zone->reg.mode, &mode_raw);
    if (err)
        goto error;

    /* Determine the mode based on the values */
    if (mode_raw >= AURA_MODE_BREATHING && mode_raw <= AURA_MODE_LAST){
        gpu_mode = mode_raw;
    } else if (mode_raw <= AURA_MODE_STATIC) {
        if (0 == color.value) {
            gpu_mode = AURA_MODE_OFF;
        } else {
            gpu_mode = AURA_MODE_STATIC;
        }
    } else {
        /* This is an unexpected value but we can recover */
        gpu_mode = AURA_MODE_STATIC;
    }

    /* Convert to lights_mode */
    if (aura_mode_to_lights_effect(gpu_mode, &effect)) {
        err = -EINVAL;
        goto error;
    }

    zone->state.effect = *effect;
    zone->state.color = color;

error:
    spin_unlock(&zone->lock);

    return err;
}


/**
 * aura_gpu_zone_update_callback() - Async callback handler
 *
 * @result: Sent messages
 * @thunk:  Zone which was updated
 * @error:  Any error while writing
 *
 * The hardware was updated before this call, here we sync the local object.
 */
static void aura_gpu_zone_update_callback (
    struct lights_adapter_msg const * const result,
    struct lights_thunk *thunk,
    error_t error
){
    struct aura_gpu_zone *zone = zone_from_thunk(thunk);
    struct lights_adapter_msg const *iter = result;
    struct lights_effect const *effect;
    enum aura_mode gpu_mode;
    uint8_t color_bytes[3];
    bool disable = false;
    int i;

    if (IS_NULL(result, thunk, zone))
        return;

    if (error) {
        AURA_DBG("Failed to update: %s", ERR_NAME(error));
        return;
    }

    if (!lights_adapter_msg_value(iter, MSG_BYTE_DATA, &gpu_mode)) {
        AURA_ERR("Failed to read mode from messages");
        return;
    }

    if (AURA_GPU_DISABLE == lights_adapter_msg_read_flags(iter))
        disable = true;

    for (i = 0; i < 3; i++) {
        iter = iter->next;

        if (!lights_adapter_msg_value(iter, MSG_BYTE_DATA, &color_bytes[i])) {
            AURA_ERR("Failed to read mode from messages");
            return;
        }
    }

    /* If the APPLY was not set, the mode is really DIRECT */
    if (!iter->next && gpu_mode == AURA_MODE_STATIC)
        gpu_mode = disable ? AURA_MODE_OFF : AURA_MODE_DIRECT;

    if (aura_mode_to_lights_effect(gpu_mode, &effect)) {
        AURA_DBG("Not a valid aura mode 0x%02x", gpu_mode);
        return;
    }

    state_dump("pre update:", &zone->state);

    spin_lock(&zone->lock);

    if (!disable)
        lights_color_read_rgb(&zone->state.color, color_bytes);

    zone->state.effect = *effect;

    spin_unlock(&zone->lock);

    state_dump("post update:", &zone->state);
}

/**
 * aura_gpu_zone_update() - Begins the async updater
 *
 * @zone:   Zone being updated
 * @effect: New values
 *
 * @return: Error code
 */
static error_t aura_gpu_zone_update (
    struct aura_gpu_zone *zone,
    struct lights_state const *state
){
    uint8_t count = 4;
    enum aura_mode gpu_mode;
    struct lights_adapter_msg msgs[5];
    bool off = false;
    error_t err;

    if (IS_NULL(zone, state) || IS_TRUE(state->flags == 0))
        return -EINVAL;

    err = lights_effect_to_aura_mode(&state->effect, &gpu_mode);
    if (err)
        return err;

    switch (gpu_mode) {
        case AURA_MODE_DIRECT:
            msgs[0] = ADAPTER_WRITE_BYTE_DATA(zone->reg.mode,  AURA_MODE_STATIC);
            break;
        case AURA_MODE_OFF:
            msgs[0] = ADAPTER_WRITE_BYTE_DATA(zone->reg.mode,  AURA_MODE_STATIC);
            lights_adapter_msg_write_flags(&msgs[0], AURA_GPU_DISABLE);
            off = true;
            count = 5;
            break;
        default:
            msgs[0] = ADAPTER_WRITE_BYTE_DATA(zone->reg.mode,  gpu_mode);
            count = 5;
            break;
    }

    msgs[1] = ADAPTER_WRITE_BYTE_DATA(zone->reg.red,   off ? 0 : state->color.r);
    msgs[2] = ADAPTER_WRITE_BYTE_DATA(zone->reg.green, off ? 0 : state->color.g);
    msgs[3] = ADAPTER_WRITE_BYTE_DATA(zone->reg.blue,  off ? 0 : state->color.b);
    msgs[4] = ADAPTER_WRITE_BYTE_DATA(zone->reg.apply, 0x01);

    return lights_adapter_xfer_async(
        &zone->ctrl->lights_client,
        msgs,
        count,
        &zone->thunk,
        aura_gpu_zone_update_callback
    );
}

/**
 * aura_gpu_controller_update() - Updates all values of the controller
 *
 * @ctrl:   Controller being updated
 * @effect: New values
 *
 * @return: Error code
 */
static error_t aura_gpu_controller_update (
    struct aura_gpu_controller *ctrl,
    struct lights_state const *state
){
    int i;
    error_t err = 0;

    if (IS_NULL(ctrl, state))
        return -EINVAL;

    for (i = 0; i < ctrl->zone_count && !err; i++)
        err = aura_gpu_zone_update(&ctrl->zones[i], state);

    return err;
}


/**
 * aura_gpu_read() - Reads a zones state
 *
 * @thunk: The zone to read
 * @state: Buffer to write into
 *
 * @return: Zero or a negative error number
 *
 * The returned mode is from the local cache not the hardware.
 */
static error_t aura_gpu_read (
    struct lights_thunk *thunk,
    struct lights_state *state
){
    struct aura_gpu_zone *zone = zone_from_thunk(thunk);

    if (IS_NULL(thunk, state, zone))
        return -EINVAL;

    spin_lock(&zone->lock);

    if (state->type & LIGHTS_TYPE_EFFECT)
        state->effect = zone->state.effect;

    if (state->type & LIGHTS_TYPE_COLOR)
        state->color = zone->state.color;

    spin_unlock(&zone->lock);

    return 0;
}

/**
 * aura_gpu_write() - Writes a zones values
 *
 * @thunk: The zone to write
 * @state: The buffer containing the new values
 *
 * @return: Zero or a negative error number
 */
static error_t aura_gpu_write (
    struct lights_thunk *thunk,
    struct lights_state const *state
){
    struct aura_gpu_zone *zone = zone_from_thunk(thunk);
    struct lights_state pending;
    error_t err = 0;

    if (IS_NULL(thunk, state, zone))
        return -EINVAL;

    spin_lock(&zone->lock);

    pending = zone->state;
    pending.flags = 0;

    if (state->type & LIGHTS_TYPE_COLOR) {
        pending.color = state->color;
        pending.flags |= LIGHTS_TYPE_COLOR;
    }

    if (state->type & LIGHTS_TYPE_EFFECT) {
        if (!lights_effect_copy(aura_gpu_effects, &state->effect, pending.effect)) {
            lights_effect_debug(AURA_ERR, "Unknown mode: ", &state->effect);
            return -EINVAL;
        }
        // pending.effect = state->effect;
        pending.flags |= LIGHTS_TYPE_EFFECT;
    }

    if (pending.flags)
        err = aura_gpu_zone_update(zone, &pending);

    spin_unlock(&zone->lock);

    return err;
}


/**
 * aura_gpu_create_fs() - Creates Userland access to the GPU
 *
 * @gpu_ctrl: Controller to make public
 *
 * @return: Zero or a negative error number
 */
static error_t aura_gpu_create_fs (
    struct aura_gpu_controller *ctrl
){
    struct lights_attribute attrs[] = {
        LIGHTS_EFFECT_ATTR(
            &ctrl->zones[0].thunk,
            aura_gpu_read,
            aura_gpu_write
        ),
        LIGHTS_COLOR_ATTR(
            &ctrl->zones[0].thunk,
            aura_gpu_read,
            aura_gpu_write
        ),
        LIGHTS_UPDATE_ATTR(
            &ctrl->zones[0].thunk,
            aura_gpu_write
        )
    };
    uint8_t id = ctrl->id;
    error_t err;

    ctrl->lights.name = ctrl->lights_name;
    ctrl->lights.caps = aura_gpu_effects;

    for (id = ctrl->id; id < 2; id++) {
        snprintf(ctrl->lights_name, sizeof(ctrl->lights_name), "gpu-%d", id);
        err = lights_device_register(&ctrl->lights);
        if (err) {
            if (err == -EEXIST)
                continue;
            return err;
        }
        break;
    }

    /* Create the attributes */
    err = lights_device_create_files(&ctrl->lights, attrs, ARRAY_SIZE(attrs));
    if (err)
        goto error_release;

    return 0;

error_release:
    lights_device_unregister(&ctrl->lights);

    return err;
}

/**
 * aura_gpu_count()
 *
 * @return: Number of controllers
 */
static uint8_t aura_gpu_count (
    void
){
    struct list_head *iter;
    uint8_t count = 0;

    list_for_each(iter, &aura_gpu_ctrl_list)
        count++;

    return count;
}

/**
 * aura_gpu_controller_create() - Creates a controller for the adapter
 *
 * @i2c_adapter: The adapter to search
 *
 * @return: NULL, a controller or a negative error number
 */
static struct aura_gpu_controller *aura_gpu_controller_create (
    struct i2c_adapter *i2c_adapter
){
    struct aura_gpu_controller *ctrl;
    struct aura_gpu_zone *zones;
    struct lights_adapter_client client;
    uint8_t zone_count = 1;
    error_t err;

    /* Check for the presence of the chip and check its id */
    err = aura_gpu_discover(i2c_adapter, &client);
    if (err)
        return err == -ENODEV ? NULL : ERR_PTR(err);

    ctrl = kzalloc(sizeof(*ctrl), GFP_KERNEL);
    if (!ctrl)
        return ERR_PTR(-ENOMEM);

    ctrl->lights_client = client;

    err = lights_adapter_register(&ctrl->lights_client, 32);
    if (err) {
        AURA_DBG("Failed to register lights_adapter: %s", ERR_NAME(err));
        goto error_free_ctrl;
    }

    zones = kzalloc(sizeof(*zones) * zone_count, GFP_KERNEL);
    if (!zones) {
        err = -ENOMEM;
        goto error_free_ctrl;
    }

    lights_thunk_init(&zones[0].thunk, ZONE_HASH);
    zones[0].ctrl = ctrl;

    /* Work around the const */
    memcpy((void*)&zones[0].reg, &(struct zone_reg){
        .red   = AURA_GPU_RED_ADDR,
        .green = AURA_GPU_GREEN_ADDR,
        .blue  = AURA_GPU_BLUE_ADDR,
        .mode  = AURA_GPU_MODE_ADDR,
        .apply = AURA_GPU_APPLY_ADDR,
    }, sizeof(struct zone_reg));

    err = aura_gpu_fetch_zone(&zones[0]);
    if (err)
        goto error_free_zone;

    ctrl->zone_count = zone_count;
    ctrl->zones = zones;
    ctrl->id = aura_gpu_count();

    err = aura_gpu_create_fs(ctrl);
    if (err)
        goto error_free_zone;

    list_add_tail(&ctrl->siblings, &aura_gpu_ctrl_list);

    AURA_INFO(
        "Detected AURA capable GPU on '%s' at 0x%02x with Color: 0x%06x, Mode: %s",
        i2c_adapter->name,
        ctrl->lights_client.i2c_client.addr,
        ctrl->zones[0].state.color.value,
        ctrl->zones[0].state.effect.name
    );

    return ctrl;

error_free_zone:
    kfree(zones);
error_free_ctrl:
    kfree(ctrl);

    return ERR_PTR(err);
}

/**
 * aura_gpu_controller_destroy() - Releases all memory for a controller
 *
 * @gpu_ctrl: The controller to delete
 */
static void aura_gpu_controller_destroy (
    struct aura_gpu_controller *ctrl
){
    list_del(&ctrl->siblings);

    if (ctrl->zones) {
        kfree(ctrl->zones);
    }

    lights_adapter_unregister(&ctrl->lights_client);
    lights_device_unregister(&ctrl->lights);

    kfree(ctrl);
}


/**
 * aura_gpu_adapter_create() - Registers async access to the i2c device
 *
 * @i2c_adapter: Raw access point
 * @i2c_destroy: Optional destructor for the @i2c_adapter
 *
 * @return: The async interface or a negative error code
 */
static struct aura_gpu_adapter *aura_gpu_adapter_create (
    struct i2c_adapter *i2c_adapter,
    i2c_destroy_t i2c_destroy
){
    struct aura_gpu_adapter *gpu_adapter;

    if (IS_NULL(i2c_adapter, i2c_destroy))
        return ERR_PTR(-EINVAL);

    gpu_adapter = kzalloc(sizeof(*gpu_adapter), GFP_KERNEL);
    if (!gpu_adapter)
        return ERR_PTR(-ENOMEM);

    gpu_adapter->i2c_adapter = i2c_adapter;
    gpu_adapter->i2c_destroy = i2c_destroy;

    list_add_tail(&gpu_adapter->siblings, &aura_gpu_adapter_list);

    return gpu_adapter;
}

/**
 * aura_gpu_adapter_destroy() - Releases a previously bound adapter
 *
 * @gpu_adapter: The adapter to delete
 */
static void aura_gpu_adapter_destroy (
    struct aura_gpu_adapter *gpu_adapter
){
    if (IS_NULL(gpu_adapter))
        return;

    list_del(&gpu_adapter->siblings);

    gpu_adapter->i2c_destroy(gpu_adapter->i2c_adapter);

    kfree(gpu_adapter);
}

/**
 * aura_gpu_probe_device() - Callback for @i2c_for_each_dev
 *
 * @dev:  PCI device to search
 * @data: Count and errors
 *
 * @return: Zero or a negative error number
 */
static error_t aura_gpu_probe_device (
    struct device *dev,
    void *found
){
    struct i2c_adapter *adapter = to_i2c_adapter(dev);
    struct aura_gpu_controller *gpu_controller;
    error_t err = 0;

    if (dev->type != &i2c_adapter_type || *(int*)found >= MAX_SUPPORTED_GPUS)
        return 0;

    gpu_controller = aura_gpu_controller_create(adapter);
    if (IS_ERR(gpu_controller)) {
        err = CLEAR_ERR(gpu_controller);
        goto error;
    }

    if (!gpu_controller) {
        err = 0;
        goto error;
    }

    *(int*)found += 1;

error:
    return err;
}

/**
 * aura_gpu_release() - Exit
 */
void aura_gpu_release (
    void
){
    struct aura_gpu_controller *gpu_ctrl, *safe_ctrl;
    struct aura_gpu_adapter *gpu_adapter, *safe_adapter;

    list_for_each_entry_safe(gpu_ctrl, safe_ctrl, &aura_gpu_ctrl_list, siblings) {
        aura_gpu_controller_destroy(gpu_ctrl);
    }

    list_for_each_entry_safe(gpu_adapter, safe_adapter, &aura_gpu_adapter_list, siblings) {
        aura_gpu_adapter_destroy(gpu_adapter);
    }
}

/**
 * aura_gpu_probe() - Entry point
 *
 * @state: Initial state to apply to and found devices
 *
 * @return: Zero or a negative error number
 */
error_t aura_gpu_probe (
    struct lights_state const *state
){
    struct i2c_adapter *adapters[MAX_SUPPORTED_GPUS];
    struct aura_gpu_adapter *gpu_adapter = NULL;
    struct aura_gpu_controller *ctrl;
    // struct zone_state zone_state;
    int adapter_count = 0;
    int i = 0, found = 0;
    error_t err;

    AURA_DBG("Trying built-in drivers");
    err = i2c_for_each_dev(&found, aura_gpu_probe_device);

    if (!err && found < MAX_SUPPORTED_GPUS) {
        AURA_DBG("Trying custom drivers");

        adapter_count = gpu_adapters_create(adapters, ARRAY_SIZE(adapters));
        if (adapter_count < 0) {
            err = adapter_count;
            goto error;
        }

        for (i = 0; i < adapter_count; i++) {
            ctrl = aura_gpu_controller_create(adapters[i]);
            if (IS_ERR_OR_NULL(ctrl)) {
                gpu_adapter_destroy(adapters[i]);
                adapters[i] = NULL;

                if (ctrl) {
                    err = CLEAR_ERR(ctrl);
                    goto error;
                }
            } else {
                gpu_adapter = aura_gpu_adapter_create(adapters[i], gpu_adapter_destroy);
                if (IS_ERR(gpu_adapter)) {
                    err = CLEAR_ERR(gpu_adapter);
                    goto error;
                } else {
                    found++;
                    adapters[i] = NULL;
                }
            }
        }
    }

    if (!err && found) {
        list_for_each_entry(ctrl, &aura_gpu_ctrl_list, siblings) {
            struct lights_state copy = lights_state_copy(state, aura_gpu_effects, LIGHTS_TYPE_UPDATE);
            copy.flags = copy.type;
            
            err = aura_gpu_controller_update(ctrl, &copy);
            if (err)
                goto error;
        }
    }

    return err;

error:
    for (i = 0; i < adapter_count; i++) {
        if (adapters[i])
            gpu_adapter_destroy(adapters[i]);
    }

    aura_gpu_release();

    return err;
}
