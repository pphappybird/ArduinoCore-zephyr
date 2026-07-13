/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "PDM.h"

#include <zephyr/audio/dmic.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/kernel.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(ARDUINO_NANO33BLE)
#include <hal/nrf_pdm.h>
#endif

/*
 * ---- mic power regulator configuration ----
 */

#define MIC_PWR_NODE DT_NODELABEL(mic_pwr)
#if DT_NODE_HAS_STATUS_OKAY(MIC_PWR_NODE)
static const struct device *mic_regulator = DEVICE_DT_GET(MIC_PWR_NODE);
#define MIC_PWR_PRESENT
#endif

/*
 * ---- static local variables ----
 */

static struct k_msgq pdm_rx_msgq;
static char __aligned(4) pdm_msgq_buffer[SLAB_BLOCK_NUM * sizeof(void *)];

static struct k_mem_slab pdm_slab;
static uint8_t __aligned(32) pdm_slab_buffer[SLAB_BLOCK_SIZE * SLAB_BLOCK_NUM];

static struct k_thread pdm_thread_data;
static k_thread_stack_t *pdm_thread_stack;
static k_tid_t pdm_tid;

static void (*_onReceive)(void) = NULL;

/*
 * ---- PDM DRIVER INTERFACE (zephyr dmic) ----
 */

#if defined(ARDUINO_NANO33BLE) || defined(ARDUINO_GIGA) || defined(ARDUINO_KIT_PSE84_AI)

static struct pcm_stream_cfg stream;
static struct dmic_cfg cfg;
/* the PDM mic zephyr device */
#if defined(ARDUINO_KIT_PSE84_AI)
static const struct device *const dmic_dev = DEVICE_DT_GET(DT_NODELABEL(dmic0));
#else
static const struct device *const dmic_dev = DEVICE_DT_GET(DT_NODELABEL(dmic_dev));
#endif
#if defined(ARDUINO_GIGA)
static const struct device *dfsdm_dev = DEVICE_DT_GET(DT_NODELABEL(dfsdm));
#endif

static int pdm_read(void **buffer, size_t *size) {
	return dmic_read(dmic_dev, 0, buffer, size, SYS_FOREVER_MS);
}

static int pdm_configure(int channels, int sampleRate) {
	/* checks and verifications */

	/* note: due to the hierarchical structure of the DFSDM peripheral with
	 * Arduino GIGA is necessary to turn dfsm on before the actual pdm which in
	 * this case is just a filter within the dfsdm */
#if defined(ARDUINO_GIGA)
	if (!device_is_ready(dfsdm_dev)) {
		int err = device_init(dfsdm_dev);
		if (err < 0) {
			return -ENODEV;
		}
	}
#endif
	/* verify digital microphone is ready */
	if (!device_is_ready(dmic_dev)) {

		int err = device_init(dmic_dev);
		if (err < 0) {
			printk("PDM: device_init(dmic_dev) failed, real err=%d\n", err);
			return err; /* propagate the real driver error instead of masking as -ENODEV */
		}
	}
	/* check on channels */
	if (channels < 1 || channels > 2) {
		return -ENOTSUP; /* TODO: find the correct value */
	}
	/* check on sampleRate */
	if (!(sampleRate == 16000 || sampleRate == 41667)) {
		return -ENOTSUP; /* sample rate not supported */
	}
	/* set up PDM configuration */
	stream.pcm_width = PDM_SAMPLE_BIT_WIDTH;
	stream.mem_slab = &pdm_slab;

	cfg.io.min_pdm_clk_freq = 1000000;
	cfg.io.max_pdm_clk_freq = 3500000;
	cfg.io.min_pdm_clk_dc = 40;
	cfg.io.max_pdm_clk_dc = 60;

	cfg.streams = &stream;
	cfg.channel.req_num_streams = 1;

	if (channels == 1) {
		cfg.channel.req_num_chan = 1;
#if defined(ARDUINO_KIT_PSE84_AI)
		/* Onboard mic data pin P8_6 = PDM data line 3. The Infineon driver
		 * maps a requested (ctl_idx, LR) pair to hardware PDM channel:
		 *   pdm_ch = 2 * ctl_idx + (LR == RIGHT ? 1 : 0)
		 * so (ctl_idx=1, RIGHT) selects channel 3, matching dmic0_ch3. */
		cfg.channel.req_chan_map_lo = dmic_build_channel_map(0, 1, PDM_CHAN_RIGHT);
#else
		cfg.channel.req_chan_map_lo = dmic_build_channel_map(0, 0, PDM_CHAN_LEFT);
#endif
		cfg.streams[0].pcm_rate = sampleRate;
		cfg.streams[0].block_size = SLAB_BLOCK_SIZE;
	} else {
		/* 2 channels */
		/* [TODO]: Configuration not verified on real hw */
		cfg.channel.req_num_chan = 2;
		cfg.channel.req_chan_map_lo = dmic_build_channel_map(0, 0, PDM_CHAN_LEFT) |
									  dmic_build_channel_map(1, 0, PDM_CHAN_RIGHT);
		cfg.streams[0].pcm_rate = sampleRate;
		cfg.streams[0].block_size = SLAB_BLOCK_SIZE;
	}

	/* send mic configuration to driver */
	return dmic_configure(dmic_dev, &cfg);
}

