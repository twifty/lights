// SPDX-License-Identifier: GPL-2.0
#include <linux/types.h>

#include <aura/debug.h>
#include <aura/controller/aura-controller.h>

enum spd_type {
    SPD_TYPE_DDR0 = 0x7,
    SPD_TYPE_DDR2 = 0x8,
    SPD_TYPE_DDR3 = 0xB,
    SPD_TYPE_DDR4 = 0xC
};

/**
 * struct aura_memory_spd - Data from a DIMM's SPD
 *
 * @size:   Size of the SPD
 * @addr:   Address of the DIMM on the bus
 * @type:   Type of DIMM (DDR2/3/4)
 * @slot:   Slot number
 * @aura:   Address of aura controller on the bus
 * @offset: Offset into available_addresses
 */
struct aura_memory_spd {
    u16     size;
    uint8_t addr;
    uint8_t type;
    uint8_t slot;
    uint8_t aura;
    uint8_t offset;
};

/**
 * struct aura_memory_controller - Data for a single DIMM
 *
 * @siblings:  Next and prev pointers
 * @spd:       SPD data
 * @aura_ctrl: Associated controller
 * @lights:    Userland access
 * @version:   Controller version
 * @led_count: Number of LEDs
 * @name:      Name as seen in /dev/lights/
 */
