// SPDX-License-Identifier: GPL-2.0
#include "../aura.h"
#include "../trait/aura-controller.h"

enum {
    SPD_TYPE_DDR0 = 0x7,
    SPD_TYPE_DDR2 = 0x8,
    SPD_TYPE_DDR3 = 0xB,
    SPD_TYPE_DDR4 = 0xC
};

struct aura_memory_spd {
    u16     size;
    uint8_t addr;
    uint8_t type;
    uint8_t slot;
    uint8_t aura;                   /* The bus address of the aura device */
    uint8_t offset;                 /* The offset into the available_addresses array */
};

struct aura_memory_controller {
    struct list_head        siblings;
    struct aura_memory_spd  spd;
    struct aura_controller  *aura_ctrl;
    struct lights_dev       lights;
    uint8_t                 version;
    uint8_t                 led_count;
    char                    name[8]; // "dimm-n"
};

struct callback_data {
    int count;
};

static const char *led_names[] = {
    "led-1", "led-2", "led-3", "led-4", "led-5", "led-6", "led-7", "led-8",
};

static const uint8_t aura_memory_rgb_triplets[] = {
    0x52, 0x47, 0x42,  0x02, 0x01, 0x01,  0x03, 0x01, 0x01,  0x04, 0x01, 0x01,
    0x05, 0x01, 0x01,  0x06, 0x01, 0x01,  0x10, 0x01, 0x01,  0x11, 0x01, 0x01,
    0x07, 0x02, 0x01,  0x08, 0x02, 0x01,  0x09, 0x02, 0x01,  0x10, 0x02, 0x01,
    0x11, 0x02, 0x01,  0x12, 0x01, 0x01,  0x12, 0x02, 0x01,  0x10, 0x02, 0x01,
    0x04, 0x02, 0x01,  0x02, 0x02, 0x01,  0x05, 0x02, 0x01,  0x06, 0x02, 0x01,
    0x00, };

/*
    LightingService doesn't include the values 0x78-0x7F and instead
    allocates them dynamically in reverse order.
 */
static const uint8_t aura_memory_available_addresses[] = {
    0x70, 0x71, 0x73, 0x74, 0x75, 0x76, 0x78, 0x79, 0x7A,
    0x7B, 0x7C, 0x7D, 0x7E, 0x7F, 0x4F, 0x66, 0x67
};

static LIST_HEAD(aura_memory_controller_list);

static uint8_t aura_memory_rgb_triplet_exists (
    uint8_t *rgb
){
    const uint8_t *p = aura_memory_rgb_triplets;

    for (p = aura_memory_rgb_triplets; *p; p += 3) {
        if (memcmp(p, rgb, 3) == 0)
            return 1;
    }

    return 0;
}

static error_t aura_memory_create_aura_address (
    struct aura_memory_controller *node
){
    struct aura_memory_controller *prev;
    uint8_t offset;

    if (node->spd.aura) {
        offset = node->spd.offset + 1;
    } else if (!list_empty(&aura_memory_controller_list)) {
        prev = list_last_entry(&aura_memory_controller_list, typeof(*node), siblings);
        offset = prev->spd.offset + 1;
    } else {
        offset = 0;
    }

    AURA_DBG("Using available address offset %d", offset);
    if (offset >= sizeof(aura_memory_available_addresses))
        return -EDQUOT;

    node->spd.offset = offset;
    node->spd.aura = aura_memory_available_addresses[offset];

    return 0;
}


static error_t aura_memory_color_read (
    void *data,
    struct lights_io *io
){
    if (WARN_ON(NULL == data || NULL == io || io->type != LIGHTS_TYPE_COLOR))
        return -EINVAL;

    return aura_controller_get_zone_color(data, &io->data.color);
}

static error_t aura_memory_color_write (
    void *data,
    const struct lights_io *io
){
    if (WARN_ON(NULL == data || NULL == io || io->type != LIGHTS_TYPE_COLOR))
        return -EINVAL;

    return aura_controller_set_zone_color(data, &io->data.color);
}