static int pdm_start() {
	return dmic_trigger(dmic_dev, DMIC_TRIGGER_START);
}

static int pdm_stop() {
	return dmic_trigger(dmic_dev, DMIC_TRIGGER_STOP);
}

static void pdm_gain(int gain) {
	/* at the present the zephyr dmic_nrfx_pdm.c does not support the set
	 * of the gain (gain_l and gain_r are defined in the nrf HAL but not
	 * used by the driver which use a default value) */
#if defined(ARDUINO_NANO33BLE)
	NRF_PDM->GAINR = gain;
	NRF_PDM->GAINL = gain;
#endif
}

#endif

/*
 * ---- MIC RECEIVING THREAD ----
 */
void pdm_thread(void *, void *, void *) {
	void *buffer;
	uint32_t size;

	while (true) {
		int ret = pdm_read(&buffer, &size);
		if (ret < 0) {
			continue;
		}

		if (k_msgq_put(&pdm_rx_msgq, &buffer, K_NO_WAIT) == 0) {
			if (_onReceive) {
				// Loop to handle leftovers without causing stack recursion
				int last_avail = PDM.available();
				while (last_avail > 0) {
					_onReceive(); // Trigger user callback

					int current_avail = PDM.available();
					// Safety catch: If the user didn't read anything, break the loop
					// to prevent the OS thread from hanging infinitely.
					if (current_avail == last_avail) {
						break;
					}
					last_avail = current_avail;
				}
			}
		} else {
			// Queue is full, drop the frame
			k_mem_slab_free(&pdm_slab, buffer);
		}
	}
}

/*
 * ---- PDM CLASS ----
 */

PDMClass::PDMClass() : pdm_init(false), active(false), active_block{nullptr, 0, 0} {
	lastError = 0;
}

PDMClass::~PDMClass() {
}