struct aura_memory_controller {
    struct list_head                siblings;
    struct aura_memory_spd          spd;
    struct aura_controller const    *aura;
    struct lights_dev               lights;
    struct lights_thunk             thunk;
    uint8_t                         version;
    uint8_t                         led_count;
    // char                            name[8]; // "dimm-n"
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

/**
 * aura_memory_rgb_triplet_exists() - Checks if a 3 byte triplet is known
 *
 * @rgb: 3 bytes
 *
 * @return: True or false
 */
static bool aura_memory_rgb_triplet_exists (
    uint8_t *rgb
){
    const uint8_t *p = aura_memory_rgb_triplets;

    for (p = aura_memory_rgb_triplets; *p; p += 3) {
        if (memcmp(p, rgb, 3) == 0)
            return true;
    }

    return false;
}

/**
 * aura_memory_next_aura_address() - Creates a bus address based on the SPD
 *
 * @node: Controller needing an address
 *
 * @return: Error code
 */
static error_t aura_memory_next_aura_address (
    struct aura_memory_spd *spd
){
    struct aura_memory_controller *ctrl;
    uint8_t offset;

    if (spd->aura) {
        offset = spd->offset + 1;
    } else if (!list_empty(&aura_memory_controller_list)) {
        ctrl = list_last_entry(&aura_memory_controller_list, typeof(*ctrl), siblings);
        offset = ctrl->spd.offset + 1;
    } else {
        offset = 0;
    }

    AURA_DBG("Using available address offset %d", offset);
    if (offset >= sizeof(aura_memory_available_addresses))
        return -EDQUOT;

    spd->offset = offset;
    spd->aura = aura_memory_available_addresses[offset];

    return 0;
}

/**
 * smbus_read_byte() - Reads a single byte from the bus
 *
 * @adapter: Bus adapter
 * @addr:    Address on the bus
 * @reg:     Offset into device on the bus
 * @value:   Buffer to read into
 *
 * @return: Error code
 */
static error_t smbus_read_byte (
    struct i2c_adapter *adapter,
    uint8_t addr,
    uint8_t reg,
    uint8_t *value
){
    error_t err;
    struct lights_adapter_msg msgs[] = {
        ADAPTER_READ_BYTE_DATA(reg)
    };

    err = lights_adapter_xfer(&LIGHTS_I2C_CLIENT(adapter, addr, 0), msgs, ARRAY_SIZE(msgs));
    if (!err)
        *value = msgs[0].data.byte;

    return err;
}

/**
 * smbus_write_byte() - Writes a single byte to the bus
 *
 * @adapter: Bus adapter
 * @addr:    Address on the bus
 * @reg:     Offset into device on the bus
 * @value:   Value to write
 *
 * @return: Error code
 */
static error_t smbus_write_byte (
    struct i2c_adapter *adapter,
    uint8_t addr,
    uint8_t reg,
    uint8_t value
){
    struct lights_adapter_msg msgs[] = {
        ADAPTER_WRITE_BYTE_DATA(reg, value)
    };

    return lights_adapter_xfer(&LIGHTS_I2C_CLIENT(adapter, addr, 0), msgs, ARRAY_SIZE(msgs));
}

/**
 * aura_controller_load() - Creates a bus address and loads the controller
 *
 * @i2c_adapter: The bus containing the DIMMs
 * @spd:         SPD for one of the DIMMs
 * @name:        Interface name
 *
 * @return: NULL, controller or an error code
 *
 * NULL will only be returned when the manager is not available on the bus.
 * If the manager is not avaiable, it's likely that all DIMMs have already
 * been mapped.
 */
struct aura_controller const *aura_controller_load (
    struct i2c_adapter *i2c_adapter,
    struct aura_memory_spd *spd,
    const char *name
){
    struct lights_adapter_client manager = LIGHTS_SMBUS_CLIENT(i2c_adapter, 0x77, 0);
    struct lights_adapter_client slot = LIGHTS_SMBUS_CLIENT(i2c_adapter, 0, 0);
    struct aura_controller const *aura;
    error_t err;
    int i;

    // Attempt to write a word to 0x80F8
    err = lights_adapter_xfer(&manager, &ADAPTER_WRITE_WORD_DATA_SWAPPED(0x00, 0x80F8), 1);
    if (err) {
        AURA_DBG("Slot manager is not available. Are the slots already registered?");
        return NULL;
    }

    for (i = 0; i < sizeof(aura_memory_available_addresses); i++) {
        err = aura_memory_next_aura_address(spd);
        if (err) {
            AURA_ERR("Failed to allocated bus address: %s", ERR_NAME(err));
            goto error;
        }

        // Register the slot change
        err = lights_adapter_xfer(&manager, &ADAPTER_WRITE_BYTE_DATA(0x01, spd->slot), 1);
        if (err) {
            AURA_DBG("Failed to set slot number: %s", ERR_NAME(err));
            continue;
        }

        /* Test the address
         * This should fail since the new address has not yet been registered.
         * However, it is possible for the new address to alredy be in use by
         * another device or the bus was left in a state where only some of
         * the DIMMs were remapped.
         */
        LIGHTS_SMBUS_CLIENT_UPDATE(&slot, spd->aura);
        err = lights_adapter_xfer(&slot, &ADAPTER_WRITE_BYTE(0x01), 1);
        if (err == 0) {
            AURA_DBG("bus address 0x%02x is in use", spd->aura);

            // This could happen if only some of the DIMMs were mapped
            // AURA_DBG("Attempting to load aura controller");
            aura = aura_controller_create(&slot, name);
            if (IS_ERR_OR_NULL(aura)) {
                // AURA_DBG("attempt failed with %ld", PTR_ERR(aura));
                continue;
            }

            return aura;
        }

        break;
    }

    // Register the new address
    err = lights_adapter_xfer(&manager, &ADAPTER_WRITE_WORD_DATA_SWAPPED(0x00, 0x80F9), 1);
    if (err) {
        AURA_ERR("Failed to register new address: %s", ERR_NAME(err));
        goto error;
    }

    err = lights_adapter_xfer(&manager, &ADAPTER_WRITE_BYTE_DATA(0x01, spd->aura << 1), 1);
    if (err) {
        AURA_ERR("Failed to apply new address: %s", ERR_NAME(err));
        goto error;
    }

    LIGHTS_SMBUS_CLIENT_UPDATE(&slot, spd->aura);
    aura = aura_controller_create(&slot, name);
    if (IS_ERR(aura)) {
        err = PTR_ERR(aura);
        AURA_ERR("aura_controller_create() failed: %s", ERR_NAME(err));
        goto error;
    }

    /* A controller should exist */
    if (!aura) {
        AURA_ERR("Failed to detect AURA controller on remapped addr %02x", spd->aura);
        err = -ENODEV;
    }

error:
    return err ? ERR_PTR(err) : aura;
}

/**
 * aura_controller_probe() - Creates a controller from already mapped address.
 *
 * @i2c_adapter: The bus containing the DIMMs
 * @spd:         SPD for one of the DIMMs
 * @name:        Interface name
 *
 * @return: Controller or an error code
 */
struct aura_controller const *aura_controller_probe (
    struct i2c_adapter *i2c_adapter,
    struct aura_memory_spd *spd,
    const char *name
){
    struct lights_adapter_client slot = LIGHTS_SMBUS_CLIENT(i2c_adapter, 0, 0);
    struct aura_controller const *aura;
    error_t err;
    int i;

    // Let's try avoiding an infinite loop
    for (i = 0; i < sizeof(aura_memory_available_addresses); i++) {
        err = aura_memory_next_aura_address(spd);
        if (err) {
            AURA_ERR("Failed to allocated bus address: %s", ERR_NAME(err));
            goto error;
        }

        LIGHTS_SMBUS_CLIENT_UPDATE(&slot, spd->aura);
        aura = aura_controller_create(&slot, name);
        if (IS_ERR(aura)) {
            err = PTR_ERR(aura);
            AURA_ERR("aura_controller_create() failed: %s", ERR_NAME(err));
            goto error;
        }

        /*
         * No controller, no error..
         * the addr is probably used by another device
         */
        if (!aura)
            continue;

        // AURA_DBG("aura device found at 0x%02x", node->spd.aura);
        break;
    }

    if (!aura) {
        AURA_DBG("Failed to detect an AURA controller on any known address");
        err = -ENODEV;
    }

error:
    return err ? ERR_PTR(err) : aura;
}

/**
 * aura_memory_controller_create() - Converts SPDs into controllers
 *
 * @i2c_adapter: Bus adapter
 * @spd:         SPD of a single DIMM
 *
 * @return: Error code
 */
static error_t aura_memory_controller_create (
    struct i2c_adapter *i2c_adapter,
    struct aura_memory_spd const *spd
){
    struct aura_memory_controller *ctrl;
    char name[LIGHTS_MAX_FILENAME_LENGTH];
    error_t err;

    ctrl = kzalloc(sizeof(*ctrl), GFP_KERNEL);
    if (!ctrl)
        return -ENOMEM;

    snprintf(name, sizeof(name), "dimm-%d", spd->slot);

    ctrl->spd = *spd;
    ctrl->aura = aura_controller_load(i2c_adapter, &ctrl->spd, name);
    if (!ctrl->aura)
        ctrl->aura = aura_controller_probe(i2c_adapter, &ctrl->spd, name);

    if (IS_ERR(ctrl->aura)) {
        err = CLEAR_ERR(ctrl->aura);
        goto error;
    }

    // snprintf(ctrl->name, sizeof(ctrl->name), "dimm-%d", ctrl->spd.slot);

    err = aura_controller_register_ctrl(ctrl->aura, &ctrl->lights, NULL);
    if (err) {
        AURA_DBG("aura_controller_register_ctrl() failed: %s", ERR_NAME(err));
        aura_controller_destroy(ctrl->aura);
        goto error;
    }

    list_add_tail(&ctrl->siblings, &aura_memory_controller_list);

    return 0;

error:
    kfree(ctrl);

    return err;
}


/**
 * aura_memory_select_page() - Select page, based on ee1004 driver from Linux
 *
 * @i2c_adapter: i2c adapter
 * @page: page
 *
 * @return: Error code
 */
static error_t aura_memory_set_page(struct i2c_adapter *smbus, uint8_t page) {
    uint8_t data;
    error_t err;
    switch(page) {
        case 0:
            err = smbus_write_byte(smbus, 0x36 + page, 0x00, 0x00);
            if (err && err != -ENXIO)
                return err;
            break;
        case 1:
            err = smbus_write_byte(smbus, 0x36 + page, 0x00, 0x00);
            if (err && err != -ENXIO)
                return err;
            break;
        default:
            return -EFAULT;
    }
    err = smbus_read_byte(smbus, 0x36, 0x00, &data);
    if(err == -ENXIO && page == 1)
        return 0;
    else if(err < 0)
        return err;
    return 0;
}

/**
 * aura_memory_probe_adapter() - Reads all SPDs and creates a controller for each
 *
 * @dev:  Bus
 * @data: callback data
 *
 * @return: Error code
 */
static error_t aura_memory_probe_adapter (
    struct device *dev,
    void *found
){
    struct i2c_adapter *smbus = to_i2c_adapter(dev);
    struct aura_memory_spd spd[16] = {0};
    uint8_t addr, page, size, count, rgb[3], c, i;
    error_t err;

    // Return if not an adapter or already found
    if (dev->type != &i2c_adapter_type || *(int*)found)
        return 0;

    AURA_DBG("Probing '%s' for memory DIMMs", smbus->name);

    for (count = 0, addr = 0x50; addr <= 0x5F; addr++) {

        // Select page 0 on all DIMMs
        err = aura_memory_set_page(smbus, 0);
        if (err) {
            /* Page set error on this bus indicates no DIMMs */
            return 0;
        }

        // Read SPD type
        err = smbus_read_byte(smbus, addr, 0x02, &spd[count].type);
        if (err) {
            err = 0;
            // AURA_DBG("Failed to read SPD type");
            continue;
        }

        // Read SPD size
        err = smbus_read_byte(smbus, addr, 0x00, &size);
        if (err) {
            err = 0;
            // AURA_DBG("Failed to read SPD size");
            continue;
        }

        // AURA_DBG("Calculating size from type=0x%02x size=0x%02x", spd[count].type, size);
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

        AURA_DBG(
            "Detected DIMM slot=%d addr=0x%02x",
            spd[count].slot,
            spd[count].addr
        );

        count += 1;
    }

    if (count == 0)
        return 0;

    for (i = 0; i < count; i++) {
        // Select page according to size
        page = spd[i].size >= 0x100 ? 1 : 0;
        // AURA_DBG("Selecting page %d for all DIMMs", page);
        err = aura_memory_set_page(smbus, page);
        if (err)
            goto error;

        for (c = 0; c < 3; c++ ) {
            err = smbus_read_byte(smbus, spd[i].addr, 0xF0 + c, &rgb[c]);
            if (err)
                goto error;
        }

        // AURA_DBG("Found RGB triplet: 0x%02x 0x%02x 0x%02x", rgb[0], rgb[1], rgb[2]);

        // Return to page 1
        if (page == 1) {
            // AURA_DBG("Selecting page 0 for all DIMMs");
            err = aura_memory_set_page(smbus, 0);
        }

        // Check if they are known values
        if (aura_memory_rgb_triplet_exists(rgb)) {
            err = aura_memory_controller_create(smbus, &spd[i]);
            if (err) {
                AURA_DBG("aura_memory_controller_create() failed with code %d", err);
                goto error;
            }

            *(int*)found += 1;
        }
    }

error:
    return err;
}

/**
 * aura_memory_probe() - Entry point
 *
 * @state: Initial state to apply to found controllers
 *
 * @return: Error code
 */
error_t aura_memory_probe (
    struct lights_state const *state
){
    struct aura_memory_controller *ctrl;
    int found = 0;
    error_t err;

    err = i2c_for_each_dev(&found, aura_memory_probe_adapter);

    if (!err && found) {
        list_for_each_entry(ctrl, &aura_memory_controller_list, siblings) {
            err = aura_controller_update(ctrl->aura, &state->effect, &state->color);
            if (err)
                goto exit;
        }
    }

exit:
    return err;
}

/**
 * aura_memory_ctrl_destroy() - Destroys a single controller
 *
 * @ctrl: Controller to destroy
 */
static void aura_memory_ctrl_destroy (
    struct aura_memory_controller *ctrl
){
    if (ctrl->aura)
        aura_controller_destroy(ctrl->aura);

    lights_device_unregister(&ctrl->lights);
    kfree(ctrl);
}

/**
 * aura_memory_release() - Exit
 */
void aura_memory_release (
    void
){
    struct aura_memory_controller *ctrl, *safe;

    list_for_each_entry_safe(ctrl, safe, &aura_memory_controller_list, siblings) {
        list_del(&ctrl->siblings);
        aura_memory_ctrl_destroy(ctrl);
    }
}
