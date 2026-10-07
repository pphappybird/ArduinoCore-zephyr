/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file main.c
 * @brief Ethos-U55 inference server running on the CM55 core.
 *
 * Started by the CM33 loader, which powers the U55, opens the peripheral
 * and memory protection for this core and then releases it from reset.
 * The server polls the shared mailbox described in ethosu_ipc.h and runs
 * each request through the Infineon ML Middleware, so Vela-compiled
 * operators execute on the NPU and the rest on this core.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <cmsis_core.h>

#include "cy_pdl.h"
#include "mtb_ml.h"

#include "ethosu_ipc.h"

/** Mailbox at the start of the shared window.
 *  MISRA C:2012 Rule 11.4 deviation: fixed shared-memory address. */
static volatile struct ethosu_ipc_mailbox *const server_mbox =
	(volatile struct ethosu_ipc_mailbox *)ETHOSU_IPC_SHM_BASE;

/** Middleware handles per slot, NULL while the slot is closed. */
static mtb_ml_model_t *server_models[ETHOSU_IPC_SLOT_COUNT];

/** Set once mtb_ml_init() has succeeded. */
static bool server_ml_ready;

/** Last request sequence number that was served. */
static uint32_t server_last_seq;

/**
 * @brief No-op replacement for the PDL U55 power helper.
 *
 * The CM33 loader already powered the U55 and set its power dependency;
 * the SRSS registers the original touches are secure-only for this core.
 * Wired in with -Wl,--wrap=Cy_SysU55Enable.
 */
void __wrap_Cy_SysU55Enable(bool enable);
void __wrap_Cy_SysU55Enable(bool enable)
{
	(void)enable;
}

/**
 * @brief newlib-style errno accessor needed by the precompiled TFLM library.
 *
 * MISRA C:2012 Rule 21.2 deviation: reserved name required by the library ABI.
 */
int *__errno(void);
int *__errno(void)
{
	return &errno;
}

/** @brief Timestamp source for ML Middleware model profiling. */
int mtb_ml_model_profile_get_tsc(uint64_t *value)
{
	if (value == NULL) {
		return -EINVAL;
	}

	*value = k_cycle_get_64();
	return 0;
}

/** @brief Rounds a shared-memory range out to whole cache lines. */
static void server_cache_range(uintptr_t addr, size_t size, uintptr_t *start, int32_t *len)
{
	uintptr_t first = addr & ~((uintptr_t)ETHOSU_IPC_CACHE_LINE - 1U);
	uintptr_t last = (addr + size + (ETHOSU_IPC_CACHE_LINE - 1U)) &
			 ~((uintptr_t)ETHOSU_IPC_CACHE_LINE - 1U);

	*start = first;
	*len = (int32_t)(last - first);
}

/** @brief Drops cached copies so data written by the CM33 is observed. */
static void server_invalidate(uintptr_t addr, size_t size)
{
	uintptr_t start;
	int32_t len;

	if (size == 0U) {
		return;
	}

	server_cache_range(addr, size, &start, &len);
	/* MISRA C:2012 Rule 11.6 deviation: CMSIS takes an address as a pointer. */
	SCB_InvalidateDCache_by_Addr((volatile void *)start, len);
}

/** @brief Pushes data written by this core out to SOCMEM. */
static void server_clean(uintptr_t addr, size_t size)
{
	uintptr_t start;
	int32_t len;

	if (size == 0U) {
		return;
	}

	server_cache_range(addr, size, &start, &len);
	/* MISRA C:2012 Rule 11.6 deviation: CMSIS takes an address as a pointer. */
	SCB_CleanDCache_by_Addr((volatile void *)start, len);
}

/** @brief Publishes the server block. */
static void server_publish(void)
{
	server_clean((uintptr_t)&server_mbox->server, sizeof(server_mbox->server));
}

/** @brief Address of a region inside a slot. */
static uintptr_t server_slot_area(uint32_t slot, uint32_t offset)
{
	return (uintptr_t)ETHOSU_IPC_SHM_BASE + ETHOSU_IPC_SLOT_OFFSET(slot) + offset;
}

