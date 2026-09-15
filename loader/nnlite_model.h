/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file nnlite_model.h
 * @brief Model-agnostic NNLite (Infineon ML Middleware) inference service
 *        exported by the statically-linked loader to LLEXT sketches.
 *
 * The loader owns only what cannot be linked into an LLEXT: mtb_ml_init()'s
 * IRQ/DMA bring-up, the ML Middleware runtime and the model runtime objects.
 * The TFLite flatbuffer and all model-specific pre/post-processing stay in
 * the calling application, exactly as in the CPU-only TFLM examples.
 *
 * The flatbuffer passed to nnlite_model_open() is not copied: it must stay
 * valid and 16-byte aligned for as long as the model is open.
 */

#ifndef NNLITE_MODEL_H_
#define NNLITE_MODEL_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** ABI version of this API, returned by nnlite_api_version(). Bumped on any
 *  incompatible change to the functions or structures below. */
#define NNLITE_API_VERSION (1U)

/** Opaque handle to a model opened with nnlite_model_open(). */
typedef void *nnlite_model_t;

/** Model properties reported by nnlite_model_get_info(). */
struct nnlite_model_info {
	/** Must be set by the caller to sizeof(struct nnlite_model_info). */
	uint32_t struct_size;
	/** Quantization scale of the model's input tensor. */
	float input_scale;
	/** Quantization zero point of the model's input tensor. */
	int32_t input_zero_point;
	/** Quantization scale of the model's output tensor. */
	float output_scale;
	/** Quantization zero point of the model's output tensor. */
	int32_t output_zero_point;
	/** Number of elements expected in the input buffer. */
	uint32_t input_element_count;
	/** Number of elements produced in the output buffer. */
	uint32_t output_element_count;
	/** Size in bytes of one input element. */
	uint32_t input_element_size;
	/** Size in bytes of one output element. */
	uint32_t output_element_size;
	/** Tensor arena actually used, for tuning the arena_size argument. */
	uint32_t arena_used_bytes;
};

/**
 * @brief Returns NNLITE_API_VERSION of the running loader, so a sketch can
 *        detect a loader/sketch mismatch before calling anything else.
 */
uint32_t nnlite_api_version(void);

/**
 * @brief Brings up the ML Middleware and the NNLite NPU. Optional: the first
 *        nnlite_model_open() does it with a default priority. Call it
 *        explicitly only to choose the NPU IRQ priority.
 *
 * @param[in] irq_priority NPU interrupt priority passed to mtb_ml_init().
 *
 * @return 0 on success (including when already initialized), negative
 *         errno-style value on failure.
 */
int nnlite_runtime_init(uint32_t irq_priority);

/**
 * @brief Loads a TFLite flatbuffer and prepares it for inference on NNLite.
 *
 * @param[in]  model_bin  TFLite flatbuffer owned by the caller; must stay
 *                        valid and 16-byte aligned until nnlite_model_close().
 * @param[in]  model_size Size of @p model_bin in bytes.
 * @param[in]  arena_size Tensor arena to reserve, in bytes. The middleware
 *                        allocates it; nnlite_model_get_info() reports how
 *                        much of it the model actually uses.
 * @param[out] model      Handle to use with the other calls.
 *
 * @return 0 on success, negative errno-style value on failure.
 */
int nnlite_model_open(const void *model_bin, uint32_t model_size, uint32_t arena_size,
		      nnlite_model_t *model);

/**
 * @brief Reports the tensor shapes and quantization parameters of a model.
 *
 * @param[in]     model Handle returned by nnlite_model_open().
 * @param[in,out] info  Caller-allocated structure with struct_size set.
 *
 * @return 0 on success, negative errno-style value on failure.
 */
int nnlite_model_get_info(nnlite_model_t model, struct nnlite_model_info *info);

/**
 * @brief Runs one inference on the NNLite NPU.
 *
 * @param[in]  model        Handle returned by nnlite_model_open().
 * @param[in]  input        Input buffer of input_element_count elements.
 * @param[out] output       Output buffer, or NULL to skip the copy.
 * @param[in]  output_bytes Bytes to copy into @p output; must not exceed
 *                          output_element_count * output_element_size.
 *
 * @return 0 on success, negative errno-style value on failure.
 */
int nnlite_model_run(nnlite_model_t model, const void *input, void *output, uint32_t output_bytes);

/**
 * @brief Releases a model and its tensor arena. The NNLite runtime itself
 *        stays initialized, since tearing down its IRQ/DMA setup would
 *        disturb other peripherals.
 *
 * @param[in] model Handle returned by nnlite_model_open().
 *
 * @return 0 on success, negative errno-style value on failure.
 */
int nnlite_model_close(nnlite_model_t model);

#ifdef __cplusplus
}
#endif

#endif /* NNLITE_MODEL_H_ */
