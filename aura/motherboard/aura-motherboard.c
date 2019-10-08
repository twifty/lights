// SPDX-License-Identifier: GPL-2.0
#include "../aura.h"
#include "../aura-controller.h"
#include "aura-smbus.h"

struct aura_smbus_factory smbus_factory[] = {
    { aura_smbus_piix4_adapter_create },
    { aura_smbus_nuvoton_adapter_create },
};

// Only written to in the probe/remove functions (locking not required)
static LIST_HEAD(aura_motherboard_ctrl_list);
static LIST_HEAD(aura_smbus_adapter_list);

struct aura_motherboard_ctrl {
    struct list_head            siblings;
    struct list_head            zone_list;
    struct aura_controller      *aura_ctrl;
};

struct aura_motherboard_zone {
    struct list_head    siblings;
    struct lights_dev   lights;
};

struct callback_data {
    int count;
};

static error_t aura_motherboard_color_read (
    void *data,
    struct lights_io *io
){
    if (WARN_ON(NULL == data || NULL == io || io->type != LIGHTS_TYPE_COLOR))
        return -EINVAL;

    return aura_controller_get_zone_color(data, &io->data.color);
}

static error_t aura_motherboard_color_write (
    void *data,
    const struct lights_io *io
){
    if (WARN_ON(NULL == data || NULL == io || io->type != LIGHTS_TYPE_COLOR))
        return -EINVAL;

    return aura_controller_set_zone_color(data, &io->data.color);
}

static error_t aura_motherboard_color_update (
    const struct lights_state *state
){
    struct aura_motherboard_ctrl *ctrl;

    list_for_each_entry(ctrl, &aura_motherboard_ctrl_list, siblings) {
        aura_controller_set_color(ctrl->aura_ctrl, &state->color);
    }

    return 0;
}

static error_t aura_motherboard_mode_read (
    void *data,
    struct lights_io *io
){
    if (WARN_ON(NULL == data || NULL == io || io->type != LIGHTS_TYPE_MODE))
        return -EINVAL;

    return aura_controller_get_mode(data, &io->data.mode);
}

static error_t aura_motherboard_mode_write (
    void *data,
    const struct lights_io *io
){
    if (WARN_ON(NULL == data || NULL == io || io->type != LIGHTS_TYPE_MODE))
        return -EINVAL;

    return aura_controller_set_mode(data, &io->data.mode);
}

static error_t aura_motherboard_mode_update (
    const struct lights_state *state
){
    struct aura_motherboard_ctrl *ctrl;

    list_for_each_entry(ctrl, &aura_motherboard_ctrl_list, siblings) {
        aura_controller_set_mode(ctrl->aura_ctrl, &state->mode);
    }

    return 0;
}

static error_t aura_motherboard_zone_create (
    struct aura_motherboard_ctrl *ctrl,
    uint8_t index
){
    struct aura_zone *aura_zone;
    struct aura_motherboard_zone *ctrl_zone;
    error_t err;

    aura_zone = aura_controller_get_zone(ctrl->aura_ctrl, index);
    if (IS_ERR(aura_zone))
        return PTR_ERR(aura_zone);

    ctrl_zone = kzalloc(sizeof(*ctrl_zone), GFP_KERNEL);
    if (!ctrl_zone)
        return -ENOMEM;

    ctrl_zone->lights.name = aura_zone->name;
    ctrl_zone->lights.caps = aura_controller_get_caps();
    ctrl_zone->lights.update_color = aura_motherboard_color_update;
    ctrl_zone->lights.update_mode  = aura_motherboard_mode_update;

    err = lights_device_register(&ctrl_zone->lights);
    if (err)
        goto error_free_zone;

    err = lights_create_file(&ctrl_zone->lights, &LIGHTS_ATTR_RW(
        "mode",
        LIGHTS_TYPE_MODE,
        aura_zone,
        aura_motherboard_mode_read,
        aura_motherboard_mode_write
    ));
    if (err)
        goto error_free_zone;

    err = lights_create_file(&ctrl_zone->lights, &LIGHTS_ATTR_RW(
        "color",
        LIGHTS_TYPE_COLOR,
        aura_zone,
        aura_motherboard_color_read,
        aura_motherboard_color_write
    ));
    if (err)
        goto error_free_zone;

    list_add_tail(&ctrl_zone->siblings, &ctrl->zone_list);

    return 0;

error_free_zone:
    kfree(ctrl_zone);

    return err;
}

static void aura_motherboard_zone_destroy (
    struct aura_motherboard_zone *zone
){
    lights_device_unregister(&zone->lights);
    kfree(zone);
}

