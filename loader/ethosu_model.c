/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ethosu_model.c
 * @brief CM33 client of the CM55 Ethos-U55 inference server.
 *
 * Boots the CM55 on first use, copies models and inputs into the shared
 * SOCMEM window and posts commands through the ethosu_ipc.h mailbox. Calls
 * are synchronous and serialized, like the NNLite backend. Nothing here
 * depends on ML Middleware: the TFLM runtime lives in the CM55 image.
 */

#include "ethosu_model.h"

#ifdef CONFIG_ARDUINO_ETHOSU_REMOTE

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <cmsis_core.h>

#include "ethosu_ipc.h"
#include "ethosu_m55_boot.h"

/** NPU IRQ priority used when ethosu_runtime_init() was not called first. */
#define ETHOSU_MODEL_DEFAULT_IRQ_PRIORITY (5U)

struct ethosu_model_slot {
	bool in_use;
	struct ethosu_ipc_info info;
	struct ethosu_ipc_profile profile;
};

/* MISRA C:2012 Rule 11.4 deviation: fixed shared-memory address. */
static volatile struct ethosu_ipc_mailbox *const ethosu_mbox =
	(volatile struct ethosu_ipc_mailbox *)ETHOSU_IPC_SHM_BASE;

static struct ethosu_model_slot ethosu_slots[ETHOSU_IPC_SLOT_COUNT];
static bool ethosu_runtime_ready;
static bool ethosu_runtime_failed;
static uint32_t ethosu_req_seq;
static uint32_t ethosu_irq_priority = ETHOSU_MODEL_DEFAULT_IRQ_PRIORITY;

static K_MUTEX_DEFINE(ethosu_lock);

/** Address of an area inside a slot of the shared window. */
static uint8_t *ethosu_slot_area(uint32_t slot, uint32_t offset) {
	/* MISRA C:2012 Rule 11.4 deviation: fixed shared-memory address. */
	return (uint8_t *)(ETHOSU_IPC_SHM_BASE + ETHOSU_IPC_SLOT_OFFSET(slot) + offset);
}

/** Handles are 1-based slot indices, as in the NNLite backend. */
static struct ethosu_model_slot *ethosu_slot_from_handle(ethosu_model_t model,
							 uint32_t *slot_index) {
	uintptr_t index = (uintptr_t)model;

	if ((index == 0U) || (index > ETHOSU_IPC_SLOT_COUNT)) {
		return NULL;
	}

	if (!ethosu_slots[index - 1U].in_use) {
		return NULL;
	}

	if (slot_index != NULL) {
		*slot_index = (uint32_t)(index - 1U);
	}

	return &ethosu_slots[index - 1U];
}

static bool ethosu_deadline_passed(int64_t start) {
	return (k_uptime_get() - start) > (int64_t)CONFIG_ARDUINO_ETHOSU_TIMEOUT_MS;
}

static int ethosu_wait_server_ready(void) {
	int64_t start = k_uptime_get();

	for (;;) {
		if (ethosu_mbox->server.magic == ETHOSU_IPC_MAGIC) {
			uint32_t state = ethosu_mbox->server.state;

			if (state == ETHOSU_IPC_STATE_READY) {
				break;
			}

			if (state == ETHOSU_IPC_STATE_ERROR) {
				return -EIO;
			}
		}

		if (ethosu_deadline_passed(start)) {
			return -ETIMEDOUT;
		}

		k_yield();
	}

	if (ethosu_mbox->server.version != ETHOSU_IPC_VERSION) {
		return -EPROTO;
	}

	return 0;
}

/** Posts a command whose parameters are already in the host block and
 *  waits for its completion. A timeout leaves the protocol out of sync, so
 *  the runtime is marked failed. */
