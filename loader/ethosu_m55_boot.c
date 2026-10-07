/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ethosu_m55_boot.c
 * @brief CM55/Ethos-U55 bring-up from the secure CM33 loader.
 *
 * Follows the upstream PSE84 CM55 start sequence (security_config/
 * pse84_boot.c) with three differences: the loader keeps running
 * afterwards, only the CM55-related MPC domains are applied, and only the
 * U55 PPC regions are made non-secure unless CONFIG_ARDUINO_ETHOSU_PPC_ALL_NS
 * is set. The SAU gets non-secure regions for the memory the loader shares
 * with the CM55, so its secure accesses there are not blocked by the MPC.
 */

#include "ethosu_m55_boot.h"

#ifdef CONFIG_ARDUINO_ETHOSU_REMOTE

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "cy_pdl.h"
#include "cy_syspm_ppu.h"
#include "system_edge.h"
#include "pse84_s_protection.h"
#include "ethosu_ipc.h"

/** Time the CM55 is held after reset release, as in the upstream boot. */
#define ETHOSU_CM55_BOOT_WAIT_US (10U)

/** Non-secure RAMC1 range made non-secure by the CM33_M55 MPC domain; it
 *  holds the .cy_sharedmem section the loader itself uses. */
#define ETHOSU_RAMC1_NS_BASE  (0x240BD000UL)
#define ETHOSU_RAMC1_NS_LIMIT (0x24100000UL)

/** SAU regions are 32-byte granular. */
#define ETHOSU_SAU_GRANULE (32UL)

#if !defined(CONFIG_ARDUINO_ETHOSU_PPC_ALL_NS)
/** U55 peripheral regions opened to the non-secure CM55. */
static const cy_en_prot_region_t ethosu_ppc_regions[] = {
	PROT_PERI1_U550_U55,
	PROT_PERI1_U550_ROT,
	PROT_PERI1_U550_ACG,
	PROT_PERI1_U550_MXU55_PPU_PPU,
	PROT_PERI1_U550_PD_PD,
};
#endif

static bool ethosu_boot_done;
static int ethosu_boot_result;

/** Applies the MPC configuration of one unified domain, skipping RRAM,
 *  which needs a separate sequence and is not used by the CM55 server. */
static int ethosu_mpc_apply_domain(uint32_t domain_idx) {
	const cy_stc_mpc_unified_t *domain;
	uint32_t region_idx;
	uint32_t cfg_idx;

	if (domain_idx >= (uint32_t)unified_mpc_domains_count) {
		return -EINVAL;
	}

	domain = &unified_mpc_domains[domain_idx];

	for (region_idx = 0U; region_idx < domain->region_count; region_idx++) {
		const cy_stc_mpc_regions_t *region = &domain->regions[region_idx];

		if ((region->base == NULL) || (region->base == (MPC_Type *)RRAMC0_MPC0)) {
			continue;
		}

		for (cfg_idx = 0U; cfg_idx < domain->cfg_count; cfg_idx++) {
			if (Cy_Mpc_ConfigRotMpcStruct(region->base, region->offset, region->size,
						      &domain->cfg[cfg_idx]) != CY_RSLT_SUCCESS) {
				return -EIO;
			}
		}
	}

	return 0;
}

static int ethosu_mpc_init(void) {
	size_t idx;
	int ret;

	for (idx = 0U; idx < cy_response_mpcs_count; idx++) {
		Cy_Mpc_SetViolationResponse(cy_response_mpcs[idx].base, cy_response_mpcs[idx].response);
	}

	ret = ethosu_mpc_apply_domain(M55_UNIFIED_MPC_DOMAIN_IDX);
	if (ret == 0) {
		ret = ethosu_mpc_apply_domain(M33_M55_UNIFIED_MPC_DOMAIN_IDX);
	}

	return ret;
}

