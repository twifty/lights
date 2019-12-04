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
enum {
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
};

/*
    NOTE - Don't change the values, they are used as i2c values
 */
enum aura_gpu_mode {
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
    NOTE - Don't change the order, aura_gpu_mode val used as array index
 */
struct lights_mode aura_gpu_modes[] = {
    LIGHTS_MODE(OFF),
    LIGHTS_MODE(STATIC),
    LIGHTS_MODE(BREATHING),
    LIGHTS_MODE(FLASHING),
    LIGHTS_MODE(CYCLE),

    LIGHTS_CUSTOM_MODE(AURA_MODE_DIRECT, "direct"),

    LIGHTS_MODE_LAST_ENTRY()
};

struct zone_reg {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t mode;
    uint8_t apply;
};

/**
 * struct zone_context - State of the zone
 *
 * @gpu_ctrl: Owning controller
 * @mode:     Current mode
 * @color:    Current color
 * @reg:      Register offsets for colors and mode
 */
struct zone_context {
    struct aura_gpu_controller  *gpu_ctrl;
    struct lights_mode          mode;
    struct lights_color         color;
    const struct zone_reg       reg;
};

/**
 * struct aura_gpu_controller - Single GPU storage
 *
 * @siblings:      Next and prev pointers
 * @lights_client: Async hardware access
 * @lock:          Read/Write lock of this object
 * @id:            The numeric of the GPU (As seen in /dev/lights/gpu-ID)
 * @zone_count:    Number of lighting zones (Currently only one)
 * @zone_contexts: Array of zone data
 * @lights:        Userland access
 * @lights_name:   Name as seen in /dev/lights/
 */
struct aura_gpu_controller {
    struct list_head                siblings;
    struct lights_adapter_client    lights_client;

    struct mutex                    lock;
    uint8_t                         id;

