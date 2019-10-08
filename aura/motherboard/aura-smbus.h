/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UAPI_AURA_SMBUS_H
#define _UAPI_AURA_SMBUS_H

#include "../aura.h"

struct aura_smbus_adapter {
    struct list_head    siblings;
    struct i2c_adapter  *adapter;

    void (*destroy)(struct aura_smbus_adapter*);
};

struct aura_smbus_factory {
    struct aura_smbus_adapter *(*create)(void);
};

struct aura_smbus_adapter *aura_smbus_piix4_adapter_create (void);
struct aura_smbus_adapter *aura_smbus_nuvoton_adapter_create (void);

#endif
