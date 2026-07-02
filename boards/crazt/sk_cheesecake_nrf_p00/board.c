/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/init.h>
#include <hal/nrf_power.h>
#include <stdint.h>

static int board_sk_cheesecake_nrf_p00_init(void)
{
	uint32_t regout0 = NRF_UICR->REGOUT0 & UICR_REGOUT0_VOUT_Msk;

	/* UICR can only clear bits without an erase, so update the factory default only. */
	if ((nrf_power_mainregstatus_get(NRF_POWER) == NRF_POWER_MAINREGSTATUS_HIGH) &&
	    (regout0 == (UICR_REGOUT0_VOUT_DEFAULT << UICR_REGOUT0_VOUT_Pos))) {
		NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Wen << NVMC_CONFIG_WEN_Pos;
		while (NRF_NVMC->READY == NVMC_READY_READY_Busy) {
			;
		}

		NRF_UICR->REGOUT0 =
			(NRF_UICR->REGOUT0 & ~((uint32_t)UICR_REGOUT0_VOUT_Msk)) |
			(UICR_REGOUT0_VOUT_3V3 << UICR_REGOUT0_VOUT_Pos);

		NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren << NVMC_CONFIG_WEN_Pos;
		while (NRF_NVMC->READY == NVMC_READY_READY_Busy) {
			;
		}

		NVIC_SystemReset();
	}

	return 0;
}

SYS_INIT(board_sk_cheesecake_nrf_p00_init, PRE_KERNEL_1,
	 CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