/** @brief Element size in bytes, one byte when the middleware reports none. */
static uint32_t server_element_size(int type_size)
{
	if (type_size <= 0) {
		return (uint32_t)sizeof(int8_t);
	}

	return (uint32_t)type_size;
}

/** @brief Tensor size in bytes. */
static uint32_t server_tensor_bytes(int count, int type_size)
{
	if (count <= 0) {
		return 0U;
	}

	return (uint32_t)count * server_element_size(type_size);
}

/** @brief Brings the middleware and the NPU driver up on first use. */
static int server_ml_start(uint32_t irq_priority)
{
	if (server_ml_ready) {
		return 0;
	}

	if (mtb_ml_init(irq_priority) != MTB_ML_RESULT_SUCCESS) {
		server_mbox->server.init_status = -ENODEV;
		return -ENODEV;
	}

	server_mbox->server.npu_clk_hz = mtb_ml_npu_clk_freq;
	server_mbox->server.init_status = 0;
	server_ml_ready = true;

	return 0;
}

/** @brief OPEN: builds an interpreter for the flatbuffer in the slot. */
static int server_open(uint32_t slot, uint32_t model_size, uint32_t arena_size,
		       uint32_t irq_priority)
{
	mtb_ml_model_t *obj = NULL;
	uint32_t in_bytes;
	uint32_t out_bytes;
	int ret;

	if (server_models[slot] != NULL) {
		return -EBUSY;
	}

	if ((model_size == 0U) || (model_size > ETHOSU_IPC_MODEL_MAX_SIZE) ||
	    (arena_size == 0U) || (arena_size > (uint32_t)INT32_MAX)) {
		return -EINVAL;
	}

	ret = server_ml_start(irq_priority);
	if (ret != 0) {
		return ret;
	}

	server_invalidate(server_slot_area(slot, 0U), model_size);

	/** The flatbuffer stays in the slot for the lifetime of the model. */
	const mtb_ml_model_bin_t bin = {
		.name = "",
		/* MISRA C:2012 Rule 11.4 deviation: slot address to buffer pointer. */
		.model_bin = (const uint8_t *)server_slot_area(slot, 0U),
		.model_size = (int)model_size,
		.arena_size = (int)arena_size,
	};

	if (mtb_ml_model_init(&bin, NULL, &obj) != MTB_ML_RESULT_SUCCESS) {
		return -ENODEV;
	}

	in_bytes = server_tensor_bytes(obj->input_size, obj->input_type_size);
	out_bytes = server_tensor_bytes(obj->output_size, obj->output_type_size);

	if ((in_bytes > ETHOSU_IPC_INPUT_MAX_SIZE) || (out_bytes > ETHOSU_IPC_OUTPUT_MAX_SIZE)) {
		(void)mtb_ml_model_deinit(obj);
		return -ENOBUFS;
	}

	if (mtb_ml_model_profile_config(obj, MTB_ML_PROFILE_ENABLE_MODEL) !=
	    MTB_ML_RESULT_SUCCESS) {
		(void)mtb_ml_model_deinit(obj);
		return -EIO;
	}

	server_mbox->server.info.input_scale = obj->input_scale;
	server_mbox->server.info.input_zero_point = (int32_t)obj->input_zero_point;
	server_mbox->server.info.output_scale = obj->output_scale;
	server_mbox->server.info.output_zero_point = (int32_t)obj->output_zero_point;
	server_mbox->server.info.input_element_count = (uint32_t)obj->input_size;
	server_mbox->server.info.output_element_count = (uint32_t)obj->output_size;
	server_mbox->server.info.input_element_size = server_element_size(obj->input_type_size);
	server_mbox->server.info.output_element_size = server_element_size(obj->output_type_size);
	server_mbox->server.info.arena_used_bytes = (uint32_t)obj->buffer_size;

	server_models[slot] = obj;

	return 0;
}

