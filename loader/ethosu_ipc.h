/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ethosu_ipc.h
 * @brief Shared-memory mailbox between the CM33 loader and the CM55
 *        Ethos-U55 inference server.
 *
 * Included by both images, so it depends on nothing but <stdint.h>.
 * The window lives in SOCMEM, which is non-secure for both cores. The CM33
 * has no data cache; the CM55 does, so every cache line is owned by exactly
 * one side: the host block is only written by the CM33, the server block
 * only by the CM55. A request is posted by bumping host.req_seq last and
 * completed when server.ack_seq equals it.
 */

#ifndef ETHOSU_IPC_H_
#define ETHOSU_IPC_H_

#include <stdint.h>

/** Value of server.magic once the CM55 server is running. */
#define ETHOSU_IPC_MAGIC (0x55534F45UL)

/** Protocol version, bumped on any layout change. */
#define ETHOSU_IPC_VERSION (1UL)

/** CM55 data cache line size. */
#define ETHOSU_IPC_CACHE_LINE (32UL)

/** Non-secure SOCMEM window shared by both cores and the NPU. */
#define ETHOSU_IPC_SHM_BASE (0x26200000UL)
#define ETHOSU_IPC_SHM_SIZE (0x00200000UL)

/** Mailbox at the start of the window. */
#define ETHOSU_IPC_MAILBOX_SIZE (0x00001000UL)

/** Model slots; each holds a flatbuffer copy and its I/O buffers. */
#define ETHOSU_IPC_SLOT_COUNT       (2UL)
#define ETHOSU_IPC_SLOT_BASE_OFFSET (ETHOSU_IPC_MAILBOX_SIZE)
#define ETHOSU_IPC_SLOT_STRIDE      (0x000E0000UL)
#define ETHOSU_IPC_MODEL_MAX_SIZE   (0x000C0000UL)
#define ETHOSU_IPC_INPUT_OFFSET     (0x000C0000UL)
#define ETHOSU_IPC_INPUT_MAX_SIZE   (0x00010000UL)
#define ETHOSU_IPC_OUTPUT_OFFSET    (0x000D0000UL)
#define ETHOSU_IPC_OUTPUT_MAX_SIZE  (0x00010000UL)

/** Byte offset of a slot from ETHOSU_IPC_SHM_BASE. */
#define ETHOSU_IPC_SLOT_OFFSET(slot) \
	(ETHOSU_IPC_SLOT_BASE_OFFSET + ((uint32_t)(slot) * ETHOSU_IPC_SLOT_STRIDE))

/** Commands in host.cmd. */
#define ETHOSU_IPC_CMD_NONE  (0UL)
#define ETHOSU_IPC_CMD_OPEN  (1UL)
#define ETHOSU_IPC_CMD_RUN   (2UL)
#define ETHOSU_IPC_CMD_CLOSE (3UL)
#define ETHOSU_IPC_CMD_PING  (4UL)

/** Values of server.state. */
#define ETHOSU_IPC_STATE_OFF     (0UL)
#define ETHOSU_IPC_STATE_BOOTING (1UL)
#define ETHOSU_IPC_STATE_READY   (2UL)
#define ETHOSU_IPC_STATE_ERROR   (3UL)

/** Request block, written only by the CM33. One cache line. */
struct ethosu_ipc_host {
	/** Incremented by the CM33 after all other fields are written. */
	uint32_t req_seq;
	/** One of ETHOSU_IPC_CMD_*. */
	uint32_t cmd;
	/** Target slot index, 0-based. */
	uint32_t slot;
	/** OPEN: flatbuffer size in bytes. */
	uint32_t model_size;
	/** OPEN: tensor arena size in bytes. */
	uint32_t arena_size;
	/** RUN: input bytes placed in the slot input buffer. */
	uint32_t input_bytes;
	/** RUN: output bytes the CM33 will read back. */
	uint32_t output_bytes;
	/** NPU IRQ priority used when the server brings up the middleware. */
	uint32_t irq_priority;
};

/** Model properties reported by OPEN. */
struct ethosu_ipc_info {
	float input_scale;
	int32_t input_zero_point;
	float output_scale;
	int32_t output_zero_point;
	uint32_t input_element_count;
	uint32_t output_element_count;
	uint32_t input_element_size;
	uint32_t output_element_size;
	uint32_t arena_used_bytes;
};

/** Profiling values reported by RUN. */
struct ethosu_ipc_profile {
	uint64_t npu_cycles;
	uint64_t cpu_cycles;
	uint32_t completed_frames;
	int32_t library_error;
};

/** Response block, written only by the CM55. Three cache lines. */
struct ethosu_ipc_server {
	/** ETHOSU_IPC_MAGIC once the server has started. */
	uint32_t magic;
	/** ETHOSU_IPC_VERSION of the server image. */
	uint32_t version;
	/** One of ETHOSU_IPC_STATE_*. */
	uint32_t state;
	/** Result of the middleware bring-up, 0 or negative errno. */
	int32_t init_status;
	/** Incremented by the server loop while it is alive. */
	uint32_t heartbeat;
	/** CM55 core clock as seen by the server, 0 if unreadable. */
	uint32_t cpu_clk_hz;
	/** NPU clock as seen by the middleware, 0 if unreadable. */
	uint32_t npu_clk_hz;
	/** Copy of host.req_seq once the request is complete. */
	uint32_t ack_seq;
	/** Result of the completed request, 0 or negative errno. */
	int32_t status;
	struct ethosu_ipc_info info;
	struct ethosu_ipc_profile profile;
};

/** Mailbox at ETHOSU_IPC_SHM_BASE. */
struct ethosu_ipc_mailbox {
	struct ethosu_ipc_host host;
	struct ethosu_ipc_server server;
};

_Static_assert(sizeof(struct ethosu_ipc_host) == ETHOSU_IPC_CACHE_LINE,
	       "host block must fill exactly one cache line");
_Static_assert((sizeof(struct ethosu_ipc_server) % ETHOSU_IPC_CACHE_LINE) == 0U,
	       "server block must be a whole number of cache lines");
_Static_assert(sizeof(struct ethosu_ipc_mailbox) <= ETHOSU_IPC_MAILBOX_SIZE,
	       "mailbox does not fit");
_Static_assert((ETHOSU_IPC_SLOT_BASE_OFFSET + (ETHOSU_IPC_SLOT_COUNT * ETHOSU_IPC_SLOT_STRIDE)) <=
		       ETHOSU_IPC_SHM_SIZE,
	       "slots do not fit in the shared window");
_Static_assert((ETHOSU_IPC_OUTPUT_OFFSET + ETHOSU_IPC_OUTPUT_MAX_SIZE) <= ETHOSU_IPC_SLOT_STRIDE,
	       "slot layout overflows its stride");

#endif /* ETHOSU_IPC_H_ */
