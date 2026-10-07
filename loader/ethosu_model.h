/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ethosu_model.h
 * @brief Model-agnostic Ethos-U55 inference service exported by the
 *        statically-linked loader to LLEXT sketches.
 *
 * The Ethos-U55 is only reachable from the CM55, so the loader boots a CM55
 * inference server and forwards each call to it through shared memory.
 * The API mirrors nnlite_model.h one to one; the structures are
 * binary-identical, so a sketch written for NNLite can run unchanged when
 * CONFIG_ARDUINO_ETHOSU_NNLITE_ABI exports these under the nnlite_* names.
 *
 * The flatbuffer is copied into the shared window by ethosu_model_open(),
 * so the caller's buffer needs no particular alignment or lifetime.
 */

#ifndef ETHOSU_MODEL_H_
#define ETHOSU_MODEL_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** ABI version of this API, returned by ethosu_api_version(). Kept equal to
 *  NNLITE_API_VERSION while the two APIs stay binary-compatible. */
#define ETHOSU_API_VERSION (1U)

/** Opaque handle to a model opened with ethosu_model_open(). */
typedef void *ethosu_model_t;

/** Model properties reported by ethosu_model_get_info(). */
struct ethosu_model_info {
	/** Must be set by the caller to sizeof(struct ethosu_model_info). */
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

/** Profiling values returned by ethosu_model_get_profile(). */
struct ethosu_model_profile {
	/** Must be set by the caller to sizeof(struct ethosu_model_profile). */
	uint32_t struct_size;
	/** Ethos-U55 cycles for the most recent successful run. */
	uint64_t npu_cycles;
	/** CM55 cycles measured by the middleware for the most recent run. */
	uint64_t cpu_cycles;
	/** Number of successfully profiled model invocations. */
	uint32_t completed_frames;
	/** Last middleware library error code. */
	int32_t library_error;
};

/**
 * @brief Returns ETHOSU_API_VERSION of the running loader.
 */
uint32_t ethosu_api_version(void);

/**
 * @brief Boots the CM55 inference server and waits until it is ready.
 *        Optional: the first ethosu_model_open() does it with a default
 *        priority.
 *
 * @param[in] irq_priority NPU interrupt priority used on the CM55.
 *
 * @return 0 on success (including when already running), negative
 *         errno-style value on failure.
 */
int ethosu_runtime_init(uint32_t irq_priority);

/**
 * @brief Copies a TFLite flatbuffer to the CM55 and prepares it. Operators
 *        compiled by Vela run on the Ethos-U55, the rest on the CM55.
 *
 * @param[in]  model_bin  TFLite flatbuffer; copied, may be freed afterwards.
 * @param[in]  model_size Size of @p model_bin in bytes.
 * @param[in]  arena_size Tensor arena to reserve on the CM55, in bytes.
 * @param[out] model      Handle to use with the other calls.
 *
 * @return 0 on success, negative errno-style value on failure.
 */
int ethosu_model_open(const void *model_bin, uint32_t model_size, uint32_t arena_size,
		      ethosu_model_t *model);

/**
 * @brief Reports the tensor shapes and quantization parameters of a model.
 *
 * @param[in]     model Handle returned by ethosu_model_open().
 * @param[in,out] info  Caller-allocated structure with struct_size set.
 *
 * @return 0 on success, negative errno-style value on failure.
 */
int ethosu_model_get_info(ethosu_model_t model, struct ethosu_model_info *info);

/**
 * @brief Reads the profiling values of the most recent run.
 *
 * @return 0 on success, negative errno-style value on failure.
 */
int ethosu_model_get_profile(ethosu_model_t model, struct ethosu_model_profile *profile);

/**
 * @brief Runs one inference on the CM55/Ethos-U55 and waits for it.
 *
 * @param[in]  model        Handle returned by ethosu_model_open().
 * @param[in]  input        Input buffer of input_element_count elements.
 * @param[out] output       Output buffer, or NULL to skip the copy.
 * @param[in]  output_bytes Bytes to copy into @p output; must not exceed
 *                          output_element_count * output_element_size.
 *
 * @return 0 on success, negative errno-style value on failure.
 */
int ethosu_model_run(ethosu_model_t model, const void *input, void *output,
		     uint32_t output_bytes);

/**
 * @brief Releases a model on the CM55. The server keeps running.
 *
 * @param[in] model Handle returned by ethosu_model_open().
 *
 * @return 0 on success, negative errno-style value on failure.
 */
int ethosu_model_close(ethosu_model_t model);

#ifdef __cplusplus
}
#endif

#endif /* ETHOSU_MODEL_H_ */
