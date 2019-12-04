// SPDX-License-Identifier: GPL-2.0
#include <adapter/smbus/factory.h>
#include <adapter/lights-adapter.h>

#include <aura/debug.h>
#include <aura/controller/aura-controller.h>

// Only written to in the probe/remove functions (locking not required)
static LIST_HEAD(aura_motherboard_ctrl_list);
static LIST_HEAD(aura_smbus_adapter_list);

/**
 * struct aura_smbus_adapter - Storage for i2c/async
 *
 * @siblings:       Next and Prev pointers
 * @i2c_adapter:    Physical access point to the hardware
 * @i2c_destroy:    Destructor for @i2c_adapter
 *
 * The @i2c_destroy method is only valid if an adapter was created
 * by this module.
 */
struct aura_smbus_adapter {
    struct list_head        siblings;
    struct i2c_adapter      *i2c_adapter;
    i2c_destroy_t           i2c_destroy;
};

/**
 * struct aura_motherboard_ctrl - Storage for the zones
 *
 * @siblings:  Next and Prev pointers
 * @zone_list: List of struct aura_motherboard_zone
 * @aura_ctrl: AURA controller for all zones
 */
struct aura_motherboard_ctrl {
    struct list_head            siblings;
    struct list_head            zone_list;
    struct aura_controller      *aura_ctrl;
};

/**
 * struct aura_motherboard_zone - Storage for each zone
 *
 * @siblings: Next and Prev pointers
 * @lights:   Userland access to the zone
 */
struct aura_motherboard_zone {
    struct list_head    siblings;
    struct lights_dev   lights;
};

struct callback_data {
    int     count;
    error_t error;
};

/**
 * aura_motherboard_color_read() - Reads the color of a single zone
 *
 * @data:  A struct aura_zone
 * @state: Buffer to read into
 *
 * @return: Zero or a negative error code
 */
static error_t aura_motherboard_color_read (
    void *data,
    struct lights_state *state
){
    if (IS_NULL(data, state) || IS_FALSE(state->type & LIGHTS_TYPE_COLOR))
        return -EINVAL;

    return aura_controller_get_zone_color(data, &state->color);
}

/**
 * aura_motherboard_color_write() - Writes a single zone color
 *
 * @data:  A struct aura_zone
 * @state: Buffer to read from
 *
 * @return: Zero or a negative error code
 */
static error_t aura_motherboard_color_write (
    void *data,
    const struct lights_state *state
){
    if (IS_NULL(data, state) || IS_FALSE(state->type & LIGHTS_TYPE_COLOR))
        return -EINVAL;

    return aura_controller_set_zone_color(data, &state->color);
}

/**
 * aura_motherboard_color_read() - Reads the mode of a single zone
 *
 * @data:  A struct aura_zone
 * @state: Buffer to read into
 *
 * @return: Zero or a negative error code
 */
static error_t aura_motherboard_mode_read (
    void *data,
    struct lights_state *state
){
    if (IS_NULL(data, state) || IS_FALSE(state->type & LIGHTS_TYPE_MODE))
        return -EINVAL;

    return aura_controller_get_mode(data, &state->mode);
}

/**
 * aura_motherboard_color_write() - Writes a single zone mode
 *
 * @data:  A struct aura_zone
 * @state: Buffer to read from
 *
 * @return: Zero or a negative error code
 */
static error_t aura_motherboard_mode_write (
    void *data,
    const struct lights_state *state
){
    if (IS_NULL(data, state) || IS_FALSE(state->type & LIGHTS_TYPE_MODE))
        return -EINVAL;

    return aura_controller_set_mode(data, &state->mode);
}

static error_t aura_motherboard_update (
    void *data,
    const struct lights_state *state
){
    error_t err;

    if (IS_NULL(data, state) || IS_FALSE(state->type & LIGHTS_TYPE_MODE))
        return -EINVAL;

    if (state->type & LIGHTS_TYPE_MODE) {
        err = aura_controller_set_mode(data, &state->mode);
        if (err)
            return err;
    }

    if (state->type & LIGHTS_TYPE_COLOR) {
        err = aura_controller_set_color(data, &state->color);
        if (err)
            return err;
    }

    return 0;
}


/**
 * aura_motherboard_zone_create() - Sets up the Userland files for the zone
 *
 * @ctrl:  Owning controller
 * @index: Zero based index of the zone
 *
 * @return: Zero or a negative error number
 */