static void aura_motherboard_ctrl_destroy (
    struct aura_motherboard_ctrl *ctrl
){
    struct aura_motherboard_zone *zone, *safe;

    list_for_each_entry_safe(zone, safe, &ctrl->zone_list, siblings) {
        list_del(&zone->siblings);
        aura_motherboard_zone_destroy(zone);
    }

    aura_controller_destroy(ctrl->aura_ctrl);
    kfree(ctrl);
}

static error_t aura_motherboard_ctrl_create (
    struct aura_controller *aura
){
    struct aura_motherboard_ctrl *ctrl;
    error_t err;
    uint8_t i;

    ctrl = kzalloc(sizeof(*ctrl), GFP_KERNEL);
    if (!ctrl) {
        err = -ENOMEM;
        goto error_free_ctrl;
    }

    ctrl->aura_ctrl = aura;
    INIT_LIST_HEAD(&ctrl->zone_list);

    for (i = 0; i < aura->zone_count; i++) {
        err = aura_motherboard_zone_create(ctrl, i);
        if (err)
            goto error_free_ctrl;
    }

    list_add_tail(&ctrl->siblings, &aura_motherboard_ctrl_list);

    return 0;

error_free_ctrl:
    aura_motherboard_ctrl_destroy(ctrl);

    return err;
}

static struct aura_controller *aura_motherboard_probe_address (
    struct i2c_adapter *adap,
    uint8_t address
){
    struct aura_controller *aura;
    error_t err;

    aura = aura_controller_create(adap, address);
    if (IS_ERR(aura))
        return aura;

    if (aura == NULL) {
        AURA_DBG("aura_controller_create() returned a NULL pointer");
        return ERR_PTR(-ENODEV);
    }

    AURA_DBG("aura controller '%s' found at 0x%02x", aura->name, address);

    err = aura_motherboard_ctrl_create(aura);
    if (err)
        goto error_free_aura;

    return aura;

error_free_aura:
    aura_controller_destroy(aura);

    return ERR_PTR(err);
}

static int aura_motherboard_probe_adapter (
    struct i2c_adapter *adapter
){
    struct aura_controller *ctrl, *slave;
    const unsigned char ctrl_addresses[] = { 0x40, 0x4E };
    uint32_t i, err;
    int found = 0;
    uint8_t next, addr;

    for (i = 0; i < ARRAY_SIZE(ctrl_addresses); i++) {
        ctrl = aura_motherboard_probe_address(adapter, ctrl_addresses[i]);
        if (IS_ERR(ctrl))
            continue;

        found++;

        // Detect any slave devices
        for (addr = 0xAA; addr <= 0xAD; ++addr) {
            err = aura_controller_read_byte(ctrl, 0x8000 | addr, &next);
            if (!err && next) {
                slave = aura_motherboard_probe_address(adapter, next >> 1);
                if (!IS_ERR(slave))
                    found++;
                continue;
            }
            break;
        }
    }

    return found;
}

static error_t aura_motherboard_probe_device (
    struct device *dev,
    void *data
){
    struct callback_data *cb_data;
    error_t found;

    // dev_dbg(aura_dev, "Detected Motherboard: %s", dmi_get_system_info(DMI_BOARD_NAME));
    cb_data = (struct callback_data*)data;
    if (dev->type != &i2c_adapter_type || cb_data->count)
        return cb_data->count;

    found = aura_motherboard_probe_adapter(to_i2c_adapter(dev));
    if (found > 0)
        cb_data->count += found;

    return cb_data->count;
}

error_t aura_motherboard_probe (
    const struct lights_state *state
){
    struct aura_smbus_adapter *smbus;
    int i, found;

    struct callback_data data = {
        .count = 0
    };

    i2c_for_each_dev(&data, aura_motherboard_probe_device);

    for (i = 0; i < ARRAY_SIZE(smbus_factory); i++) {
        smbus = smbus_factory[i].create();
        if (!IS_ERR(smbus)) {
            found = aura_motherboard_probe_adapter(smbus->adapter);
            if (found)
                list_add_tail(&smbus->siblings, &aura_smbus_adapter_list);
            else
                smbus->destroy(smbus);
        }
    }

    aura_motherboard_mode_update(state);
    aura_motherboard_color_update(state);

    return 0;
}

void aura_motherboard_release (
    void
){
    struct aura_motherboard_ctrl *ctrl, *ctrl_safe;
    struct aura_smbus_adapter *smbus, *smbus_safe;

    list_for_each_entry_safe(ctrl, ctrl_safe, &aura_motherboard_ctrl_list, siblings) {
        list_del(&ctrl->siblings);
        aura_motherboard_ctrl_destroy(ctrl);
    }

    list_for_each_entry_safe(smbus, smbus_safe, &aura_smbus_adapter_list, siblings) {
        list_del(&smbus->siblings);
        smbus->destroy(smbus);
    }
}
