/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UAPI_AURA_GPU_I2C_H
#define _UAPI_AURA_GPU_I2C_H

#include "../aura.h"
// #include "aura-gpu.h"
#include "aura-gpu-reg.h"
#include "asic/asic-types.h"

struct aura_i2c_service {
    // void *private;
    struct i2c_adapter *adapter;
};

/* declared in aura-gpu.h */
// enum aura_asic_type;

struct aura_i2c_service *aura_gpu_i2c_create (
    struct pci_dev *pci_dev,
    enum aura_asic_type asic_type
);

void aura_gpu_i2c_destroy (
    struct aura_i2c_service *service
);

#endif