static int ethosu_command(uint32_t cmd, uint32_t slot) {
	int64_t start;

	ethosu_mbox->host.cmd = cmd;
	ethosu_mbox->host.slot = slot;
	ethosu_mbox->host.irq_priority = ethosu_irq_priority;

	ethosu_req_seq++;
	if (ethosu_req_seq == 0U) {
		ethosu_req_seq = 1U;
	}

	/* Payload and parameters must land before the sequence number. */
	__DSB();
	ethosu_mbox->host.req_seq = ethosu_req_seq;
	__DSB();

	start = k_uptime_get();
	while (ethosu_mbox->server.ack_seq != ethosu_req_seq) {
		if (ethosu_deadline_passed(start)) {
			ethosu_runtime_failed = true;
			return -ETIMEDOUT;
		}

		k_yield();
	}

	__DMB();

	return (int)ethosu_mbox->server.status;
}

static void ethosu_read_info(struct ethosu_ipc_info *info) {
	info->input_scale = ethosu_mbox->server.info.input_scale;
	info->input_zero_point = ethosu_mbox->server.info.input_zero_point;
	info->output_scale = ethosu_mbox->server.info.output_scale;
	info->output_zero_point = ethosu_mbox->server.info.output_zero_point;
	info->input_element_count = ethosu_mbox->server.info.input_element_count;
	info->output_element_count = ethosu_mbox->server.info.output_element_count;
	info->input_element_size = ethosu_mbox->server.info.input_element_size;
	info->output_element_size = ethosu_mbox->server.info.output_element_size;
	info->arena_used_bytes = ethosu_mbox->server.info.arena_used_bytes;
}

static void ethosu_read_profile(struct ethosu_ipc_profile *profile) {
	profile->npu_cycles = ethosu_mbox->server.profile.npu_cycles;
	profile->cpu_cycles = ethosu_mbox->server.profile.cpu_cycles;
	profile->completed_frames = ethosu_mbox->server.profile.completed_frames;
	profile->library_error = ethosu_mbox->server.profile.library_error;
}

/** Caller holds ethosu_lock. */
static int ethosu_runtime_start(void) {
	int ret;

	if (ethosu_runtime_failed) {
		return -EIO;
	}

	if (ethosu_runtime_ready) {
		return 0;
	}

	ret = ethosu_m55_boot();
	if (ret == 0) {
		ret = ethosu_wait_server_ready();
	}

	if (ret != 0) {
		ethosu_runtime_failed = true;
		return ret;
	}

	ethosu_runtime_ready = true;

	return 0;
}

uint32_t ethosu_api_version(void) {
	return ETHOSU_API_VERSION;
}

int ethosu_runtime_init(uint32_t irq_priority) {
	int ret;

	(void)k_mutex_lock(&ethosu_lock, K_FOREVER);
	if (!ethosu_runtime_ready) {
		ethosu_irq_priority = irq_priority;
	}
	ret = ethosu_runtime_start();
	(void)k_mutex_unlock(&ethosu_lock);

	return ret;
}

int ethosu_model_open(const void *model_bin, uint32_t model_size, uint32_t arena_size,
		      ethosu_model_t *model) {
	struct ethosu_model_slot *slot = NULL;
	uint32_t index = 0U;
	int ret;

	if ((model_bin == NULL) || (model_size == 0U) || (arena_size == 0U) || (model == NULL)) {
		return -EINVAL;
	}

	if (model_size > ETHOSU_IPC_MODEL_MAX_SIZE) {
		return -EFBIG;
	}

	(void)k_mutex_lock(&ethosu_lock, K_FOREVER);

	ret = ethosu_runtime_start();

	if (ret == 0) {
		for (index = 0U; index < ETHOSU_IPC_SLOT_COUNT; index++) {
			if (!ethosu_slots[index].in_use) {
				slot = &ethosu_slots[index];
				break;
			}
		}

		if (slot == NULL) {
			ret = -ENOSPC;
		}
	}

	if (ret == 0) {
		(void)memcpy(ethosu_slot_area(index, 0U), model_bin, (size_t)model_size);
		ethosu_mbox->host.model_size = model_size;
		ethosu_mbox->host.arena_size = arena_size;

		ret = ethosu_command(ETHOSU_IPC_CMD_OPEN, index);
	}

	if (ret == 0) {
		ethosu_read_info(&slot->info);
		(void)memset(&slot->profile, 0, sizeof(slot->profile));
		slot->in_use = true;
		*model = (ethosu_model_t)(uintptr_t)(index + 1U);
	}

	(void)k_mutex_unlock(&ethosu_lock);

	return ret;
}

