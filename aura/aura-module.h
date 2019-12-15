/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UAPI_AURA_H
#define _UAPI_AURA_H

typedef error_t (*probe_func_t)(struct lights_state const *state);
typedef void (*release_func_t)(void);
/*
	Probe and release functions for each interface
 */
error_t aura_motherboard_probe(struct lights_state const *state);
void aura_motherboard_release(void);

error_t aura_header_probe(struct lights_state const *state);
void aura_header_release(void);

error_t aura_memory_probe(struct lights_state const *state);
void aura_memory_release(void);

error_t aura_gpu_probe(struct lights_state const *state);
void aura_gpu_release(void);

#endif
