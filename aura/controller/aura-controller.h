/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UAPI_AURA_CONTROLLER_H
#define _UAPI_AURA_CONTROLLER_H

#include <linux/types.h>
#include <linux/i2c.h>

#include <aura/debug.h>
#include <adapter/lights-interface.h>
#include <adapter/lights-adapter.h>

#define ZONE_ID_ALL 0xFF

/**
 * struct aura_controller - interface to the controller
 *
 * @name:       The name of the controller
 * @version:    The version number
 * @zone_count: The number of zones available
 */
struct aura_controller {
    const char              *name;
    uint8_t                 version;
    uint8_t                 zone_count;
};

/**
 * struct aura_zone = Interface to a single zone
 *
 * @name: The name based on the id
 * @ctrl: The owning controller
 * @id:   The chipset defined id of the zone
 */
struct aura_zone {
    const char              *name;
    struct aura_controller  *ctrl;
    uint8_t                 id;
};

/**
 * aura_controller_get_caps()
 *
 * @return: The array of capabilities
 */
const struct lights_mode *aura_controller_get_caps (
    void
);

/**
 * aura_controller_create() - Tests for and creates an interface
 *
 * @client: The lights managed i2c_adapter
 *
 * @return: The controller, NULL if not found or a negative error
 */
struct aura_controller *aura_controller_create (
    struct lights_adapter_client *client
);

/**
 * aura_controller_destroy() - Releases the controller
 *
 * @ctrl: Previously allocated with @aura_controller_create
 */
void aura_controller_destroy (
    struct aura_controller *ctrl
);

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
    struct aura_controller *ctrl,
    struct aura_controller *slaves[4],
    size_t count
);

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
);

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
);

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
);

/**
 * aura_controller_set_color() - Applies a color to all zones
 *
 * @ctrl:  Previously allocated with @aura_controller_create
 * @color: Color to apply
 *
 * @return: Zero or negative error number
 */
error_t aura_controller_set_color (
    struct aura_controller *ctrl,
    const struct lights_color *color
);

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
);

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
);

#endif