static error_t aura_memory_color_update (
    const struct lights_state *state
){
    struct aura_memory_controller *ctrl;
    error_t err;

    list_for_each_entry(ctrl, &aura_memory_controller_list, siblings) {
        err = aura_controller_set_color(ctrl->aura_ctrl, &state->color);
        if (err) {
            AURA_DBG("aura_controller_set_color() filed with code %d", err);
            return err;
        }
    }

    return 0;
}

static error_t aura_memory_mode_read (
    void *data,
    struct lights_io *io
){
    if (WARN_ON(NULL == data || NULL == io || io->type != LIGHTS_TYPE_MODE))
        return -EINVAL;

    return aura_controller_get_mode(data, &io->data.mode);
}

static error_t aura_memory_mode_write (
    void *data,
    const struct lights_io *io
){
    if (WARN_ON(NULL == data || NULL == io || io->type != LIGHTS_TYPE_MODE))
        return -EINVAL;

    return aura_controller_set_mode(data, &io->data.mode);
}

static error_t aura_memory_mode_update (
    const struct lights_state *state
){
    struct aura_memory_controller *node;

    list_for_each_entry(node, &aura_memory_controller_list, siblings) {
        aura_controller_set_mode(node->aura_ctrl, &state->mode);
    }

    return 0;
}

static error_t aura_memory_create_zone (
    struct aura_memory_controller *ctrl
){
    struct aura_zone *aura_zone;
    int i;
    uint8_t id;
    error_t err;

    ctrl->lights.name = ctrl->name;
    ctrl->lights.caps = aura_controller_get_caps();
    ctrl->lights.update_color = aura_memory_color_update;
    ctrl->lights.update_mode = aura_memory_mode_update;

    snprintf(ctrl->name, sizeof(ctrl->name), "dimm-%d", ctrl->spd.slot);
    err = lights_device_register(&ctrl->lights);
    if (err)
        return err;

    aura_zone = aura_controller_get_zone(ctrl->aura_ctrl, ZONE_ID_ALL);
    if (IS_ERR(aura_zone)) {
        AURA_DBG("Failed to get aura zone 'all'");
        err = PTR_ERR(aura_zone);
        goto error_unregister;
    }

    /* Create a color and mode file which adjusts all leds */
    err = lights_create_file(&ctrl->lights, &LIGHTS_ATTR_RW(
        "mode",
        LIGHTS_TYPE_MODE,
        aura_zone,
        aura_memory_mode_read,
        aura_memory_mode_write
    ));
    if (err)
        goto error_unregister;

    err = lights_create_file(&ctrl->lights, &LIGHTS_ATTR_RW(
        "color",
        LIGHTS_TYPE_COLOR,
        aura_zone,
        aura_memory_color_read,
        aura_memory_color_write
    ));
    if (err)
        goto error_unregister;

    /* Create a color file for each led */
    for (i = 2, id = 0; id < ctrl->aura_ctrl->zone_count && id < 8; i++, id++) {
        aura_zone = aura_controller_get_zone(ctrl->aura_ctrl, id);
        if (IS_ERR(aura_zone)) {
            AURA_DBG("Failed to get aura zone %d", id);
            err = PTR_ERR(aura_zone);
            goto error_unregister;
        }

        err = lights_create_file(&ctrl->lights, &LIGHTS_ATTR_RW(
            led_names[id],
            LIGHTS_TYPE_COLOR,
            aura_zone,
            aura_memory_color_read,
            aura_memory_color_write
        ));
        if (err)
            goto error_unregister;
    }

    return 0;

error_unregister:
    lights_device_unregister(&ctrl->lights);

    return err;
}


static error_t aura_memory_test_address (
    struct i2c_adapter *adap,
    uint8_t addr
){
    uint8_t data = 0x01;
    struct i2c_msg msg = {
        .addr = addr,
        .flags = 0,
        .buf = &data,
        .len = 1
    };

    if (1 == i2c_transfer(adap, &msg, 1))
        return 0;

    return -ENODEV;
}

static error_t smbus_read_byte (
    struct i2c_adapter *adap,
    uint8_t addr,
    uint8_t offset,
    uint8_t *value
){
    int response;
    struct i2c_client client = {
        .adapter = adap,
        .addr = addr
    };

    response = i2c_smbus_read_byte_data(&client, offset);
    if (response < 0)
        return response;

    *value = (response & 0xff);

    return 0;
}

