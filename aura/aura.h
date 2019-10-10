/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UAPI_AURA_H
#define _UAPI_AURA_H

#define CONFIG_PCI_IOV 1

#include <linux/acpi.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/dmi.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "../adapter/lights-adapter.h"

typedef s32 error_t;

extern struct device *aura_dev;
extern struct lights_mode aura_available_modes[];

extern int lights_device_register(struct lights_dev * dev);

#define AURA_DBG(fmt, ...) dev_dbg(aura_dev, fmt, ##__VA_ARGS__)
#define AURA_INFO(fmt, ...) dev_info(aura_dev, fmt, ##__VA_ARGS__)
#define AURA_ERR(fmt, ...) dev_err(aura_dev, fmt, ##__VA_ARGS__)
#define AURA_NOTE(fmt, ...) dev_note(aura_dev, fmt, ##__VA_ARGS__)
#define AURA_WARN(fmt, ...) dev_warn(aura_dev, fmt, ##__VA_ARGS__)

/*
	Probe and release functions for each interface
 */
error_t aura_motherboard_probe(const struct lights_state *state);
void aura_motherboard_release(void);

error_t aura_header_probe(const struct lights_state *state);
void aura_header_release(void);

error_t aura_memory_probe(const struct lights_state *state);
void aura_memory_release(void);

error_t aura_gpu_probe(const struct lights_state *state);
void aura_gpu_release(void);

#endif