/** Same steps as Cy_SysU55Enable(), which PDL only builds for the CM55. */
static int ethosu_u55_power_on(void) {
	Cy_SysClk_PeriGroupSlaveInit(CY_MMIO_MXU550_PERI_NR, CY_MMIO_MXU550_GROUP_NR,
				     CY_MMIO_MXU550_SLAVE_NR, CY_MMIO_MXU550_CLK_HF_NR);

	if (cy_pd_ppu_set_power_mode((struct ppu_v1_reg *)CY_PPU_U55_BASE,
				     (uint32_t)PPU_V1_MODE_ON) != CY_SYSPM_SUCCESS) {
		return -EIO;
	}

	if (cy_pd_pdcm_set_dependency(CY_PD_PDCM_U55, CY_PD_PDCM_APPCPU) != CY_SYSPM_SUCCESS) {
		return -EIO;
	}

	return 0;
}

#if !defined(CONFIG_ARDUINO_ETHOSU_PPC_ALL_NS)
static int ethosu_ppc_open_u55(void) {
	size_t idx;

	for (idx = 0U; idx < ARRAY_SIZE(ethosu_ppc_regions); idx++) {
		if (Cy_Ppc_ConfigAttrib(PPC1, ethosu_ppc_regions[idx], &cycfg_unused_ppc_cfg) !=
		    CY_PPC_SUCCESS) {
			return -EIO;
		}

		if (Cy_Ppc_SetPcMask(PPC1, ethosu_ppc_regions[idx], PPC_PC_MASK_ALL_ACCESS) !=
		    CY_PPC_SUCCESS) {
			return -EIO;
		}
	}

	return 0;
}
#endif

/** Programs a free SAU region as non-secure for [base, limit). */
static int ethosu_sau_add_ns(uint32_t base, uint32_t limit) {
	uint32_t count = (SAU->TYPE & SAU_TYPE_SREGION_Msk) >> SAU_TYPE_SREGION_Pos;
	uint32_t idx;

	for (idx = 0U; idx < count; idx++) {
		SAU->RNR = idx;
		if ((SAU->RLAR & SAU_RLAR_ENABLE_Msk) == 0U) {
			SAU->RBAR = base & SAU_RBAR_BADDR_Msk;
			SAU->RLAR = ((limit - ETHOSU_SAU_GRANULE) & SAU_RLAR_LADDR_Msk) |
				    SAU_RLAR_ENABLE_Msk;
			return 0;
		}
	}

	return -ENOSPC;
}

/** Enables the SAU without ALLNS, so everything not covered stays secure. */
static int ethosu_sau_init(void) {
	int ret;

	ret = ethosu_sau_add_ns(ETHOSU_IPC_SHM_BASE, ETHOSU_IPC_SHM_BASE + ETHOSU_IPC_SHM_SIZE);
	if (ret == 0) {
		ret = ethosu_sau_add_ns(ETHOSU_RAMC1_NS_BASE, ETHOSU_RAMC1_NS_LIMIT);
	}

	if (ret == 0) {
		SAU->CTRL = (SAU->CTRL & ~SAU_CTRL_ALLNS_Msk) | SAU_CTRL_ENABLE_Msk;
		__DSB();
		__ISB();
	}

	return ret;
}

#if defined(CONFIG_ARDUINO_ETHOSU_PPC_ALL_NS)
/** Same as cy_ppc_unsecure_init(), but keeps the regions of peripherals the
 *  secure loader still uses (DMA, console UART, PDM) secure. */
static cy_rslt_t ethosu_ppc_all_ns(PPC_Type *base, cy_en_prot_region_t start,
				   cy_en_prot_region_t end) {
	cy_rslt_t ret;
	cy_en_prot_region_t region;

	ret = Cy_Ppc_InitPpc(base, CY_PPC_BUS_ERR);

	for (region = start; (ret == CY_PPC_SUCCESS) && (region <= end); region++) {
		if ((region == PROT_PERI1_PPC1_PPC_PPC_SECURE) ||
		    (region == PROT_PERI1_PPC1_PPC_PPC_NONSECURE)) {
			continue;
		}

		/* DW0/DW1 serve the running secure PDM DMA; keep them secure. */
		if ((base == PPC0) && (region >= PROT_PERI0_DW0_DW) &&
		    (region <= PROT_PERI0_DW1_CH_STRUCT15_CH)) {
			continue;
		}

		/* SCB0..SCB11 include the secure console UART (SCB2); keep them secure. */
		if ((base == PPC0) && (region >= PROT_PERI0_SCB0) && (region <= PROT_PERI0_SCB1)) {
			continue;
		}

		/* PDM0 is read by the secure DMA for the running microphone. */
		if ((base == PPC1) && (region >= PROT_PERI1_PDM0_MAIN) &&
		    (region <= PROT_PERI1_PDM0_CH5_RX)) {
			continue;
		}

		ret = Cy_Ppc_ConfigAttrib(base, region, &cycfg_unused_ppc_cfg);
		if (ret == CY_RSLT_SUCCESS) {
			ret = Cy_Ppc_SetPcMask(base, region, PPC_PC_MASK_ALL_ACCESS);
		}
	}

	return ret;
}
#endif

