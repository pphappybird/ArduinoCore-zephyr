/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file nnlite_micro_speech.h
 * @brief Loader-side API that runs the TFLM micro_speech keyword-spotting
 *        model on the NNLite NPU via Infineon ML Middleware, exposed to the
 *        LLEXT sketch as a drop-in replacement for
 *        tflite::MicroInterpreter::Invoke().
 *
 * Must live in the statically-linked loader image, not the LLEXT sketch,
 * since mtb_ml_init() needs SYS_INIT ordering and irq_connect_dynamic().
 * Feature extraction and command recognition stay with the caller (the
 * sketch); only the model inference step is offloaded here.
 */

#ifndef NNLITE_MICRO_SPEECH_H_
#define NNLITE_MICRO_SPEECH_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief One-time initialization: brings up the ML Middleware/NNLite NPU and
 *        loads the micro_speech model. Safe to call more than once; only the
 *        first call performs the actual initialization.
 *
 * @param[out] input_scale       Quantization scale of the model's input tensor.
 * @param[out] input_zero_point  Quantization zero point of the model's input tensor.
 * @param[out] output_scale      Quantization scale of the model's output tensor.
 * @param[out] output_zero_point Quantization zero point of the model's output tensor.
 *
 * @return 0 on success, negative errno-style value on failure.
 */
int nnlite_micro_speech_init(float *input_scale, int32_t *input_zero_point,
			      float *output_scale, int32_t *output_zero_point);

/**
 * @brief Runs one inference of the micro_speech model on NNLite.
 *
 * @param[in]  features   Quantized int8 feature buffer (kFeatureElementCount
 *                         elements, i.e. 1960, already computed by the
 *                         sketch's FeatureProvider).
 * @param[out] scores_out Quantized int8 category scores (kCategoryCount
 *                         elements, i.e. 4).
 *
 * @return 0 on success, negative errno-style value on failure.
 */
int nnlite_micro_speech_infer(const int8_t *features, int8_t *scores_out);

#ifdef __cplusplus
}
#endif

#endif /* NNLITE_MICRO_SPEECH_H_ */