static error_t aura_motherboard_zone_create (
    struct aura_motherboard_ctrl *ctrl,
    uint8_t index
){
    struct aura_zone *aura_zone = aura_controller_get_zone(ctrl->aura_ctrl, index);
    struct lights_io_attribute attrs[] = {
        LIGHTS_MODE_ATTR(
            aura_zone,
            aura_motherboard_mode_read,
            aura_motherboard_mode_write
        ),
        LIGHTS_COLOR_ATTR(
            aura_zone,
            aura_motherboard_color_read,
            aura_motherboard_color_write
        )
    };
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

    err = lights_device_register(&ctrl_zone->lights);
    if (err)
        goto error_free_zone;

    err = lights_create_files(&ctrl_zone->lights, attrs, ARRAY_SIZE(attrs));
    if (err) {
        lights_device_unregister(&ctrl_zone->lights);
        goto error_free_zone;
    }

    list_add_tail(&ctrl_zone->siblings, &ctrl->zone_list);

    return 0;

error_free_zone:
    kfree(ctrl_zone);

    return err;
}

/**
 * aura_motherboard_zone_destroy() - Removes all Userland access
 *
 * @zone: The zone to free
 */
static void aura_motherboard_zone_destroy (
    struct aura_motherboard_zone *zone
){
    lights_device_unregister(&zone->lights);
    kfree(zone);
}

/**
 * aura_motherboard_ctrl_destroy() - Destroys the Controller and all zones
 *
 * @ctrl: The controller to free
 */
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

/**
 * aura_motherboard_ctrl_create() - Creates Userland access to the AURA controller
 *
 * @aura: The detected AURA controller
 *
 * @return: Zero or a negative error code
 */
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


/**
 * aura_motherboard_adapter_create() - Caches the adapter and destructor
 *
 * @i2c_adapter: Raw access point
 * @i2c_destroy: Optional destructor for the @i2c_adapter
 *
 * @return: The async interface or a negative error code
 */
static struct aura_smbus_adapter *aura_motherboard_adapter_create (
    struct i2c_adapter *i2c_adapter,
    i2c_destroy_t i2c_destroy
){
    struct aura_smbus_adapter *smbus_adapter;

    if (IS_NULL(i2c_adapter, i2c_destroy))
        return ERR_PTR(-EINVAL);

    smbus_adapter = kzalloc(sizeof(*smbus_adapter), GFP_KERNEL);
    if (!smbus_adapter)
        return ERR_PTR(-ENOMEM);

    smbus_adapter->i2c_adapter = i2c_adapter;
    smbus_adapter->i2c_destroy = i2c_destroy;

    list_add_tail(&smbus_adapter->siblings, &aura_smbus_adapter_list);

    return smbus_adapter;
}

/**
 * aura_motherboard_adapter_destroy() - Removes async access and releases memory
 *
 * @adapter: Previously registered async access
 */
static void aura_motherboard_adapter_destroy (
    struct aura_smbus_adapter *adapter
){
    if (IS_NULL(adapter))
        return;

    list_del(&adapter->siblings);

    adapter->i2c_destroy(adapter->i2c_adapter);

    kfree(adapter);
}

/**
 * aura_motherboard_probe_address() - Tests the address for an AURA controller
 *
 * @i2c_adapter: access to the hardware device
 * @address:     The address to search
 *
 * @return: Null if not found, a controller on success or a negative error code
 *
 * This function will only return an error on catastophic failure. The returned
 * controller will have all zones and uerland access. Although an async device
 * is given, the actual probing of the device is done synchronously.
 */