static int ethosu_m55_boot_sequence(void) {
	int ret;

	Cy_System_EnablePD1();

	Cy_SysClk_PeriGroupSlaveInit(CY_MMIO_CM55_TCM_512K_PERI_NR, CY_MMIO_CM55_TCM_512K_GROUP_NR,
				     CY_MMIO_CM55_TCM_512K_SLAVE_NR,
				     CY_MMIO_CM55_TCM_512K_CLK_HF_NR);
	Cy_SysClk_PeriGroupSlaveInit(CY_MMIO_SMIF0_PERI_NR, CY_MMIO_SMIF0_GROUP_NR,
				     CY_MMIO_SMIF0_SLAVE_NR, CY_MMIO_SMIF0_CLK_HF_NR);
	Cy_SysClk_PeriGroupSlaveInit(CY_MMIO_SMIF01_PERI_NR, CY_MMIO_SMIF01_GROUP_NR,
				     CY_MMIO_SMIF01_SLAVE_NR, CY_MMIO_SMIF01_CLK_HF_NR);

	Cy_SysEnableSOCMEM(true);

	ret = ethosu_mpc_init();
	if (ret != 0) {
		return ret;
	}

	/* The PPU and PD registers become non-secure below, so power first. */
	ret = ethosu_u55_power_on();
	if (ret != 0) {
		return ret;
	}

#if !defined(CONFIG_ARDUINO_ETHOSU_PPC_ALL_NS)
	ret = ethosu_ppc_open_u55();
	if (ret != 0) {
		return ret;
	}
#endif

	ret = ethosu_sau_init();
	if (ret != 0) {
		return ret;
	}

	/* Clean mailbox: the server treats req_seq == 0 as nothing pending. */
	(void)memset((void *)ETHOSU_IPC_SHM_BASE, 0, ETHOSU_IPC_MAILBOX_SIZE);
	__DSB();

	(void)cy_pd_pdcm_clear_dependency(CY_PD_PDCM_APPCPUSS, CY_PD_PDCM_SYSCPU);
	(void)cy_pd_pdcm_clear_dependency(CY_PD_PDCM_APPCPU, CY_PD_PDCM_SYSCPU);

	Cy_SysEnableCM55(MXCM55, CONFIG_ARDUINO_ETHOSU_M55_VTOR, ETHOSU_CM55_BOOT_WAIT_US);

#if defined(CONFIG_ARDUINO_ETHOSU_PPC_ALL_NS)
	/* Applied right after release: the CM55 early init needs these at once. */
	{
		cy_rslt_t rc0 = ethosu_ppc_all_ns(PPC0, PROT_PERI0_START, PROT_PERI0_END);
		cy_rslt_t rc1 = ethosu_ppc_all_ns(PPC1, PROT_PERI1_START, PROT_PERI1_END);

		if ((rc0 != CY_RSLT_SUCCESS) || (rc1 != CY_RSLT_SUCCESS)) {
			return -EIO;
		}
	}
#endif

	return 0;
}

int ethosu_m55_boot(void) {
	if (!ethosu_boot_done) {
		ethosu_boot_result = ethosu_m55_boot_sequence();
		ethosu_boot_done = true;
	}

	return ethosu_boot_result;
}

#endif /* CONFIG_ARDUINO_ETHOSU_REMOTE */
