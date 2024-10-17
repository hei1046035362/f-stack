/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(C) 2022 Marvell.
 */

#include "cn10k_worker.h"
#include "cnxk_eventdev.h"
#include "cnxk_worker.h"

#define R(name, flags)                                                         \
	SSO_CMN_DEQ_BURST(cn10k_sso_hws_deq_tmo_seg_burst_##name,              \
			  cn10k_sso_hws_deq_tmo_seg_##name, flags)             \
	SSO_CMN_DEQ_BURST(cn10k_sso_hws_reas_deq_tmo_seg_burst_##name,         \
			cn10k_sso_hws_reas_deq_tmo_seg_##name, flags | NIX_RX_REAS_F)

NIX_RX_FASTPATH_MODES_112_127
#undef R
