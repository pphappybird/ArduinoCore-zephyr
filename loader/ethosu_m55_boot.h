/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ethosu_m55_boot.h
 * @brief One-shot CM55/Ethos-U55 bring-up performed by the secure CM33
 *        loader, so the CM55 inference server can run next to it.
 */

#ifndef ETHOSU_M55_BOOT_H_
#define ETHOSU_M55_BOOT_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Powers PD1/SOCMEM/U55, opens the shared window and the U55 to the
 *        non-secure CM55, clears the mailbox and releases the CM55.
 *
 * Runs the sequence only once; later calls return the first result.
 *
 * @return 0 on success, negative errno-style value on failure.
 */
int ethosu_m55_boot(void);

#ifdef __cplusplus
}
#endif

#endif /* ETHOSU_M55_BOOT_H_ */
