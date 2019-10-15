/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UAPI_AURA_CONTROLLER_H
#define _UAPI_AURA_CONTROLLER_H

#include "../aura.h"

#define ZONE_ID_ALL 0xFF

struct aura_controller {
    const char              *name;         /* The name of the controller */
    uint8_t                 version;       /* The version number */
    uint8_t                 zone_count;    /* The number of zones available */
};

struct aura_zone {
    struct aura_controller  *ctrl;         /* The owning controller */
    uint8_t                 id;            /* The chipset defined id of the zone */
    const char              *name;         /* The name based on the id */
};

const struct lights_mode *aura_controller_get_caps (void);

struct aura_controller *aura_controller_create (struct i2c_adapter *adap, uint8_t addr);
void aura_controller_destroy (struct aura_controller *ctrl);

struct aura_zone *aura_controller_get_zone (struct aura_controller *ctrl, uint8_t id);

// TODO - These should be private
error_t aura_controller_read_byte (struct aura_controller *ctrl, uint16_t reg, uint8_t *value);
error_t aura_controller_write_byte (struct aura_controller *ctrl, uint16_t reg, uint8_t value);
error_t aura_controller_read_word (struct aura_controller *ctrl, uint16_t reg, uint16_t *value);
error_t aura_controller_write_word (struct aura_controller *ctrl, uint16_t reg, uint8_t value);
error_t aura_controller_read_block (struct aura_controller *ctrl, uint16_t reg, uint8_t *data, uint8_t size);
error_t aura_controller_write_block (struct aura_controller *ctrl, uint16_t reg, uint8_t *data, uint8_t size);

error_t aura_controller_set_zone_color (struct aura_zone *zone, const struct lights_color *color);
error_t aura_controller_set_color (struct aura_controller *ctrl, const struct lights_color *color);
error_t aura_controller_set_mode (struct aura_controller *ctrl, const struct lights_mode *mode);

error_t aura_controller_get_zone_color (struct aura_zone *zone, struct lights_color *color);
error_t aura_controller_get_mode (struct aura_controller *ctrl, struct lights_mode *mode);

#endif