/** @brief RUN: one inference on the slot input buffer. */
static int server_run(uint32_t slot, uint32_t input_bytes, uint32_t output_bytes)
{
	mtb_ml_model_t *obj = server_models[slot];
	uintptr_t in_addr = server_slot_area(slot, ETHOSU_IPC_INPUT_OFFSET);
	uintptr_t out_addr = server_slot_area(slot, ETHOSU_IPC_OUTPUT_OFFSET);
	cy_rslt_t result;

	if (obj == NULL) {
		return -ENODEV;
	}

	if ((input_bytes != server_tensor_bytes(obj->input_size, obj->input_type_size)) ||
	    (output_bytes > server_tensor_bytes(obj->output_size, obj->output_type_size))) {
		return -ENOBUFS;
	}

	server_invalidate(in_addr, input_bytes);

	/* The middleware copies the input into its own tensor.
	 * MISRA C:2012 Rule 11.4 deviation: slot address to buffer pointer. */
	result = mtb_ml_model_inputs(obj, (MTB_ML_DATA_T *)in_addr, 0);

	if (result == MTB_ML_RESULT_SUCCESS) {
		result = mtb_ml_model_invoke(obj);
	}

	server_mbox->server.profile.npu_cycles = obj->m_npu_cycles;
	server_mbox->server.profile.cpu_cycles = obj->m_cpu_cycles;
	server_mbox->server.profile.completed_frames = obj->m_sum_frames;
	server_mbox->server.profile.library_error = (int32_t)obj->lib_error;

	if (result != MTB_ML_RESULT_SUCCESS) {
		return -EIO;
	}

	if (output_bytes != 0U) {
		/* MISRA C:2012 Rule 11.4 deviation: slot address to buffer pointer. */
		(void)memcpy((void *)out_addr, obj->output, (size_t)output_bytes);
		server_clean(out_addr, output_bytes);
	}

	return 0;
}

/** @brief CLOSE: releases the interpreter of a slot. */
static int server_close(uint32_t slot)
{
	if (server_models[slot] == NULL) {
		return -ENODEV;
	}

	(void)mtb_ml_model_deinit(server_models[slot]);
	server_models[slot] = NULL;

	return 0;
}

/** @brief Dispatches one request from the host block. */
static int server_dispatch(void)
{
	uint32_t cmd = server_mbox->host.cmd;
	uint32_t slot = server_mbox->host.slot;
	int ret;

	if (cmd == ETHOSU_IPC_CMD_PING) {
		return 0;
	}

	if (slot >= ETHOSU_IPC_SLOT_COUNT) {
		return -EINVAL;
	}

	switch (cmd) {
	case ETHOSU_IPC_CMD_OPEN:
		ret = server_open(slot, server_mbox->host.model_size, server_mbox->host.arena_size,
				  server_mbox->host.irq_priority);
		break;
	case ETHOSU_IPC_CMD_RUN:
		ret = server_run(slot, server_mbox->host.input_bytes,
				 server_mbox->host.output_bytes);
		break;
	case ETHOSU_IPC_CMD_CLOSE:
		ret = server_close(slot);
		break;
	default:
		ret = -ENOTSUP;
		break;
	}

	return ret;
}

int main(void)
{
	int64_t last_beat = 0;

	server_mbox->server.magic = ETHOSU_IPC_MAGIC;
	server_mbox->server.version = ETHOSU_IPC_VERSION;
	server_mbox->server.state = ETHOSU_IPC_STATE_BOOTING;
	server_publish();

	SystemCoreClockUpdate();
	server_mbox->server.cpu_clk_hz = SystemCoreClock;

	server_invalidate((uintptr_t)&server_mbox->host, sizeof(server_mbox->host));
	server_last_seq = server_mbox->host.req_seq;
	server_mbox->server.ack_seq = server_last_seq;
	server_mbox->server.state = ETHOSU_IPC_STATE_READY;
	server_publish();

	for (;;) {
		uint32_t seq;
		int64_t now;

		server_invalidate((uintptr_t)&server_mbox->host, sizeof(server_mbox->host));
		seq = server_mbox->host.req_seq;

		if (seq != server_last_seq) {
			/* Parameters were written before req_seq. */
			__DMB();
			server_mbox->server.status = server_dispatch();
			server_publish();
			__DSB();

			/** ack_seq goes out last so the CM33 never sees a partial reply. */
			server_last_seq = seq;
			server_mbox->server.ack_seq = seq;
			server_publish();
			__DSB();
			continue;
		}

		now = k_uptime_get();
		if (now != last_beat) {
			last_beat = now;
			server_mbox->server.heartbeat++;
			server_publish();
		}
	}

	return 0;
}
