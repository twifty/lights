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
struct lights_mode const *aura_controller_get_caps (
    void
);

/**
 * aura_controller_create() - Tests for and creates an interface
 *
 * @client: The lights managed i2c_adapter
 * @name:   The interface name
 *
 * @return: The controller, NULL if not found or a negative error
 */
struct aura_controller const *aura_controller_create (
    struct lights_adapter_client *client,
    const char *name
);

/**
 * aura_controller_destroy() - Releases the controller
 *
 * @ctrl: Previously allocated with aura_controller_create()
 */
void aura_controller_destroy (
    struct aura_controller const *ctrl
);

/**
 * aura_controller_create_slaves() - Creates slave controllers
 *
 * @ctrl:   Previously allocated with aura_controller_create()
 * @slaves: Array to hold found slave controllers
 * @count:  Length of @slaves array (MUST be 4)
 *
 * @return: The number of found slaves
 */
int aura_controller_create_slaves (
    struct aura_controller const *ctrl,
    struct aura_controller const ** slaves,
    size_t count
);

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
);

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
    struct aura_zone const *zone,
    struct lights_dev *lights,
    const char *name
);

/**
 * aura_controller_get_zone() - Fetches a zone by its index
 *
 * @ctrl:  Previously allocated with aura_controller_create()
 * @index: Zero based index of the zone
 *
 * @return: The zone or a negative error number
 */
struct aura_zone const *aura_controller_get_zone (
    struct aura_controller const *ctrl,
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
    struct aura_zone const *zone,
    struct lights_color const *color
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
    struct aura_zone const *zone,
    struct lights_color *color
);

/**
 * aura_controller_set_colors() - Applies a color to all zones
 *
 * @ctrl:  Previously allocated with aura_controller_create()
 * @color: Color to apply
 * @count: Number of colors
 *
 * @return: Zero or negative error number
 *
 * If @count is 1, the same color is applied to all zones, otherwise
 * @count must be equal to the zone count.
 */
error_t aura_controller_set_colors (
    struct aura_controller const *ctrl,
    struct lights_color const * const colors,
    uint8_t count
);

/**
 * aura_controller_set_color() - Applies a color to all zones
 *
 * @ctrl:  Previously allocated with aura_controller_create()
 * @color: Color to apply
 *
 * @return: Zero or negative error number
 */
static inline error_t aura_controller_set_color (
    struct aura_controller const *ctrl,
    struct lights_color const *color
){
    return aura_controller_set_colors(ctrl, color, 1);
};

/**
 * aura_controller_set_mode() - Applies a mode to all zones
 *
 * @ctrl:  Previously allocated with aura_controller_create()
 * @color: Mode to apply
 *
 * @return: Zero or negative error number
 *
 * NOTE: A single zone cannot have its own mode.
 */
error_t aura_controller_set_mode (
    struct aura_controller const *ctrl,
    struct lights_mode const *mode
);

/**
 * aura_controller_get_mode() - Reads the mode of all zones
 *
 * @ctrl: Previously allocated with aura_controller_create()
 * @mode: Buffer to read into
 *
 * @return: Zero or negative error number
 */
error_t aura_controller_get_mode (
    struct aura_controller const *ctrl,
    struct lights_mode *mode
);

/**
 * aura_controller_update() - Writes a mode and color to all zones
 *
 * @ctrl:  Previously allocated with aura_controller_create()
 * @mode:  New mode to apply
 * @color: New color to apply
 *
 * @return: Error code
 */
error_t aura_controller_update (
    struct aura_controller const *ctrl,
    struct lights_mode const *mode,
    struct lights_color const *color
);

#endif
