/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file nnlite_hello_world.h
 * @brief Loader-side API that runs the TFLM hello_world sine model on the
 *        NNLite NPU via Infineon ML Middleware, exposed to the LLEXT sketch
 *        as a drop-in replacement for tflite::MicroInterpreter::Invoke().
 *
 * Must live in the statically-linked loader image, not the LLEXT sketch,
 * since mtb_ml_init() needs SYS_INIT ordering and irq_connect_dynamic().
 * Quantization/dequantization stays with the caller (the sketch).
 */

#ifndef NNLITE_HELLO_WORLD_H_
#define NNLITE_HELLO_WORLD_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief One-time initialization: brings up the ML Middleware/NNLite NPU and
 *        loads the hello_world sine model. Safe to call more than once; only
 *        the first call performs the actual initialization.
 *
 * @param[out] input_scale       Quantization scale of the model's input tensor.
 * @param[out] input_zero_point  Quantization zero point of the model's input tensor.
 * @param[out] output_scale      Quantization scale of the model's output tensor.
 * @param[out] output_zero_point Quantization zero point of the model's output tensor.
 *
 * @return 0 on success, negative errno-style value on failure.
 */
int nnlite_hello_world_init(float *input_scale, int32_t *input_zero_point,
			     float *output_scale, int32_t *output_zero_point);

/**
 * @brief Runs one inference of the hello_world sine model on NNLite.
 *
 * @param[in]  x_quantized Quantized input value (already scaled by the caller).
 * @param[out] y_quantized Quantized output value.
 *
 * @return 0 on success, negative errno-style value on failure.
 */
int nnlite_hello_world_infer(int8_t x_quantized, int8_t *y_quantized);

#ifdef __cplusplus
}
#endif

#endif /* NNLITE_HELLO_WORLD_H_ */
