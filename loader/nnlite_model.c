/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file nnlite_model.c
 * @brief Model-agnostic NNLite backend built on the Infineon ML Middleware.
 *
 * Compiled into the statically-linked loader because mtb_ml_init() needs
 * SYS_INIT ordering and irq_connect_dynamic(), which an LLEXT sketch cannot
 * provide. Nothing here is specific to a model: the flatbuffer comes from the
 * caller. Requires the ml-middleware/ml-tflite-micro west modules and the
 * CONFIG_ML_MIDDLEWARE_* options set in the board .conf.
 */

#include "nnlite_model.h"

#ifdef CONFIG_ML_MIDDLEWARE_NNLITE

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "mtb_ml.h"

/** NPU IRQ priority used when nnlite_model_open() has to bring the runtime up
 *  itself, i.e. when nnlite_runtime_init() was not called first. */
#define NNLITE_MODEL_DEFAULT_IRQ_PRIORITY (5U)

/** Models that can be open at the same time. */
#define NNLITE_MODEL_SLOT_COUNT (2U)

/** Alignment the TFLite flatbuffer must have for the middleware/NPU. */
#define NNLITE_MODEL_ALIGNMENT (16U)

struct nnlite_model_slot {
	bool in_use;
	mtb_ml_model_t *obj;
};

static struct nnlite_model_slot nnlite_slots[NNLITE_MODEL_SLOT_COUNT];
static bool nnlite_runtime_ready;

/** Handles are 1-based slot indices, so no loader-internal pointer is handed
 *  across the LLEXT boundary. */
static struct nnlite_model_slot *nnlite_slot_from_handle(nnlite_model_t model)
{
	uintptr_t index = (uintptr_t)model;

	if ((index == 0U) || (index > NNLITE_MODEL_SLOT_COUNT)) {
		return NULL;
	}

	if (!nnlite_slots[index - 1U].in_use) {
		return NULL;
	}

	return &nnlite_slots[index - 1U];
}

/** Element size of a tensor in bytes, falling back to one byte when the
 *  middleware does not report one. */
static uint32_t nnlite_element_size(int type_size)
{
	if (type_size <= 0) {
		return (uint32_t)sizeof(int8_t);
	}

	return (uint32_t)type_size;
}

static uint32_t nnlite_output_bytes(const mtb_ml_model_t *obj)
{
	if (obj->output_size <= 0) {
		return 0U;
	}

	return (uint32_t)obj->output_size * nnlite_element_size(obj->output_type_size);
}

uint32_t nnlite_api_version(void)
{
	return NNLITE_API_VERSION;
}

int nnlite_runtime_init(uint32_t irq_priority)
{
	if (nnlite_runtime_ready) {
		return 0;
	}

	/* Keep this path free of blocking diagnostics: PDM capture that may
	 * already be running is timing-sensitive. */
	if (mtb_ml_init(irq_priority) != MTB_ML_RESULT_SUCCESS) {
		return -EIO;
	}

	nnlite_runtime_ready = true;

	return 0;
}

int nnlite_model_open(const void *model_bin, uint32_t model_size, uint32_t arena_size,
		      nnlite_model_t *model)
{
	struct nnlite_model_slot *slot = NULL;
	uintptr_t index;
	int ret;

	if ((model_bin == NULL) || (model_size == 0U) || (arena_size == 0U) || (model == NULL)) {
		return -EINVAL;
	}

	if (((uintptr_t)model_bin % NNLITE_MODEL_ALIGNMENT) != 0U) {
		return -EFAULT;
	}

	ret = nnlite_runtime_init(NNLITE_MODEL_DEFAULT_IRQ_PRIORITY);
	if (ret != 0) {
		return ret;
	}

	for (index = 0U; index < NNLITE_MODEL_SLOT_COUNT; index++) {
		if (!nnlite_slots[index].in_use) {
			slot = &nnlite_slots[index];
			break;
		}
	}

	if (slot == NULL) {
		return -ENOSPC;
	}

	/* mtb_ml_model_init() copies the descriptor fields it needs, so a local
	 * instance is enough; only model_bin itself is kept by the runtime. */
	const mtb_ml_model_bin_t bin = {
		.name = "",
		.model_bin = (const uint8_t *)model_bin,
		.model_size = model_size,
		.arena_size = (int)arena_size,
	};

	/* buffer == NULL: let the middleware allocate its own persistent and
	 * scratch buffers sized from bin.arena_size. */
	if (mtb_ml_model_init(&bin, NULL, &slot->obj) != MTB_ML_RESULT_SUCCESS) {
		slot->obj = NULL;
		return -ENODEV;
	}

	slot->in_use = true;
	*model = (nnlite_model_t)(index + 1U);

	return 0;
}

int nnlite_model_get_info(nnlite_model_t model, struct nnlite_model_info *info)
{
	const struct nnlite_model_slot *slot = nnlite_slot_from_handle(model);

	if (slot == NULL) {
		return -ENODEV;
	}

	if ((info == NULL) || (info->struct_size != (uint32_t)sizeof(*info))) {
		return -EINVAL;
	}

	info->input_scale = slot->obj->input_scale;
	info->input_zero_point = (int32_t)slot->obj->input_zero_point;
	info->output_scale = slot->obj->output_scale;
	info->output_zero_point = (int32_t)slot->obj->output_zero_point;
	info->input_element_count = (uint32_t)slot->obj->input_size;
	info->output_element_count = (uint32_t)slot->obj->output_size;
	info->input_element_size = nnlite_element_size(slot->obj->input_type_size);
	info->output_element_size = nnlite_element_size(slot->obj->output_type_size);
	info->arena_used_bytes = (uint32_t)slot->obj->buffer_size;

	return 0;
}

int nnlite_model_run(nnlite_model_t model, const void *input, void *output, uint32_t output_bytes)
{
	struct nnlite_model_slot *slot = nnlite_slot_from_handle(model);

	if (slot == NULL) {
		return -ENODEV;
	}

	if (input == NULL) {
		return -EINVAL;
	}

	if ((output != NULL) && (output_bytes > nnlite_output_bytes(slot->obj))) {
		return -ENOBUFS;
	}

	/* Cast away const: the middleware copies the input into its own tensor
	 * but takes a non-const pointer. */
	if (mtb_ml_model_run(slot->obj, (MTB_ML_DATA_T *)(uintptr_t)input) !=
	    MTB_ML_RESULT_SUCCESS) {
		return -EIO;
	}

	if ((output != NULL) && (output_bytes != 0U)) {
		(void)memcpy(output, slot->obj->output, (size_t)output_bytes);
	}

	return 0;
}

int nnlite_model_close(nnlite_model_t model)
{
	struct nnlite_model_slot *slot = nnlite_slot_from_handle(model);

	if (slot == NULL) {
		return -ENODEV;
	}

	if (mtb_ml_model_deinit(slot->obj) != MTB_ML_RESULT_SUCCESS) {
		return -EIO;
	}

	slot->obj = NULL;
	slot->in_use = false;

	return 0;
}

#endif /* CONFIG_ML_MIDDLEWARE_NNLITE */