    /* Allow multiple zones for future */
    uint8_t                         zone_count;
    struct zone_context             *zone_contexts;
    struct lights_dev               lights;
    char                            lights_name[6];
};

/**
 * struct callback_data - Used to iterate pci devices
 *
 * @count: Number of found GPUs
 * @error: Any error that occured while creating controller
 */
struct callback_data {
    uint8_t count;
    error_t error;
};

/**
 * lights_mode_to_aura_gpu_mode() - Mode convertion
 *
 * @mode:     Globally recognizable mode
 * @gpu_mode: GPU recognizable mode
 *
 * @return: Zero or a negative error number
 */
static error_t lights_mode_to_aura_gpu_mode (
    const struct lights_mode *mode,
    enum aura_gpu_mode *gpu_mode
){
    if (lights_is_custom_mode(mode)) {
        *gpu_mode = lights_custom_mode_id(mode);
        if (*gpu_mode <= AURA_MODE_LAST || *gpu_mode == AURA_MODE_DIRECT)
            return 0;

        return -ENODATA;
    }

    switch (lights_mode_id(mode)) {
        case LIGHTS_MODE_OFF:
            *gpu_mode = AURA_MODE_OFF;
            return 0;
        case LIGHTS_MODE_STATIC:
            *gpu_mode = AURA_MODE_STATIC;
            return 0;
        case LIGHTS_MODE_BREATHING:
            *gpu_mode = AURA_MODE_BREATHING;
            return 0;
        case LIGHTS_MODE_FLASHING:
            *gpu_mode = AURA_MODE_FLASHING;
            return 0;
        case LIGHTS_MODE_CYCLE:
            *gpu_mode = AURA_MODE_CYCLE;
            return 0;
    }

    return -ENODATA;
}

/**
 * aura_gpu_mode_to_lights_mode() - Mode convertion
 *
 * @gpu_mode: GPU recognizable mode
 *
 * @return: NULL or a globally recognizable mode
 */
static const struct lights_mode *aura_gpu_mode_to_lights_mode (
    enum aura_gpu_mode gpu_mode
){
    if (gpu_mode == AURA_MODE_DIRECT) {
        return &aura_gpu_modes[AURA_INDEX_DIRECT];
    } else if (gpu_mode >= 0 && gpu_mode <= AURA_MODE_LAST) {
        return &aura_gpu_modes[gpu_mode];
    }

    return NULL;
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
    struct lights_adapter_client * client,
    uint8_t reg,
    uint8_t * value
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
    struct i2c_adapter * i2c_adapter,
    struct lights_adapter_client * client
){
    uint8_t offset[2] = { AURA_GPU_CHIPSET_MAGIC_HI, AURA_GPU_CHIPSET_MAGIC_LO };
    uint8_t value[2] = {0};
    error_t err;
    int i, j;

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
                "Discovered aura chip at address %x on %s",
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
    struct zone_context * zone
){
    enum aura_gpu_mode gpu_mode;
    const struct lights_mode * mode;
    struct lights_color color;
    struct lights_adapter_client * client = &zone->gpu_ctrl->lights_client;
    uint8_t mode_raw;
    error_t err;

    mutex_lock(&zone->gpu_ctrl->lock);

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
    mode = aura_gpu_mode_to_lights_mode(gpu_mode);
    if (!mode) {
        err = -EINVAL;
        goto error;
    }

    memcpy((void*)&zone->mode, mode, sizeof(*mode));
    memcpy((void*)&zone->color, &color, sizeof(color));

error:
    mutex_unlock(&zone->gpu_ctrl->lock);

    return err;
}

/**
 * aura_gpu_set_color_callback() - Callback handler
 *
 * @result: Sent messages
 * @_data:  Zone which was updated
 * @error:  Any error while writing
 *
 * The hardware was updated before this call, here we sync the local object.
 */
static void aura_gpu_set_color_callback (
    struct lights_adapter_msg const * const result,
    void *_data,
    error_t error
){
    struct zone_context *zone = _data;
    struct lights_adapter_msg const * color_msg;
    uint8_t color_byte[3];
    int i;

    if (IS_NULL(result, _data))
        return;

    if (error) {
        AURA_DBG("Failed to set color %d", error);
        return;
    }

    color_msg = result;
    for (i = 0; i < 3; i++) {
        if (!color_msg) {
            AURA_DBG("Next message not found");
            return;
        }
        if (color_msg->type != MSG_BYTE_DATA) {
            AURA_ERR("Message has an invalid type '%d'", color_msg->type);
            return;
        }
        color_byte[i] = color_msg->data.byte;
        color_msg = color_msg->next;
    }

    mutex_lock(&zone->gpu_ctrl->lock);

    zone->color.r = color_byte[0];
    zone->color.g = color_byte[1];
    zone->color.b = color_byte[2];

    mutex_unlock(&zone->gpu_ctrl->lock);
}

/**
 * aura_gpu_apply_zone_color() - Begins the async writer
 *
 * @zone:  The zone to update
 * @color: The new color to apply
 *
 * @return: Zero or a negative error number
 */
static error_t aura_gpu_apply_zone_color (
    struct zone_context *zone,
    const struct lights_color *color
){
    uint8_t msg_count = 3;
    struct lights_adapter_msg msgs[] = {
        /* The controller is fussy about block read/writes */
        ADAPTER_WRITE_BYTE_DATA(zone->reg.red,   color->r),
        ADAPTER_WRITE_BYTE_DATA(zone->reg.green, color->g),
        ADAPTER_WRITE_BYTE_DATA(zone->reg.blue,  color->b),
        ADAPTER_WRITE_BYTE_DATA(zone->reg.apply, 0x01)
    };

    if (lights_custom_mode_id(&zone->mode) != AURA_MODE_DIRECT) {
        AURA_DBG("Non-Direct mode detected, applying save");
        msg_count = 4;
    } else {
        AURA_DBG("Current mode: %s %x", zone->mode.name, lights_custom_mode_id(&zone->mode));
    }

    return lights_adapter_xfer_async(
        &zone->gpu_ctrl->lights_client,
        msgs,
        msg_count,
        zone,
        aura_gpu_set_color_callback
    );
}

/**
 * aura_gpu_set_mode_callback() - Callback handler
 *
 * @result: Sent messages
 * @_data:  Zone which was updated
 * @error:  Any error while writing
 *
 * The hardware was updated before this call, here we sync the local object.
 */
static void aura_gpu_set_mode_callback (
    struct lights_adapter_msg const * const result,
    void *_data,
    error_t error
){
    struct zone_context *zone = _data;
    const struct lights_mode *mode;

    if (IS_NULL(result, _data))
        return;

    if (error) {
        AURA_DBG("Failed to set mode %d", error);
        return;
    }

    if (result->type != MSG_BYTE_DATA) {
        AURA_ERR("Message has an invalid type '%d'", result->type);
        return;
    }

    /* If the APPLY was not set, the mode is really DIRECT */
    if (result->data.byte == AURA_MODE_STATIC && result->next == NULL)
        mode = aura_gpu_mode_to_lights_mode(AURA_MODE_DIRECT);
    else
        mode = aura_gpu_mode_to_lights_mode(result->data.byte);

    if (!mode) {
        AURA_DBG("Not a valid aura mode 0x%02x", result->data.byte);
        return;
    }

    mutex_lock(&zone->gpu_ctrl->lock);

    memcpy((void*)&zone->mode, mode, sizeof(*mode));

    mutex_unlock(&zone->gpu_ctrl->lock);
}

/**
 * aura_gpu_apply_zone_mode() - Begins the async writer
 *
 * @zone: The zone to update
 * @mode: The new mode to apply
 *
 * @return: Zero or a negative error number
 */
static error_t aura_gpu_apply_zone_mode (
    struct zone_context *zone,
    const struct lights_mode *mode
){
    uint8_t msg_count = 1;
    enum aura_gpu_mode gpu_mode;
    struct lights_adapter_msg msgs[] = {
        ADAPTER_WRITE_BYTE_DATA(zone->reg.mode,  0),
        ADAPTER_WRITE_BYTE_DATA(zone->reg.apply, 0x01)
    };
    error_t err;

    err = lights_mode_to_aura_gpu_mode(mode, &gpu_mode);
    if (err)
        return err;

    /* DIRECT mode requires special handling */
    if (gpu_mode == AURA_MODE_DIRECT)
        gpu_mode = AURA_MODE_STATIC;
    else
        msg_count = 2;

    msgs[0].data.byte = gpu_mode;

    return lights_adapter_xfer_async(
        &zone->gpu_ctrl->lights_client,
        msgs,
        msg_count,
        zone,
        aura_gpu_set_mode_callback
    );
}

/**
 * aura_gpu_get_mode() - Reads a zones mode
 *
 * @data:  The zone to read
 * @state: Buffer to write into
 *
 * @return: Zero or a negative error number
 *
 * The returned mode is from the local cache not the hardware.
 */
static error_t aura_gpu_get_mode (
    void *data,
    struct lights_state *state
){
    struct zone_context *zone = data;

    if (IS_NULL(zone, state) || IS_FALSE(state->type & LIGHTS_TYPE_MODE))
        return -EINVAL;

    state->mode = zone->mode;

    return 0;
}

/**
 * aura_gpu_set_mode() - Writes a zones mode
 *
 * @data:  The zone to read
 * @state: Buffer containing new mode
 *
 * @return: Zero or a negative error number
 *
 * The actual writing is done asynchronously.
 */
static error_t aura_gpu_set_mode (
    void *data,
    const struct lights_state *state
){
    struct zone_context *zone = data;

    if (IS_NULL(zone, state) || IS_FALSE(state->type & LIGHTS_TYPE_MODE))
        return -EINVAL;

    return aura_gpu_apply_zone_mode(zone, &state->mode);
}

/**
 * aura_gpu_get_color() - Reads a zones color
 *
 * @data:  The zone to read
 * @state: The buffer to write into
 *
 * @return: Zero or a negative error number
 */
static error_t aura_gpu_get_color (
    void *data,
    struct lights_state *state
){
    struct zone_context *zone = data;

    if (IS_NULL(zone, state) || IS_FALSE(state->type & LIGHTS_TYPE_COLOR))
        return -EINVAL;

    state->color = zone->color;

    return 0;
}

/**
 * aura_gpu_set_color() - Writes a zones color
 *
 * @data:  The zone to write
 * @state: The buffer containing the new color
 *
 * @return: Zero or a negative error number
 */
static error_t aura_gpu_set_color (
    void *data,
    const struct lights_state *state
){
    struct zone_context *zone = data;

    if (IS_NULL(zone, state) || IS_FALSE(state->type & LIGHTS_TYPE_COLOR))
        return -EINVAL;

    return aura_gpu_apply_zone_color(zone, &state->color);
}

/**
 * aura_gpu_create_fs() - Creates Userland access to the GPU
 *
 * @gpu_ctrl: Controller to make public
 *
 * @return: Zero or a negative error number
 */
static error_t aura_gpu_create_fs (
    struct aura_gpu_controller *gpu_ctrl
){
    struct lights_io_attribute attrs[] = {
        LIGHTS_MODE_ATTR(
            &gpu_ctrl->zone_contexts[0],
            aura_gpu_get_mode,
            aura_gpu_set_mode
        ),
        LIGHTS_COLOR_ATTR(
            &gpu_ctrl->zone_contexts[0],
            aura_gpu_get_color,
            aura_gpu_set_color
        )
    };
    uint8_t id = gpu_ctrl->id;
    error_t err;

    gpu_ctrl->lights.name = gpu_ctrl->lights_name;
    gpu_ctrl->lights.caps = aura_gpu_modes;

    for (id = gpu_ctrl->id; id < 2; id++) {
        snprintf(gpu_ctrl->lights_name, sizeof(gpu_ctrl->lights_name), "gpu-%d", id);
        err = lights_device_register(&gpu_ctrl->lights);
        if (err) {
            if (err == -EEXIST)
                continue;
            return err;
        }
        break;
    }

    /* Create the attributes */
    err = lights_create_files(&gpu_ctrl->lights, attrs, ARRAY_SIZE(attrs));
    if (err)
        goto error_release;

    return 0;

error_release:
    lights_device_unregister(&gpu_ctrl->lights);

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
    struct aura_gpu_controller *gpu_ctrl;
    struct zone_context *zone_ctx;
    struct lights_adapter_client client;
    uint8_t zone_count = 1;
    error_t err;

    /* Check for the presence of the chip and check its id */
    err = aura_gpu_discover(i2c_adapter, &client);
    if (err)
        return err == -ENODEV ? NULL : ERR_PTR(err);

    gpu_ctrl = kzalloc(sizeof(*gpu_ctrl), GFP_KERNEL);
    if (!gpu_ctrl)
        return ERR_PTR(-ENOMEM);

    gpu_ctrl->lights_client = client;

    err = lights_adapter_register(&gpu_ctrl->lights_client, 32);
    if (err) {
        AURA_DBG("Failed to register lights_adapter: %s", ERR_NAME(err));
        goto error_free_ctrl;
    }

    zone_ctx = kzalloc(sizeof(*zone_ctx) * zone_count, GFP_KERNEL);
    if (!zone_ctx) {
        err = -ENOMEM;
        goto error_free_ctrl;
    }

    zone_ctx[0].gpu_ctrl = gpu_ctrl;
    /* Work around the const */
    memcpy((void*)&zone_ctx[0].reg, &(struct zone_reg){
        .red   = AURA_GPU_RED_ADDR,
        .green = AURA_GPU_GREEN_ADDR,
        .blue  = AURA_GPU_BLUE_ADDR,
        .mode  = AURA_GPU_MODE_ADDR,
        .apply = AURA_GPU_APPLY_ADDR,
    }, sizeof(struct zone_reg));

    err = aura_gpu_fetch_zone(&zone_ctx[0]);
    if (err)
        goto error_free_zone;

    gpu_ctrl->zone_count = zone_count;
    gpu_ctrl->zone_contexts = zone_ctx;
    gpu_ctrl->id = aura_gpu_count();

    err = aura_gpu_create_fs(gpu_ctrl);
    if (err)
        goto error_free_zone;

    list_add_tail(&gpu_ctrl->siblings, &aura_gpu_ctrl_list);

    AURA_INFO(
        "Detected AURA capable GPU on '%s' at 0x%02x with Color: 0x%06x, Mode: %s",
        i2c_adapter->name,
        gpu_ctrl->lights_client.i2c_client.addr,
        gpu_ctrl->zone_contexts[0].color.value,
        gpu_ctrl->zone_contexts[0].mode.name
    );

    return gpu_ctrl;

error_free_zone:
    kfree(zone_ctx);
error_free_ctrl:
    kfree(gpu_ctrl);

    return ERR_PTR(err);
}

/**
 * aura_gpu_controller_destroy() - Releases all memory for a controller
 *
 * @gpu_ctrl: The controller to delete
 */
static void aura_gpu_controller_destroy (
    struct aura_gpu_controller *gpu_ctrl
){
    list_del(&gpu_ctrl->siblings);

    if (gpu_ctrl->zone_contexts) {
        kfree(gpu_ctrl->zone_contexts);
    }

    lights_adapter_unregister(&gpu_ctrl->lights_client);
    lights_device_unregister(&gpu_ctrl->lights);

    kfree(gpu_ctrl);
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
    void *data
){
    struct callback_data *cb_data = data;
    struct i2c_adapter *adapter = to_i2c_adapter(dev);
    // struct aura_gpu_adapter *gpu_adapter;
    struct aura_gpu_controller *gpu_controller;
    error_t err;

    if (dev->type != &i2c_adapter_type || cb_data->count >= MAX_SUPPORTED_GPUS)
        return cb_data->count;

    // gpu_adapter = aura_gpu_adapter_create(adapter, NULL);
    // if (IS_ERR(gpu_adapter)) {
    //     err = CLEAR_ERR(gpu_adapter);
    //     goto error;
    // }

    gpu_controller = aura_gpu_controller_create(adapter);
    if (IS_ERR(gpu_controller)) {
        err = CLEAR_ERR(gpu_controller);
        goto error;
    }

    if (!gpu_controller) {
        err = 0;
        goto error;
    }

    AURA_DBG("Found %d controllers", cb_data->count);
    cb_data->count++;

    return cb_data->count;

error:
    // if (gpu_adapter)
    //     aura_gpu_adapter_destroy(gpu_adapter);

    cb_data->error = err;

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
    const struct lights_state *state
){
    struct callback_data data = {0};
    struct i2c_adapter *adapters[MAX_SUPPORTED_GPUS];
    struct aura_gpu_adapter *gpu_adapter = NULL;
    struct aura_gpu_controller *gpu_controller;
    error_t err;
    int adapter_count = 0;
    int i = 0;

    i2c_for_each_dev(&data, aura_gpu_probe_device);
    if (data.error) {
        err = data.error;
        AURA_DBG("aura_gpu_probe_device() Failed with error %d", err);
        goto error;
    }

    if (data.count < MAX_SUPPORTED_GPUS) {
        AURA_DBG("Trying built-in drivers");

        adapter_count = gpu_adapters_create(adapters, ARRAY_SIZE(adapters));
        if (adapter_count < 0) {
            err = adapter_count;
            goto error;
        }

        for (i = 0; i < adapter_count; i++) {
            gpu_controller = aura_gpu_controller_create(adapters[i]);
            if (IS_ERR_OR_NULL(gpu_controller)) {
                gpu_adapter_destroy(adapters[i]);
                adapters[i] = NULL;

                if (gpu_controller) {
                    err = CLEAR_ERR(gpu_controller);
                    goto error;
                }
            } else {
                gpu_adapter = aura_gpu_adapter_create(adapters[i], gpu_adapter_destroy);
                if (IS_ERR(gpu_adapter)) {
                    err = CLEAR_ERR(gpu_adapter);
                    goto error;
                } else {
                    adapters[i] = NULL;
                }
            }
        }
    }

    return data.count;

error:
    for (i = 0; i < adapter_count; i++) {
        if (adapters[i])
            gpu_adapter_destroy(adapters[i]);
    }

    aura_gpu_release();

    return err;
}