static struct aura_controller *aura_motherboard_probe_address (
    struct i2c_adapter *i2c_adapter,
    uint8_t address
){
    struct aura_controller *aura;
    error_t err;

    aura = aura_controller_create(&LIGHTS_SMBUS_CLIENT(i2c_adapter, address, 0));
    if (IS_ERR(aura))
        return aura;

    if (aura == NULL) {
        AURA_DBG("aura_controller_create() returned a NULL pointer");
        return NULL;
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

/**
 * aura_motherboard_probe_adapter() - Searches the adapter for AURA controllers
 *
 * @i2c_adapter: Access to the hardware device
 *
 * @return: Number of found controllers
 *
 * This function tests all known AURA address, and when one is found will
 * also search for any slave controllers.
 */
static int aura_motherboard_probe_adapter (
    struct i2c_adapter *i2c_adapter
){
    struct aura_controller *ctrl;
    struct aura_controller *slaves[4] = {0};
    const unsigned char ctrl_addresses[] = { 0x40, 0x4E };
    uint32_t i, j;
    int created = 0;
    int found = 0;
    error_t err = 0;

    for (i = 0; i < ARRAY_SIZE(ctrl_addresses); i++) {
        ctrl = aura_motherboard_probe_address(i2c_adapter, ctrl_addresses[i]);
        if (ctrl == NULL)
            continue;

        if (IS_ERR(ctrl)) {
            err = CLEAR_ERR(ctrl);
            break;
        }

        created++;

        found = aura_controller_create_slaves(ctrl, slaves, ARRAY_SIZE(slaves));
        if (found > 0) {
            for (j = 0; j < found; j++) {
                if (!err)
                    err = aura_motherboard_ctrl_create(slaves[j]);
                if (!err)
                    continue;
                /* Only destroy remainder and current. Other are in list */
                aura_controller_destroy(slaves[j]);
            }

            created += found;
        } else {
            err = found;
        }
    }

    return err ? err : created;
}

/**
 * aura_motherboard_probe_device() - Callback for @i2c_for_each_dev
 *
 * @dev:  An i2c device
 * @data: count of found controllers and/or any error number
 *
 * @return: The number of found controllers or a negative error number
 */
static int aura_motherboard_probe_device (
    struct device *dev,
    void *data
){
    struct callback_data *cb_data = data;
    // struct aura_smbus_adapter *smbus_adapter;
    error_t found;

    if (dev->type != &i2c_adapter_type || cb_data->count)
        return cb_data->count;
    if (cb_data->error)
        return cb_data->error;

    // smbus_adapter = aura_motherboard_adapter_create(to_i2c_adapter(dev), NULL);
    // if (IS_ERR_OR_NULL(smbus_adapter)) {
    //     cb_data->error = CLEAR_ERR(smbus_adapter);
    //     return cb_data->error;
    // }

    found = aura_motherboard_probe_adapter(to_i2c_adapter(dev));

    if (found > 0) {
        cb_data->count += found;
        return cb_data->count;
    } else {
        // aura_motherboard_adapter_destroy(smbus_adapter);
        if (found < 0)
            cb_data->error = found;
        return found;
    }
}

/**
 * aura_motherboard_release() - Releases everything created by this module
 */
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
        aura_motherboard_adapter_destroy(smbus);
    }
}

/**
 * aura_motherboard_probe() - Probes the motherboard for AURA controllers
 *
 * @state: Pre-configured state to apply to any found controllers
 *
 * @return: Zero or a negative error number
 */
error_t aura_motherboard_probe (
    const struct lights_state *state
){
    struct i2c_adapter *i2c_adapter;
    struct aura_smbus_adapter *smbus_adapter;
    struct aura_motherboard_ctrl *ctrl;
    struct callback_data data = {0};
    int i, found;
    error_t err;

    // TODO: This should not return an error for not found
    i2c_for_each_dev(&data, aura_motherboard_probe_device);
    err = data.error;

    for (i = 0; i < ARRAY_SIZE(smbus_factory) && !err; i++) {
        AURA_DBG("Attempting to create I2C adapter '%s'", smbus_factory[i].name);

        i2c_adapter = smbus_factory[i].create();
        if (!IS_ERR_OR_NULL(i2c_adapter)) {
            found = aura_motherboard_probe_adapter(i2c_adapter);
            if (found > 0) {
                smbus_adapter = aura_motherboard_adapter_create(i2c_adapter, smbus_factory[i].destroy);
                if (IS_ERR_OR_NULL(smbus_adapter)) {
                    smbus_factory[i].destroy(i2c_adapter);
                    err = CLEAR_ERR(smbus_adapter);
                    break;
                }
            } else {
                smbus_factory[i].destroy(i2c_adapter);
                err = found;
                break;
            }
        } else {
            AURA_DBG("Failed to create I2C adapter '%s'", smbus_factory[i].name);
        }
    }

    if (err) {
        aura_motherboard_release();
    } else {
        list_for_each_entry(ctrl, &aura_motherboard_ctrl_list, siblings)
            aura_motherboard_update(ctrl, state);
    }

    return 0;
}