int ethosu_model_get_info(ethosu_model_t model, struct ethosu_model_info *info) {
	const struct ethosu_model_slot *slot = ethosu_slot_from_handle(model, NULL);

	if (slot == NULL) {
		return -ENODEV;
	}

	if ((info == NULL) || (info->struct_size != (uint32_t)sizeof(*info))) {
		return -EINVAL;
	}

	info->input_scale = slot->info.input_scale;
	info->input_zero_point = slot->info.input_zero_point;
	info->output_scale = slot->info.output_scale;
	info->output_zero_point = slot->info.output_zero_point;
	info->input_element_count = slot->info.input_element_count;
	info->output_element_count = slot->info.output_element_count;
	info->input_element_size = slot->info.input_element_size;
	info->output_element_size = slot->info.output_element_size;
	info->arena_used_bytes = slot->info.arena_used_bytes;

	return 0;
}

int ethosu_model_get_profile(ethosu_model_t model, struct ethosu_model_profile *profile) {
	const struct ethosu_model_slot *slot = ethosu_slot_from_handle(model, NULL);

	if (slot == NULL) {
		return -ENODEV;
	}

	if ((profile == NULL) || (profile->struct_size != (uint32_t)sizeof(*profile))) {
		return -EINVAL;
	}

	profile->npu_cycles = slot->profile.npu_cycles;
	profile->cpu_cycles = slot->profile.cpu_cycles;
	profile->completed_frames = slot->profile.completed_frames;
	profile->library_error = slot->profile.library_error;

	return 0;
}

int ethosu_model_run(ethosu_model_t model, const void *input, void *output,
		     uint32_t output_bytes) {
	uint32_t index = 0U;
	struct ethosu_model_slot *slot = ethosu_slot_from_handle(model, &index);
	uint32_t input_bytes;
	uint32_t output_limit;
	int ret;

	if (slot == NULL) {
		return -ENODEV;
	}

	if (input == NULL) {
		return -EINVAL;
	}

	input_bytes = slot->info.input_element_count * slot->info.input_element_size;
	output_limit = slot->info.output_element_count * slot->info.output_element_size;

	if (output == NULL) {
		output_bytes = 0U;
	}

	if (output_bytes > output_limit) {
		return -ENOBUFS;
	}

	(void)k_mutex_lock(&ethosu_lock, K_FOREVER);

	if (ethosu_runtime_failed) {
		ret = -EIO;
	} else {
		(void)memcpy(ethosu_slot_area(index, ETHOSU_IPC_INPUT_OFFSET), input,
			     (size_t)input_bytes);
		ethosu_mbox->host.input_bytes = input_bytes;
		ethosu_mbox->host.output_bytes = output_bytes;

		ret = ethosu_command(ETHOSU_IPC_CMD_RUN, index);
	}

	if (ret == 0) {
		if (output_bytes != 0U) {
			(void)memcpy(output, ethosu_slot_area(index, ETHOSU_IPC_OUTPUT_OFFSET),
				     (size_t)output_bytes);
		}
		ethosu_read_profile(&slot->profile);
	}

	(void)k_mutex_unlock(&ethosu_lock);

	return ret;
}

int ethosu_model_close(ethosu_model_t model) {
	uint32_t index = 0U;
	struct ethosu_model_slot *slot = ethosu_slot_from_handle(model, &index);
	int ret;

	if (slot == NULL) {
		return -ENODEV;
	}

	(void)k_mutex_lock(&ethosu_lock, K_FOREVER);

	if (ethosu_runtime_failed) {
		ret = -EIO;
	} else {
		ret = ethosu_command(ETHOSU_IPC_CMD_CLOSE, index);
	}

	if (ret == 0) {
		slot->in_use = false;
	}

	(void)k_mutex_unlock(&ethosu_lock);

	return ret;
}

#endif /* CONFIG_ARDUINO_ETHOSU_REMOTE */
