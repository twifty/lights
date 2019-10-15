// SPDX-License-Identifier: GPL-2.0
#include "../aura.h"
#include "asic/asic-types.h"
#include "aura-gpu-i2c.h"

#define MAX_SUPPORTED_GPUS 2

/*
    AMD devices require an i2c adapter to be created,
    NVIDIA devices already have the adapter loaded.
 */
static const struct pci_device_id pciidlist[] = {
    {0x1002, 0x67df, 0x1043, 0x0517, 0, 0, CHIP_POLARIS10},     // RX580 (Strix)
    {0x1002, 0x687F, 0x1043, 0x0555, 0, 0, CHIP_VEGA10},        // Vega 56 (Strix)
    // {0x1002, 0x731f, 0x1043, 0x04e2, 0, 0, CHIP_NAVI10},     // RX5700XT (Strix)
    {0, 0, 0},
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

        TODO - How does the card react between power cycles?
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

    /*
        TODO - This requires testing.

        The LightingService uses STATIC mode.

        If the colors are changed but not applied, are they retained across
        a power cycle?
     */
    AURA_INDEX_DIRECT       = 0x05,
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

struct zone_context {
    struct aura_gpu_controller  *gpu_ctrl;
    struct lights_mode          mode;
    struct lights_color         color;
    const struct zone_reg       reg;
};

static LIST_HEAD(aura_gpu_ctrl_list);
struct aura_gpu_controller {
    struct list_head            siblings;
    struct i2c_adapter          *adapter;
    uint8_t                     address;        /* The 7bit chipset address */
    struct aura_i2c_service     *i2c_service;
    struct mutex                i2c_lock;
    uint8_t                     id;

    /* Allow multiple zones for future */
    uint8_t                     zone_count;
    struct zone_context         *zone_contexts;
    // struct aura_fops            *aura_fops;
    struct lights_dev           lights;
    char                        lights_name[6];
};

struct callback_data {
    uint8_t count;
};


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

static const struct lights_mode *aura_gpu_mode_to_lights_mode (
    enum aura_gpu_mode gpu_mode
){
    if (gpu_mode == AURA_MODE_DIRECT) {
        return &aura_gpu_modes[AURA_INDEX_DIRECT];
    } else if (gpu_mode >= 0 && gpu_mode <= AURA_MODE_LAST) {
        return &aura_gpu_modes[AURA_INDEX_DIRECT];
    }

    return NULL;
}

static error_t aura_gpu_i2c_read_byte (
    struct i2c_adapter *adapter,
    uint8_t addr,
    uint8_t offset,
    uint8_t *value
){
    int ret;
    struct i2c_msg msgs[] = {{
        .addr = addr,
        .len = 1,
        .buf = &offset
    },{
        .addr = addr,
        .flags = I2C_M_RD,
        .len = 1,
        .buf = value
    }};

    ret = i2c_transfer(adapter, msgs, 2);
    if (ret == 2)
        return 0;

    return ret;
}

static error_t aura_gpu_i2c_write_byte (
    struct i2c_adapter *adapter,
    uint8_t addr,
    uint8_t offset,
    uint8_t value
){
    int ret;
    struct i2c_msg msgs[] = {{
        .addr = addr,
        .len = 1,
        .buf = &offset
    },{
        .addr = addr,
        .len = 1,
        .buf = &value
    }};

    ret = i2c_transfer(adapter, msgs, 2);
    if (ret == 2)
        return 0;

    return ret;
}

static error_t aura_gpu_discover (
    struct i2c_adapter *adapter,
    uint8_t *addr
){
    uint8_t offset[2] = { AURA_GPU_CHIPSET_MAGIC_HI, AURA_GPU_CHIPSET_MAGIC_LO };
    uint8_t value[2] = {0};
    error_t err;
    int i, j;

    for (j = 0; j < ARRAY_SIZE(chipset_addresses); j++) {
        *addr = chipset_addresses[j];

        for (i = 0; i < 2; i++) {
            err = aura_gpu_i2c_read_byte(adapter, *addr, offset[i], &value[i]);
            if (err) {
                AURA_DBG("Failed to read offset 0x%02x on address 0x%02x: %d", offset[i], *addr, err);
                break;
            }
        }

        if (!err && ((value[0] << 8) | value[1]) == AURA_GPU_CHIPSET_MAGIC_VALUE) {
            AURA_DBG("Discovered aura chip at address %x on %s", *addr, adapter->name);
            return 0;
        }
    }

    return -ENODEV;
}

static error_t aura_gpu_fetch_zone (
    struct zone_context *zone
){
    enum aura_gpu_mode gpu_mode;
    const struct lights_mode *mode;
    struct lights_color color;
    struct i2c_adapter *adapter = zone->gpu_ctrl->adapter;
    uint8_t address = zone->gpu_ctrl->address;
    uint8_t mode_raw;
    error_t err;

    mutex_lock(&zone->gpu_ctrl->i2c_lock);

    err = aura_gpu_i2c_read_byte(adapter, address, zone->reg.red, &color.r);
    if (err)
        goto error;

    err = aura_gpu_i2c_read_byte(adapter, address, zone->reg.green, &color.g);
    if (err)
        goto error;

    err = aura_gpu_i2c_read_byte(adapter, address, zone->reg.blue, &color.b);
    if (err)
        goto error;

    err = aura_gpu_i2c_read_byte(adapter, address, zone->reg.mode, &mode_raw);
    if (err)
        goto error;

    /* Detmine the mode base on the values */
    if (mode_raw >= AURA_MODE_BREATHING && mode_raw <= AURA_MODE_LAST){
        gpu_mode = mode_raw;
    } else if (mode_raw <= AURA_MODE_STATIC) {
        if (0 == zone->color.r && 0 == zone->color.g && 0 == zone->color.b) {
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

    memcpy((void*)&zone->mode, &mode, sizeof(mode));
    memcpy((void*)&zone->color, &color, sizeof(color));

error:
    mutex_unlock(&zone->gpu_ctrl->i2c_lock);

    return err;
}


static error_t aura_gpu_apply_zone_color (
    struct zone_context *zone,
    const struct lights_color *color
){
    struct i2c_adapter *adapter = zone->gpu_ctrl->adapter;
    uint8_t address = zone->gpu_ctrl->address;
    error_t err;

    mutex_lock(&zone->gpu_ctrl->i2c_lock);

    err = aura_gpu_i2c_write_byte(adapter, address, zone->reg.red, color->r);
    if (err)
        goto error;

    err = aura_gpu_i2c_write_byte(adapter, address, zone->reg.green, color->g);
    if (err)
        goto error;

    err = aura_gpu_i2c_write_byte(adapter,
        address,
        zone->reg.blue,color->b
    );
    if (err)
        goto error;

    if (!err && lights_custom_mode_id(&zone->mode) == AURA_MODE_DIRECT) {
        err = aura_gpu_i2c_write_byte(adapter, address, zone->reg.apply, 0x01);
    }

    memcpy((void*)&zone->color, color, sizeof(*color));

error:
    mutex_unlock(&zone->gpu_ctrl->i2c_lock);

    return err;
}

static error_t aura_gpu_apply_zone_mode (
    struct zone_context *zone,
    const struct lights_mode *mode
){
    struct i2c_adapter *adapter = zone->gpu_ctrl->adapter;
    uint8_t address = zone->gpu_ctrl->address;
    enum aura_gpu_mode gpu_mode;
    error_t err;

    mutex_lock(&zone->gpu_ctrl->i2c_lock);

    err = lights_mode_to_aura_gpu_mode(mode, &gpu_mode);
    if (err)
        goto error;

    err = aura_gpu_i2c_write_byte(adapter, address, zone->reg.mode, gpu_mode);
    if (err)
        goto error;

    if (!err && lights_custom_mode_id(&zone->mode) == AURA_MODE_DIRECT) {
        err = aura_gpu_i2c_write_byte(adapter, address, zone->reg.apply, 0x01);
    }

    memcpy((void*)&zone->mode, mode, sizeof(*mode));

error:
    mutex_unlock(&zone->gpu_ctrl->i2c_lock);

    return err;
}

static error_t aura_gpu_get_mode (
    void *data,
    struct lights_io *io
){
    struct zone_context *zone = data;

    if (WARN_ON(NULL == zone || NULL == io))
        return -EINVAL;

    io->data.mode = zone->mode;

    return 0;
}

static error_t aura_gpu_set_mode (
    void *data,
    const struct lights_io *io
){
    struct zone_context *zone = data;

    if (WARN_ON(NULL == zone || NULL == io || io->type != LIGHTS_TYPE_MODE))
        return -EINVAL;

    return aura_gpu_apply_zone_mode(zone, &io->data.mode);
}

static error_t aura_gpu_update_mode (
    const struct lights_state *state
){
    struct aura_gpu_controller *ctrl;
    error_t err;

    list_for_each_entry(ctrl, &aura_gpu_ctrl_list, siblings) {
        err = aura_gpu_apply_zone_mode(&ctrl->zone_contexts[0], &state->mode);
        if (err)
            break;
    }

    return err;
}

static error_t aura_gpu_get_color (
    void *data,
    struct lights_io *io
){
    struct zone_context *zone = data;

    if (WARN_ON(NULL == zone || NULL == io))
        return -EINVAL;

    io->data.color = zone->color;

    return 0;
}

static error_t aura_gpu_set_color (
    void *data,
    const struct lights_io *io
){
    struct zone_context *zone = data;

    if (WARN_ON(NULL == zone || NULL == io || io->type != LIGHTS_TYPE_COLOR))
        return -EINVAL;

    return aura_gpu_apply_zone_color(zone, &io->data.color);
}

static error_t aura_gpu_update_color (
    const struct lights_state *state
){
    struct aura_gpu_controller *ctrl;
    error_t err;

    list_for_each_entry(ctrl, &aura_gpu_ctrl_list, siblings) {
        err = aura_gpu_apply_zone_color(&ctrl->zone_contexts[0], &state->color);
        if (err)
            break;
    }

    return err;
}


static error_t aura_gpu_create_fs (
    struct aura_gpu_controller *gpu_ctrl
){
    uint8_t id = gpu_ctrl->id;
    error_t err;

    gpu_ctrl->lights.name = gpu_ctrl->lights_name;
    gpu_ctrl->lights.caps = aura_gpu_modes;
    gpu_ctrl->lights.update_mode  = aura_gpu_update_mode;
    gpu_ctrl->lights.update_color = aura_gpu_update_color;

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
    err = lights_create_file(&gpu_ctrl->lights, &LIGHTS_ATTR_RW(
        "mode",
        LIGHTS_TYPE_MODE,
        &gpu_ctrl->zone_contexts[0],
        aura_gpu_get_mode,
        aura_gpu_set_mode
    ));
    if (err)
        goto error_release;

    err = lights_create_file(&gpu_ctrl->lights, &LIGHTS_ATTR_RW(
        "color",
        LIGHTS_TYPE_COLOR,
        &gpu_ctrl->zone_contexts[0],
        aura_gpu_get_color,
        aura_gpu_set_color
    ));
    if (err)
        goto error_release;

    return 0;

error_release:
    lights_device_unregister(&gpu_ctrl->lights);

    return err;
}

static uint8_t aura_gpu_count (
    void
){
    struct list_head *iter;
    uint8_t count = 0;

    list_for_each(iter, &aura_gpu_ctrl_list)
        count++;

    return count;
}

static struct aura_gpu_controller *aura_gpu_device_create (
    struct i2c_adapter *adapter
){
    struct aura_gpu_controller *gpu_ctrl;
    struct zone_context *zone_ctx;
    uint8_t zone_count = 1;
    uint8_t addr;
    error_t err;

    /* Check for the presence of the chip and check its id */
    err = aura_gpu_discover(adapter, &addr);
    if (err)
        return ERR_PTR(err);

    gpu_ctrl = kzalloc(sizeof(*gpu_ctrl), GFP_KERNEL);
    if (!gpu_ctrl)
        return ERR_PTR(-ENOMEM);

    gpu_ctrl->address = addr;
    gpu_ctrl->adapter = adapter;

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

    return gpu_ctrl;

error_free_zone:
    kfree(zone_ctx);
error_free_ctrl:
    kfree(gpu_ctrl);

    return ERR_PTR(err);
}

static error_t aura_gpu_probe_device (
    struct device *dev,
    void *data
){
    struct callback_data *cb_data;
    struct i2c_adapter *adapter = to_i2c_adapter(dev);
    struct aura_gpu_controller *found;

    cb_data = (struct callback_data*)data;
    if (dev->type != &i2c_adapter_type || cb_data->count >= MAX_SUPPORTED_GPUS)
        return cb_data->count;

    found = aura_gpu_device_create(adapter);
    if (!IS_ERR(found))
        cb_data->count++;

    return cb_data->count;
}

error_t aura_gpu_probe (
    const struct lights_state *state
){
    struct pci_dev *pci_dev;
    const struct pci_device_id *match;
    struct aura_i2c_service *i2c_service;
    struct aura_gpu_controller *gpu_ctrl;
    struct callback_data data = {0};

    i2c_for_each_dev(&data, aura_gpu_probe_device);

    /* Handle the case of mixed GPU types */
    pci_dev = NULL;
    while (data.count < MAX_SUPPORTED_GPUS) {
        while (NULL != (pci_dev = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, pci_dev))) {
            match = pci_match_id(pciidlist, pci_dev);
            if (match) {
                AURA_DBG("Creating adapter for GPU %x:%x", pci_dev->subsystem_vendor, pci_dev->subsystem_device);

                i2c_service = aura_gpu_i2c_create(pci_dev, match->driver_data);
                if (IS_ERR(i2c_service))
                    continue;

                gpu_ctrl = aura_gpu_device_create(i2c_service->adapter);
                if (IS_ERR(gpu_ctrl)) {
                    aura_gpu_i2c_destroy(i2c_service);
                    i2c_service = NULL;
                    continue;
                }

                gpu_ctrl->i2c_service = i2c_service;
                data.count++;

                break;
            }
        }
        if (NULL == pci_dev)
            break;
    }

    aura_gpu_update_mode(state);
    aura_gpu_update_color(state);

    return data.count;
}

static void aura_gpu_dev_release (
    struct aura_gpu_controller *gpu_ctrl
){
    if (gpu_ctrl->i2c_service) {
        aura_gpu_i2c_destroy(gpu_ctrl->i2c_service);
    }

    if (gpu_ctrl->zone_contexts) {
        kfree(gpu_ctrl->zone_contexts);
    }

    lights_device_unregister(&gpu_ctrl->lights);

    kfree(gpu_ctrl);
}

void aura_gpu_release (
    void
){
    struct aura_gpu_controller *gpu_ctrl, *safe;

    if (!list_empty(&aura_gpu_ctrl_list)) {
        list_for_each_entry_safe(gpu_ctrl, safe, &aura_gpu_ctrl_list, siblings) {
            list_del(&gpu_ctrl->siblings);
            aura_gpu_dev_release(gpu_ctrl);
        }
    }
}