static error_t smbus_write_byte (
    struct i2c_adapter *adap,
    uint8_t addr,
    uint8_t offset,
    uint8_t value
){
    struct i2c_client client = {
        .adapter = adap,
        .addr = addr
    };

    return i2c_smbus_write_byte_data(&client, offset, value);
}

static error_t aura_memory_create_controller (
    struct i2c_adapter *adap,
    const struct aura_memory_spd *spd
){
    struct aura_memory_controller *node;
    struct aura_controller *aura_ctrl;
    struct i2c_client client = {
        .adapter = adap,
        .addr = 0x77
    };
    uint8_t i;
    error_t err;

    node = kzalloc(sizeof(struct aura_memory_controller), GFP_KERNEL);
    if (!node)
        return -ENOMEM;

    node->spd = *spd;
    aura_ctrl = NULL;

    // TODO - Should we use a muxed region?

    // Attempt to write a word to 0x80F8
    err = i2c_smbus_write_word_swapped(&client, 0x00, 0x80F8);
    if (err == 0) {
        AURA_DBG("Loading aura devices");

        for (i = 0; i < sizeof(aura_memory_available_addresses); i++) {
            err = aura_memory_create_aura_address(node);
            if (err)
                goto error;

            // Register the slot change
            err = i2c_smbus_write_byte_data(&client, 0x01, node->spd.slot);
            if (err) {
                AURA_DBG("Failed to set slot number");
                continue;
            }

            // Test the address
            err = aura_memory_test_address(adap, node->spd.aura);
            if (err == 0) {
                AURA_DBG("bus address 0x%02x is in use", node->spd.aura);

                // This could happen if only some of the DIMMs were mapped
                AURA_DBG("Attempting to load aura controller");
                aura_ctrl = aura_controller_create(adap, node->spd.aura);
                if (IS_ERR(aura_ctrl)) {
                    AURA_DBG("attempt failed with %ld", PTR_ERR(aura_ctrl));
                    continue;
                } else {
                    goto found;
                }
            } else {
                break;
            }
        }

        // Register the new address
        err = i2c_smbus_write_word_swapped(&client, 0x00, 0x80F9);
        if (err) {
            AURA_DBG("Failed to register new address");
            goto error;
        }

        err = i2c_smbus_write_byte_data(&client, 0x01, node->spd.aura << 1);
        if (err) {
            AURA_DBG("Failed to apply new address");
            goto error;
        }

        aura_ctrl = aura_controller_create(adap, node->spd.aura);
        if (IS_ERR(aura_ctrl)) {
            AURA_DBG("aura_controller_create() failed with %ld", PTR_ERR(aura_ctrl));
            err = PTR_ERR(aura_ctrl);
            goto error;
        }
        AURA_DBG("aura device found at 0x%02x", node->spd.aura);
    } else {
        AURA_DBG("Detecting aura devices");

        // This can happen if the memory has already been mapped. Which can
        // happen if the module was unloaded, or switching from windows.
        // We need to iterate the available addresses and map the aura device
        // back to a slot number. The slot to address mapping is lost.

        // Let's try avoiding an infinite loop
        for (i = 0; i < sizeof(aura_memory_available_addresses); i++) {
            err = aura_memory_create_aura_address(node);
            if (!err) {
                aura_ctrl = aura_controller_create(adap, node->spd.aura);
                if (IS_ERR(aura_ctrl)) {
                    AURA_DBG("aura_controller_create() failed with %ld", PTR_ERR(aura_ctrl));
                    err = PTR_ERR(aura_ctrl);
                    goto error;
                }
                AURA_DBG("aura device found at 0x%02x", node->spd.aura);
                break;
            }
        }
    }

found:
    node->aura_ctrl = aura_ctrl;

    err = aura_memory_create_zone(node);
    if (err) {
        AURA_DBG("aura_memory_create_zone() failed with %d", err);
        aura_controller_destroy(aura_ctrl);
        goto error;
    }

    list_add_tail(&node->siblings, &aura_memory_controller_list);

    return 0;
error:
    kfree(node);

    return err;
}