int PDMClass::begin(int channels, int sampleRate) {

	lastError = 0;

	/* SLAB & THREAD INITIALIZATION (to be performed once) */
	if (!pdm_init) {
		int err = k_mem_slab_init(&pdm_slab, pdm_slab_buffer, SLAB_BLOCK_SIZE, SLAB_BLOCK_NUM);
		if (err != 0) {
			lastError = (err < 0) ? (err - 2000) : -2000; /* slab init failure */
			return 0;                                     /* failed slab initialization */
		}
		k_msgq_init(&pdm_rx_msgq, pdm_msgq_buffer, sizeof(void *), SLAB_BLOCK_NUM);

		pdm_thread_stack = k_thread_stack_alloc(PDM_THREAD_STACK_SIZE, 0);
		if (pdm_thread_stack == NULL) {
			lastError = -3000; /* thread stack allocation failure */
			return 0;          /* failed thread stack allocation */
		}

		pdm_tid = k_thread_create(&pdm_thread_data, pdm_thread_stack, PDM_THREAD_STACK_SIZE,
								  pdm_thread, NULL, NULL, NULL, PDM_THREAD_PRIORITY, 0, K_FOREVER);
		pdm_init = true;
	}

	/* Start MIC listening */

	if (!active) {
		/* give microphone power */
#ifdef MIC_PWR_PRESENT
		if (device_is_ready(mic_regulator)) {
			regulator_enable(mic_regulator);
			/* give little time regulator */
			k_msleep(15);
		} else {
			/* [TODO]: suppose the microphone is always powered */
		}
#endif
		/* configure the microphone */
		if (!device_is_ready(dmic_dev)) {
			printk("PDM: dmic_dev NOT ready\n");
			lastError = -100; /* diagnostic: device not ready before configure */
		} else {
			printk("PDM: dmic_dev ready\n");
		}
		int cfg_ret = pdm_configure(channels, sampleRate);
		if (cfg_ret < 0) {
			printk("PDM: pdm_configure failed, ret=%d\n", cfg_ret);
			lastError = cfg_ret; /* capture dmic_configure errno */
			return 0;
		}
		printk("PDM: pdm_configure OK\n");
		/* Start the microphone BEFORE (re)starting the receiving thread.
		 * dmic_read() only blocks while the device state is ACTIVE; if the
		 * thread is started while the state is still CONFIGURED it busy-loops
		 * on -EIO and can starve lower-priority threads (e.g. setup/loop),
		 * preventing pdm_start() from ever running. */
		int start_ret = pdm_start();
		if (start_ret < 0) {
			printk("PDM: pdm_start (dmic_trigger START) failed, ret=%d\n", start_ret);
			lastError = (start_ret < 0) ?
							(start_ret - 1000) :
							-200; /* offset so caller can tell it was the start step */
			return 0;
		}
		printk("PDM: pdm_start OK\n");
		/* start or resume receiving thread now that data is flowing */
		static bool thread_started = false;
		if (!thread_started) {
			// First boot: explicitly start the thread
			k_thread_start(pdm_tid);
			thread_started = true;
		} else {
			// Subsequent boots: just resume it
			k_thread_resume(pdm_tid);
		}
		/* Set the status as ACTIVE */
		active = true;
	}
	return 1;
}

void PDMClass::end() {
	if (active) {
		/* stop the microphone */
		pdm_stop();
		/* stop receiving thread */
		k_thread_suspend(pdm_tid);
		/* shut down mic regulator */
#ifdef MIC_PWR_PRESENT
		regulator_disable(mic_regulator);
#endif

		/* Set the status as INACTIVE */
		active = false;
	}
}

int PDMClass::available() {
	int avail = 0;
	// Bytes remaining in the block currently being read
	if (active_block.data != nullptr) {
		avail += (active_block.size - active_block.offset);
	}
	// Bytes waiting in the queue
	avail += k_msgq_num_used_get(&pdm_rx_msgq) * SLAB_BLOCK_SIZE;
	return avail;
}

int PDMClass::read(void *user_buffer, size_t size) {
	size_t bytes_read = 0;
	uint8_t *dst = (uint8_t *)user_buffer;

	while (bytes_read < size) {
		// If we don't have an active block, grab the next one from the queue
		if (active_block.data == nullptr) {
			void *new_block = nullptr;
			if (k_msgq_get(&pdm_rx_msgq, &new_block, K_NO_WAIT) == 0) {
				active_block.data = new_block;
				active_block.size = SLAB_BLOCK_SIZE;
				active_block.offset = 0;
			} else {
				break; // No more data available
			}
		}

		// Calculate how much we can copy from the current block
		size_t avail_in_block = active_block.size - active_block.offset;
		size_t to_copy = size - bytes_read;
		if (to_copy > avail_in_block) {
			to_copy = avail_in_block;
		}

		// Copy directly from slab to sketch
		memcpy(dst + bytes_read, (uint8_t *)active_block.data + active_block.offset, to_copy);

		bytes_read += to_copy;
		active_block.offset += to_copy;

		// Once the block is fully consumed, free it back to the Zephyr pool
		if (active_block.offset >= active_block.size) {
			k_mem_slab_free(&pdm_slab, active_block.data);
			active_block.data = nullptr;
		}
	}
	return bytes_read;
}

void PDMClass::onReceive(void (*func)(void)) {
	_onReceive = func;
}

void PDMClass::setGain(int gain) {
	pdm_gain(gain);
}

size_t PDMClass::getBufferSize() {
	return SLAB_BLOCK_SIZE;
}

PDMClass PDM;