static error_t aura_memory_probe_adapter (
    struct device *dev,
    void *data
){
    struct i2c_adapter *adap = to_i2c_adapter(dev);
    struct callback_data *cb_data;
    struct aura_memory_spd spd[16] = {0};
    uint8_t addr, page, size, count, rgb[3], c, i;
    error_t err;

    // Return if not an adapter or already found
    cb_data = (struct callback_data*)data;
    if (dev->type != &i2c_adapter_type || cb_data->count)
        return cb_data->count;

    for (count = 0, addr = 0x50; addr <= 0x5F; addr++) {
        AURA_DBG("Pinging %s address 0x%02x", adap->name, addr);

        // Select page 0 on all DIMMs
        err = smbus_write_byte(adap, 0x36, 0x00, 0x00);
        if (err) {
            /* No pager on this bus indicates no DIMMs */
            break;
        }

        // Read SPD type
        err = smbus_read_byte(adap, addr, 0x02, &spd[count].type);
        if (err) {
            AURA_DBG("Failed to read SPD type");
            continue;
        }

        // Read SPD size
        err = smbus_read_byte(adap, addr, 0x00, &size);
        if (err) {
            AURA_DBG("Failed to read SPD size");
            continue;
        }

        AURA_DBG("Calculating size from type=0x%02x size=0x%02x", spd[count].type, size);
        switch (spd[count].type) {
            case SPD_TYPE_DDR0:
            case SPD_TYPE_DDR2:
                if (size == 0 || size > 0x0E)
                    continue;
                spd[count].size = 1 << size;
                break;
            case SPD_TYPE_DDR3:
                spd[count].size = (size & 0x10) << 4;
                break;
            case SPD_TYPE_DDR4:
                spd[count].size = (!(size & 0x20) && (size & 0x10)) ? 0x100 : 0x200;
                break;
            default:
                continue;
        }

        spd[count].addr = addr;
        spd[count].slot = addr - 0x50;

        AURA_DBG("Detected DIMM slot=%d addr=0x%02x type=0x%02x size=0x%04x",
            spd[count].slot, spd[count].addr, spd[count].type, spd[count].size);

        count += 1;
    }

    for (i = 0; i < count; i++) {
        // Select page according to size
        page = spd[i].size >= 0x100 ? 0x37 : 0x36;
        AURA_DBG("Selecting page %d for all DIMMs", page == 0x36 ? 0 : 1);
        err = smbus_write_byte(adap, page, 0x00, 0x00);
        if (err)
            goto error;

        for (c = 0; c < 3; c++ ) {
            err = smbus_read_byte(adap, spd[i].addr, 0xF0 + c, &rgb[c]);
            if (err)
                goto error;
        }

        AURA_DBG("Found RGB triplet: 0x%02x 0x%02x 0x%02x", rgb[0], rgb[1], rgb[2]);

        // Return to page 1
        if (page == 0x37) {
            AURA_DBG("Selecting page 0 for all DIMMs");
            smbus_write_byte(adap, 0x36, 0x00, 0x00);
        }

        // Check if they are known values
        if (aura_memory_rgb_triplet_exists(rgb)) {
            err = aura_memory_create_controller(adap, &spd[i]);
            if (err) {
                AURA_DBG("aura_memory_create_controller() failed with code %d", err);
                goto error;
            }

            cb_data->count += 1;
        }
    }

    return cb_data->count;

error:
    return err;
}

error_t aura_memory_probe (
    const struct lights_state *state
){
    error_t err;
    struct callback_data data = {
        .count = 0
    };

    err = i2c_for_each_dev(&data, aura_memory_probe_adapter);
    if (err < 0 || data.count == 0)
        return err;

    err = aura_memory_mode_update(state);
    if (err)
        return err;

    err = aura_memory_color_update(state);

    return err;
}

void aura_memory_ctrl_destroy (
    struct aura_memory_controller *ctrl
){
    if (ctrl->aura_ctrl)
        aura_controller_destroy(ctrl->aura_ctrl);

    lights_device_unregister(&ctrl->lights);
    kfree(ctrl);
}

void aura_memory_release (
    void
){
    struct aura_memory_controller *ctrl, *safe;

    list_for_each_entry_safe(ctrl, safe, &aura_memory_controller_list, siblings) {
        list_del(&ctrl->siblings);
        aura_memory_ctrl_destroy(ctrl);
    }
}
