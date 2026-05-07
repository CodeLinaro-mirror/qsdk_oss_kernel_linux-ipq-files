/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <linux/bitfield.h>
#include <linux/bitmap.h>
#include <linux/bits.h>
#include <linux/device.h>
#include <linux/random.h>

#include "qce2204.h"
#include "qce2204_ppe.h"
#include "qce2204_ppe_regs.h"

/* 3550 for HTT general group0 */
static const int qce2204_ppe_bm_group_config = 3550;

/* BM port configurations for QCE2204 (15 BM ports) */
static const struct qce2204_ppe_bm_port_config qce2204_ppe_bm_port_config[] = {
	{
		/* BM port 0: EDMA port 0 (CPU) */
		.port_id_start	= 0,
		.port_id_end	= 0,
		.pre_alloc	= 0,
		.in_fly_buf	= 200,
		.ceil		= 1200,
		.weight		= 7,
		.resume_offset	= 8,
		.resume_ceil	= 0,
		.dynamic	= true,
	},
	{
		/* BM ports 1-5: EDMA ports 1-5 */
		.port_id_start	= 1,
		.port_id_end	= 5,
		.pre_alloc	= 0,
		.in_fly_buf	= 228,
		.ceil		= 650,
		.weight		= 7,
		.resume_offset	= 36,
		.resume_ceil	= 0,
		.dynamic	= true,
	},
};

/* 4000 for HTT general group0 */
static const int qce2204_ppe_qm_group_config = 4000;

/* QM queue configurations for QCE2204 */
static const struct qce2204_ppe_qm_queue_config qce2204_ppe_qm_queue_config[] = {
	{
		/* Unicast queues 0-255 */
		.queue_start	= 0,
		.queue_end	= 255,
		.prealloc_buf	= 0,
		.ceil		= 2200,
		.weight		= 7,
		.resume_offset	= 36,
		.dynamic	= true,
	},
	{
		/* Multicast queues 256-291 */
		.queue_start	= 256,
		.queue_end	= 291,
		.prealloc_buf	= 0,
		.ceil		= 250,
		.weight		= 0,
		.resume_offset	= 36,
		.dynamic	= false,
	},
};

/* BM scheduler configuration per design spec */
static const struct qce2204_ppe_scheduler_bm_config qce2204_ppe_sch_bm_config[] = {
/* 0x564 */ {true,  QCE2204_PPE_INGRESS, 4, true,  5},
/* 0x170 */ {true,  QCE2204_PPE_EGRESS,  0, true,  1},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x275 */ {true,  QCE2204_PPE_EGRESS,  5, true,  2},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x071 */ {true,  QCE2204_PPE_EGRESS,  1, true,  0},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x572 */ {true,  QCE2204_PPE_EGRESS,  2, true,  5},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x370 */ {true,  QCE2204_PPE_EGRESS,  0, true,  3},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x475 */ {true,  QCE2204_PPE_EGRESS,  5, true,  4},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x170 */ {true,  QCE2204_PPE_EGRESS,  0, true,  1},
/* 0x561 */ {true,  QCE2204_PPE_INGRESS, 1, true,  5},
/* 0x275 */ {true,  QCE2204_PPE_EGRESS,  5, true,  2},
/* 0x562 */ {true,  QCE2204_PPE_INGRESS, 2, true,  5},
/* 0x073 */ {true,  QCE2204_PPE_EGRESS,  3, true,  0},
/* 0x063 */ {true,  QCE2204_PPE_INGRESS, 3, true,  0},
/* 0x574 */ {true,  QCE2204_PPE_EGRESS,  4, true,  5},
/* 0x064 */ {true,  QCE2204_PPE_INGRESS, 4, true,  0},
/* 0x370 */ {true,  QCE2204_PPE_EGRESS,  0, true,  3},
/* 0x000 */ {false, QCE2204_PPE_INGRESS, 0, false, 0},
/* 0x475 */ {true,  QCE2204_PPE_EGRESS,  5, true,  4},
/* 0x000 */ {false, QCE2204_PPE_INGRESS, 0, false, 0},
/* 0x170 */ {true,  QCE2204_PPE_EGRESS,  0, true,  1},
/* 0x000 */ {false, QCE2204_PPE_INGRESS, 0, false, 0},
/* 0x275 */ {true,  QCE2204_PPE_EGRESS,  5, true,  2},
/* 0x025 */ {true,  QCE2204_PPE_INGRESS, 5, false, 0},
/* 0x170 */ {true,  QCE2204_PPE_EGRESS,  0, true,  1},
/* 0x000 */ {false, QCE2204_PPE_INGRESS, 0, false, 0},
/* 0x275 */ {true,  QCE2204_PPE_EGRESS,  5, true,  2},
/* 0x000 */ {false, QCE2204_PPE_INGRESS, 0, false, 0},
/* 0x071 */ {true,  QCE2204_PPE_EGRESS,  1, true,  0},
/* 0x020 */ {true,  QCE2204_PPE_INGRESS, 0, false, 0},
/* 0x572 */ {true,  QCE2204_PPE_EGRESS,  2, true,  5},
/* 0x000 */ {false, QCE2204_PPE_INGRESS, 0, false, 0},
/* 0x370 */ {true,  QCE2204_PPE_EGRESS,  0, true,  3},
/* 0x020 */ {true,  QCE2204_PPE_INGRESS, 0, false, 0},
/* 0x475 */ {true,  QCE2204_PPE_EGRESS,  5, true,  4},
/* 0x000 */ {false, QCE2204_PPE_INGRESS, 0, false, 0},
/* 0x170 */ {true,  QCE2204_PPE_EGRESS,  0, true,  1},
/* 0x020 */ {true,  QCE2204_PPE_INGRESS, 0, false, 0},
/* 0x275 */ {true,  QCE2204_PPE_EGRESS,  5, true,  2},
/* 0x025 */ {true,  QCE2204_PPE_INGRESS, 5, false, 0},
/* 0x073 */ {true,  QCE2204_PPE_EGRESS,  3, true,  0},
/* 0x020 */ {true,  QCE2204_PPE_INGRESS, 0, false, 0},
/* 0x574 */ {true,  QCE2204_PPE_EGRESS,  4, true,  5},
/* 0x000 */ {false, QCE2204_PPE_INGRESS, 0, false, 0},
/* 0x370 */ {true,  QCE2204_PPE_EGRESS,  0, true,  3},
/* 0x000 */ {false, QCE2204_PPE_INGRESS, 0, false, 0},
/* 0x475 */ {true,  QCE2204_PPE_EGRESS,  5, true,  4},
/* 0x000 */ {false, QCE2204_PPE_INGRESS, 0, false, 0},
/* 0x370 */ {true,  QCE2204_PPE_EGRESS,  0, true,  3},
/* 0x000 */ {false, QCE2204_PPE_INGRESS, 0, false, 0},
/* 0x475 */ {true,  QCE2204_PPE_EGRESS,  5, true,  4},
/* 0x000 */ {false, QCE2204_PPE_INGRESS, 0, false, 0},
/* 0x170 */ {true,  QCE2204_PPE_EGRESS,  0, true,  1},
/* 0x000 */ {false, QCE2204_PPE_INGRESS, 0, false, 0},
/* 0x275 */ {true,  QCE2204_PPE_EGRESS,  5, true,  2},
/* 0x000 */ {false, QCE2204_PPE_INGRESS, 0, false, 0},
/* 0x071 */ {true,  QCE2204_PPE_EGRESS,  1, true,  0},
/* 0x000 */ {false, QCE2204_PPE_INGRESS, 0, false, 0},
/* 0x572 */ {true,  QCE2204_PPE_EGRESS,  2, true,  5},
/* 0x000 */ {false, QCE2204_PPE_INGRESS, 0, false, 0},
/* 0x370 */ {true,  QCE2204_PPE_EGRESS,  0, true,  3},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x475 */ {true,  QCE2204_PPE_EGRESS,  5, true,  4},
/* 0x561 */ {true,  QCE2204_PPE_INGRESS, 1, true,  5},
/* 0x170 */ {true,  QCE2204_PPE_EGRESS,  0, true,  1},
/* 0x562 */ {true,  QCE2204_PPE_INGRESS, 2, true,  5},
/* 0x275 */ {true,  QCE2204_PPE_EGRESS,  5, true,  2},
/* 0x563 */ {true,  QCE2204_PPE_INGRESS, 3, true,  5},
/* 0x073 */ {true,  QCE2204_PPE_EGRESS,  3, true,  0},
/* 0x564 */ {true,  QCE2204_PPE_INGRESS, 4, true,  5},
/* 0x574 */ {true,  QCE2204_PPE_EGRESS,  4, true,  5},
/* 0x565 */ {true,  QCE2204_PPE_INGRESS, 5, true,  5},
/* 0x370 */ {true,  QCE2204_PPE_EGRESS,  0, true,  3},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x475 */ {true,  QCE2204_PPE_EGRESS,  5, true,  4},
/* 0x561 */ {true,  QCE2204_PPE_INGRESS, 1, true,  5},
/* 0x170 */ {true,  QCE2204_PPE_EGRESS,  0, true,  1},
/* 0x562 */ {true,  QCE2204_PPE_INGRESS, 2, true,  5},
/* 0x275 */ {true,  QCE2204_PPE_EGRESS,  5, true,  2},
/* 0x563 */ {true,  QCE2204_PPE_INGRESS, 3, true,  5},
/* 0x170 */ {true,  QCE2204_PPE_EGRESS,  0, true,  1},
/* 0x564 */ {true,  QCE2204_PPE_INGRESS, 4, true,  5},
/* 0x275 */ {true,  QCE2204_PPE_EGRESS,  5, true,  2},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x071 */ {true,  QCE2204_PPE_EGRESS,  1, true,  0},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x572 */ {true,  QCE2204_PPE_EGRESS,  2, true,  5},
/* 0x561 */ {true,  QCE2204_PPE_INGRESS, 1, true,  5},
/* 0x370 */ {true,  QCE2204_PPE_EGRESS,  0, true,  3},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x475 */ {true,  QCE2204_PPE_EGRESS,  5, true,  4},
/* 0x562 */ {true,  QCE2204_PPE_INGRESS, 2, true,  5},
/* 0x170 */ {true,  QCE2204_PPE_EGRESS,  0, true,  1},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x275 */ {true,  QCE2204_PPE_EGRESS,  5, true,  2},
/* 0x563 */ {true,  QCE2204_PPE_INGRESS, 3, true,  5},
/* 0x073 */ {true,  QCE2204_PPE_EGRESS,  3, true,  0},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x574 */ {true,  QCE2204_PPE_EGRESS,  4, true,  5},
/* 0x564 */ {true,  QCE2204_PPE_INGRESS, 4, true,  5},
/* 0x370 */ {true,  QCE2204_PPE_EGRESS,  0, true,  3},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x475 */ {true,  QCE2204_PPE_EGRESS,  5, true,  4},
/* 0x561 */ {true,  QCE2204_PPE_INGRESS, 1, true,  5},
/* 0x370 */ {true,  QCE2204_PPE_EGRESS,  0, true,  3},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x475 */ {true,  QCE2204_PPE_EGRESS,  5, true,  4},
/* 0x562 */ {true,  QCE2204_PPE_INGRESS, 2, true,  5},
/* 0x170 */ {true,  QCE2204_PPE_EGRESS,  0, true,  1},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x275 */ {true,  QCE2204_PPE_EGRESS,  5, true,  2},
/* 0x563 */ {true,  QCE2204_PPE_INGRESS, 3, true,  5},
/* 0x071 */ {true,  QCE2204_PPE_EGRESS,  1, true,  0},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x572 */ {true,  QCE2204_PPE_EGRESS,  2, true,  5},
/* 0x564 */ {true,  QCE2204_PPE_INGRESS, 4, true,  5},
/* 0x370 */ {true,  QCE2204_PPE_EGRESS,  0, true,  3},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x475 */ {true,  QCE2204_PPE_EGRESS,  5, true,  4},
/* 0x561 */ {true,  QCE2204_PPE_INGRESS, 1, true,  5},
/* 0x170 */ {true,  QCE2204_PPE_EGRESS,  0, true,  1},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x275 */ {true,  QCE2204_PPE_EGRESS,  5, true,  2},
/* 0x562 */ {true,  QCE2204_PPE_INGRESS, 2, true,  5},
/* 0x073 */ {true,  QCE2204_PPE_EGRESS,  3, true,  0},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x574 */ {true,  QCE2204_PPE_EGRESS,  4, true,  5},
/* 0x563 */ {true,  QCE2204_PPE_INGRESS, 3, true,  5},
/* 0x370 */ {true,  QCE2204_PPE_EGRESS,  0, true,  3},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x475 */ {true,  QCE2204_PPE_EGRESS,  5, true,  4},
/* 0x564 */ {true,  QCE2204_PPE_INGRESS, 4, true,  5},
/* 0x170 */ {true,  QCE2204_PPE_EGRESS,  0, true,  1},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x275 */ {true,  QCE2204_PPE_EGRESS,  5, true,  2},
/* 0x561 */ {true,  QCE2204_PPE_INGRESS, 1, true,  5},
/* 0x170 */ {true,  QCE2204_PPE_EGRESS,  0, true,  1},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x275 */ {true,  QCE2204_PPE_EGRESS,  5, true,  2},
/* 0x562 */ {true,  QCE2204_PPE_INGRESS, 2, true,  5},
/* 0x071 */ {true,  QCE2204_PPE_EGRESS,  1, true,  0},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x572 */ {true,  QCE2204_PPE_EGRESS,  2, true,  5},
/* 0x563 */ {true,  QCE2204_PPE_INGRESS, 3, true,  5},
/* 0x370 */ {true,  QCE2204_PPE_EGRESS,  0, true,  3},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x475 */ {true,  QCE2204_PPE_EGRESS,  5, true,  4},
/* 0x564 */ {true,  QCE2204_PPE_INGRESS, 4, true,  5},
/* 0x170 */ {true,  QCE2204_PPE_EGRESS,  0, true,  1},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x275 */ {true,  QCE2204_PPE_EGRESS,  5, true,  2},
/* 0x561 */ {true,  QCE2204_PPE_INGRESS, 1, true,  5},
/* 0x073 */ {true,  QCE2204_PPE_EGRESS,  3, true,  0},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x574 */ {true,  QCE2204_PPE_EGRESS,  4, true,  5},
/* 0x562 */ {true,  QCE2204_PPE_INGRESS, 2, true,  5},
/* 0x370 */ {true,  QCE2204_PPE_EGRESS,  0, true,  3},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x475 */ {true,  QCE2204_PPE_EGRESS,  5, true,  4},
/* 0x563 */ {true,  QCE2204_PPE_INGRESS, 3, true,  5},
/* 0x370 */ {true,  QCE2204_PPE_EGRESS,  0, true,  3},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x475 */ {true,  QCE2204_PPE_EGRESS,  5, true,  4},
/* 0x564 */ {true,  QCE2204_PPE_INGRESS, 4, true,  5},
/* 0x170 */ {true,  QCE2204_PPE_EGRESS,  0, true,  1},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x275 */ {true,  QCE2204_PPE_EGRESS,  5, true,  2},
/* 0x561 */ {true,  QCE2204_PPE_INGRESS, 1, true,  5},
/* 0x071 */ {true,  QCE2204_PPE_EGRESS,  1, true,  0},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x572 */ {true,  QCE2204_PPE_EGRESS,  2, true,  5},
/* 0x562 */ {true,  QCE2204_PPE_INGRESS, 2, true,  5},
/* 0x370 */ {true,  QCE2204_PPE_EGRESS,  0, true,  3},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x475 */ {true,  QCE2204_PPE_EGRESS,  5, true,  4},
/* 0x563 */ {true,  QCE2204_PPE_INGRESS, 3, true,  5},
/* 0x170 */ {true,  QCE2204_PPE_EGRESS,  0, true,  1},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x275 */ {true,  QCE2204_PPE_EGRESS,  5, true,  2},
/* 0x564 */ {true,  QCE2204_PPE_INGRESS, 4, true,  5},
/* 0x073 */ {true,  QCE2204_PPE_EGRESS,  3, true,  0},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x574 */ {true,  QCE2204_PPE_EGRESS,  4, true,  5},
/* 0x561 */ {true,  QCE2204_PPE_INGRESS, 1, true,  5},
/* 0x370 */ {true,  QCE2204_PPE_EGRESS,  0, true,  3},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x475 */ {true,  QCE2204_PPE_EGRESS,  5, true,  4},
/* 0x562 */ {true,  QCE2204_PPE_INGRESS, 2, true,  5},
/* 0x170 */ {true,  QCE2204_PPE_EGRESS,  0, true,  1},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x275 */ {true,  QCE2204_PPE_EGRESS,  5, true,  2},
/* 0x563 */ {true,  QCE2204_PPE_INGRESS, 3, true,  5},
/* 0x170 */ {true,  QCE2204_PPE_EGRESS,  0, true,  1},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x275 */ {true,  QCE2204_PPE_EGRESS,  5, true,  2},
/* 0x564 */ {true,  QCE2204_PPE_INGRESS, 4, true,  5},
/* 0x071 */ {true,  QCE2204_PPE_EGRESS,  1, true,  0},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x572 */ {true,  QCE2204_PPE_EGRESS,  2, true,  5},
/* 0x561 */ {true,  QCE2204_PPE_INGRESS, 1, true,  5},
/* 0x370 */ {true,  QCE2204_PPE_EGRESS,  0, true,  3},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x475 */ {true,  QCE2204_PPE_EGRESS,  5, true,  4},
/* 0x562 */ {true,  QCE2204_PPE_INGRESS, 2, true,  5},
/* 0x170 */ {true,  QCE2204_PPE_EGRESS,  0, true,  1},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x275 */ {true,  QCE2204_PPE_EGRESS,  5, true,  2},
/* 0x563 */ {true,  QCE2204_PPE_INGRESS, 3, true,  5},
/* 0x073 */ {true,  QCE2204_PPE_EGRESS,  3, true,  0},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x574 */ {true,  QCE2204_PPE_EGRESS,  4, true,  5},
/* 0x564 */ {true,  QCE2204_PPE_INGRESS, 4, true,  5},
/* 0x370 */ {true,  QCE2204_PPE_EGRESS,  0, true,  3},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x475 */ {true,  QCE2204_PPE_EGRESS,  5, true,  4},
/* 0x561 */ {true,  QCE2204_PPE_INGRESS, 1, true,  5},
/* 0x370 */ {true,  QCE2204_PPE_EGRESS,  0, true,  3},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x475 */ {true,  QCE2204_PPE_EGRESS,  5, true,  4},
/* 0x562 */ {true,  QCE2204_PPE_INGRESS, 2, true,  5},
/* 0x170 */ {true,  QCE2204_PPE_EGRESS,  0, true,  1},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x275 */ {true,  QCE2204_PPE_EGRESS,  5, true,  2},
/* 0x563 */ {true,  QCE2204_PPE_INGRESS, 3, true,  5},
/* 0x071 */ {true,  QCE2204_PPE_EGRESS,  1, true,  0},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x572 */ {true,  QCE2204_PPE_EGRESS,  2, true,  5},
/* 0x564 */ {true,  QCE2204_PPE_INGRESS, 4, true,  5},
/* 0x370 */ {true,  QCE2204_PPE_EGRESS,  0, true,  3},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x475 */ {true,  QCE2204_PPE_EGRESS,  5, true,  4},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x170 */ {true,  QCE2204_PPE_EGRESS,  0, true,  1},
/* 0x561 */ {true,  QCE2204_PPE_INGRESS, 1, true,  5},
/* 0x275 */ {true,  QCE2204_PPE_EGRESS,  5, true,  2},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x073 */ {true,  QCE2204_PPE_EGRESS,  3, true,  0},
/* 0x562 */ {true,  QCE2204_PPE_INGRESS, 2, true,  5},
/* 0x574 */ {true,  QCE2204_PPE_EGRESS,  4, true,  5},
/* 0x560 */ {true,  QCE2204_PPE_INGRESS, 0, true,  5},
/* 0x370 */ {true,  QCE2204_PPE_EGRESS,  0, true,  3},
/* 0x563 */ {true,  QCE2204_PPE_INGRESS, 3, true,  5},
/* 0x475 */ {true,  QCE2204_PPE_EGRESS,  5, true,  4},
};

/* QM scheduler configuration per design spec */
static const struct qce2204_ppe_scheduler_qm_config qce2204_ppe_sch_qm_config[] = {
/* 0x045 */ {0x0, 0x4, 0x5, false, 0x0},
/* 0x030 */ {0x0, 0x3, 0x0, false, 0x0},
/* 0x025 */ {0x0, 0x2, 0x5, false, 0x0},
/* 0x014 */ {0x0, 0x1, 0x4, false, 0x0},
/* 0x003 */ {0x0, 0x0, 0x3, false, 0x0},
/* 0x052 */ {0x0, 0x5, 0x2, false, 0x0},
/* 0x0F1 */ {0x0, 0xF, 0x1, false, 0x0},
/* 0x0F0 */ {0x0, 0xF, 0x0, false, 0x0},
/* 0x0F5 */ {0x0, 0xF, 0x5, false, 0x0},
/* 0x0F1 */ {0x0, 0xF, 0x1, false, 0x0},
/* 0x0F0 */ {0x0, 0xF, 0x0, false, 0x0},
/* 0x0F5 */ {0x0, 0xF, 0x5, false, 0x0},
/* 0x0F2 */ {0x0, 0xF, 0x2, false, 0x0},
/* 0x0F0 */ {0x0, 0xF, 0x0, false, 0x0},
/* 0x0F5 */ {0x0, 0xF, 0x5, false, 0x0},
/* 0x0F3 */ {0x0, 0xF, 0x3, false, 0x0},
/* 0x0F0 */ {0x0, 0xF, 0x0, false, 0x0},
/* 0x0F5 */ {0x0, 0xF, 0x5, false, 0x0},
/* 0x0F4 */ {0x0, 0xF, 0x4, false, 0x0},
/* 0x0F0 */ {0x0, 0xF, 0x0, false, 0x0},
/* 0x0F5 */ {0x0, 0xF, 0x5, false, 0x0},
/* 0x0F1 */ {0x0, 0xF, 0x1, false, 0x0},
/* 0x0F0 */ {0x0, 0xF, 0x0, false, 0x0},
/* 0x0F5 */ {0x0, 0xF, 0x5, false, 0x0},
/* 0x0F2 */ {0x0, 0xF, 0x2, false, 0x0},
/* 0x0F0 */ {0x0, 0xF, 0x0, false, 0x0},
/* 0x0F5 */ {0x0, 0xF, 0x5, false, 0x0},
/* 0x0F3 */ {0x0, 0xF, 0x3, false, 0x0},
/* 0x0F0 */ {0x0, 0xF, 0x0, false, 0x0},
/* 0x0F5 */ {0x0, 0xF, 0x5, false, 0x0},
/* 0x0F4 */ {0x0, 0xF, 0x4, false, 0x0},
/* 0x0F0 */ {0x0, 0xF, 0x0, false, 0x0},
/* 0x0F5 */ {0x0, 0xF, 0x5, false, 0x0},
/* 0x0F1 */ {0x0, 0xF, 0x1, false, 0x0},
/* 0x0F0 */ {0x0, 0xF, 0x0, false, 0x0},
/* 0x0F5 */ {0x0, 0xF, 0x5, false, 0x0},
/* 0x0F2 */ {0x0, 0xF, 0x2, false, 0x0},
/* 0x0F0 */ {0x0, 0xF, 0x0, false, 0x0},
/* 0x0F5 */ {0x0, 0xF, 0x5, false, 0x0},
/* 0x0F3 */ {0x0, 0xF, 0x3, false, 0x0},
/* 0x0F0 */ {0x0, 0xF, 0x0, false, 0x0},
/* 0x0F5 */ {0x0, 0xF, 0x5, false, 0x0},
/* 0x0F4 */ {0x0, 0xF, 0x4, false, 0x0},
/* 0x0F0 */ {0x0, 0xF, 0x0, false, 0x0},
/* 0x0F5 */ {0x0, 0xF, 0x5, false, 0x0},
/* 0x0F1 */ {0x0, 0xF, 0x1, false, 0x0},
/* 0x0F0 */ {0x0, 0xF, 0x0, false, 0x0},
/* 0x0F5 */ {0x0, 0xF, 0x5, false, 0x0},
/* 0x0F2 */ {0x0, 0xF, 0x2, false, 0x0},
/* 0x0F0 */ {0x0, 0xF, 0x0, false, 0x0},
/* 0x0F5 */ {0x0, 0xF, 0x5, false, 0x0},
/* 0x0F3 */ {0x0, 0xF, 0x3, false, 0x0},
/* 0x0F0 */ {0x0, 0xF, 0x0, false, 0x0},
/* 0x0F5 */ {0x0, 0xF, 0x5, false, 0x0},
/* 0x0F4 */ {0x0, 0xF, 0x4, false, 0x0},
/* 0x0F0 */ {0x0, 0xF, 0x0, false, 0x0},
/* 0x0F5 */ {0x0, 0xF, 0x5, false, 0x0},
/* 0x0F0 */ {0x0, 0xF, 0x0, false, 0x0},
/* 0x0F5 */ {0x0, 0xF, 0x5, false, 0x0},
/* 0x0F0 */ {0x0, 0xF, 0x0, false, 0x0},
/* 0x0F5 */ {0x0, 0xF, 0x5, false, 0x0},
/* 0x0F0 */ {0x0, 0xF, 0x0, false, 0x0},
};

/* Port scheduler resource allocation for QCE2204 (7 ports) */
static const struct qce2204_ppe_port_schedule_resource qce2204_ppe_scheduler_res[] = {
	{	/* Port 0: CPU */
		.ucastq_start	= 128,
		.ucastq_end	= 135,
		.mcastq_start	= 256,
		.mcastq_end	= 263,
		.flow_id_start	= 26,
		.flow_id_end	= 28,
		.l0node_start	= 128,
		.l0node_end	= 135,
		.l1node_start	= 26,
		.l1node_end	= 28,
	},
	{	/* Port 1 */
		.ucastq_start	= 0,
		.ucastq_end	= 7,
		.mcastq_start	= 272,
		.mcastq_end	= 275,
		.flow_id_start	= 0,
		.flow_id_end	= 1,
		.l0node_start	= 0,
		.l0node_end	= 7,
		.l1node_start	= 0,
		.l1node_end	= 1,
	},
	{	/* Port 2 */
		.ucastq_start	= 8,
		.ucastq_end	= 15,
		.mcastq_start	= 276,
		.mcastq_end	= 279,
		.flow_id_start	= 2,
		.flow_id_end	= 3,
		.l0node_start	= 8,
		.l0node_end	= 15,
		.l1node_start	= 2,
		.l1node_end	= 3,
	},
	{	/* Port 3 */
		.ucastq_start	= 16,
		.ucastq_end	= 23,
		.mcastq_start	= 280,
		.mcastq_end	= 283,
		.flow_id_start	= 4,
		.flow_id_end	= 5,
		.l0node_start	= 16,
		.l0node_end	= 23,
		.l1node_start	= 4,
		.l1node_end	= 5,
	},
	{	/* Port 4 */
		.ucastq_start	= 24,
		.ucastq_end	= 31,
		.mcastq_start	= 284,
		.mcastq_end	= 287,
		.flow_id_start	= 6,
		.flow_id_end	= 7,
		.l0node_start	= 24,
		.l0node_end	= 31,
		.l1node_start	= 6,
		.l1node_end	= 7,
	},
	{	/* Port 5 */
		.ucastq_start	= 32,
		.ucastq_end	= 39,
		.mcastq_start	= 288,
		.mcastq_end	= 291,
		.flow_id_start	= 8,
		.flow_id_end	= 9,
		.l0node_start	= 32,
		.l0node_end	= 39,
		.l1node_start	= 8,
		.l1node_end	= 9,
	},
};

/* Port scheduler configurations */
static const struct qce2204_ppe_scheduler_port_config qce2204_ppe_port_sch_config[] = {
	{
		.port		= 0,
		.flow_level	= true,
		.node_id	= 26,	 //SP id
		.loop_num	= 2,	 //sp_loop_pri, 2 is enough for htt
		.pri_max	= 1,	 //sp_max_pri
		.flow_id	= 0,	 //port id
		.drr_node_id	= 26,//cdrr edrr
	},
	{
		.port		= 0,
		.flow_level	= false,
		.node_id	= 128,	  //queue id
		.loop_num	= 8,	  //ucast_loop_pri or mcast_loop_pri, 8 for htt
		.pri_max	= 8,	  //fixed(8) or ucast_max_pri
		.flow_id	= 26,	  //SP id
		.drr_node_id	= 128,//cdrr edrr
	},
	{
		.port		= 0,
		.flow_level	= false,
		.node_id	= 256,
		.loop_num	= 8,
		.pri_max	= 8,
		.flow_id	= 26,
		.drr_node_id	= 128,
	},
	{
		.port		= 1,
		.flow_level	= true,
		.node_id	= 0,
		.loop_num	= 1,
		.pri_max	= 0,
		.flow_id	= 1,
		.drr_node_id	= 0,
	},
	{
		.port		= 1,
		.flow_level	= false,
		.node_id	= 0,
		.loop_num	= 8,
		.pri_max	= 8,
		.flow_id	= 0,
		.drr_node_id	= 0,
	},
	{
		.port		= 1,
		.flow_level	= false,
		.node_id	= 272,
		.loop_num	= 4,
		.pri_max	= 8,
		.flow_id	= 0,
		.drr_node_id	= 0,
	},
	{
		.port		= 2,
		.flow_level	= true,
		.node_id	= 2,
		.loop_num	= 1,
		.pri_max	= 0,
		.flow_id	= 2,
		.drr_node_id	= 2,
	},
	{
		.port		= 2,
		.flow_level	= false,
		.node_id	= 8,
		.loop_num	= 8,
		.pri_max	= 8,
		.flow_id	= 2,
		.drr_node_id	= 8,
	},
	{
		.port		= 2,
		.flow_level	= false,
		.node_id	= 276,
		.loop_num	= 4,
		.pri_max	= 8,
		.flow_id	= 2,
		.drr_node_id	= 8,
	},
	{
		.port		= 3,
		.flow_level	= true,
		.node_id	= 4,
		.loop_num	= 1,
		.pri_max	= 0,
		.flow_id	= 3,
		.drr_node_id	= 4,
	},
	{
		.port		= 3,
		.flow_level	= false,
		.node_id	= 16,
		.loop_num	= 8,
		.pri_max	= 8,
		.flow_id	= 4,
		.drr_node_id	= 16,
	},
	{
		.port		= 3,
		.flow_level	= false,
		.node_id	= 280,
		.loop_num	= 4,
		.pri_max	= 8,
		.flow_id	= 4,
		.drr_node_id	= 16,
	},
	{
		.port		= 4,
		.flow_level	= true,
		.node_id	= 6,
		.loop_num	= 1,
		.pri_max	= 0,
		.flow_id	= 4,
		.drr_node_id	= 6,
	},
	{
		.port		= 4,
		.flow_level	= false,
		.node_id	= 24,
		.loop_num	= 8,
		.pri_max	= 8,
		.flow_id	= 6,
		.drr_node_id	= 24,
	},
	{
		.port		= 4,
		.flow_level	= false,
		.node_id	= 284,
		.loop_num	= 4,
		.pri_max	= 8,
		.flow_id	= 6,
		.drr_node_id	= 24,
	},
	{
		.port		= 5,
		.flow_level	= true,
		.node_id	= 8,
		.loop_num	= 1,
		.pri_max	= 0,
		.flow_id	= 5,
		.drr_node_id	= 8,
	},
	{
		.port		= 5,
		.flow_level	= false,
		.node_id	= 32,
		.loop_num	= 8,
		.pri_max	= 8,
		.flow_id	= 8,
		.drr_node_id	= 32,
	},
	{
		.port		= 5,
		.flow_level	= false,
		.node_id	= 288,
		.loop_num	= 4,
		.pri_max	= 8,
		.flow_id	= 8,
		.drr_node_id	= 32,
	},
};

/* Set PPE queue level scheduler configuration */
static int qce2204_ppe_scheduler_l0_queue_map_set(struct qce2204_priv *priv,
						  int node_id, int port,
						  struct qce2204_ppe_scheduler_cfg scheduler_cfg)
{
	u32 val, reg, flow_map_val[4] = {};
	int ret;

	reg = QCE2204_PPE_L0_FLOW_MAP_TBL_ADDR + node_id * QCE2204_PPE_L0_FLOW_MAP_TBL_INC;

	/* Read current values */
	ret = regmap_bulk_read(priv->regmap, reg, flow_map_val, ARRAY_SIZE(flow_map_val));
	if (ret)
		return ret;

	/* Set Word 0 fields */
	val = FIELD_PREP(QCE2204_PPE_L0_FLOW_MAP_TBL_FLOW_ID, scheduler_cfg.flow_id);
	val |= FIELD_PREP(QCE2204_PPE_L0_FLOW_MAP_TBL_C_PRI, scheduler_cfg.pri);
	val |= FIELD_PREP(QCE2204_PPE_L0_FLOW_MAP_TBL_E_PRI, scheduler_cfg.pri);
	val |= FIELD_PREP(QCE2204_PPE_L0_FLOW_MAP_TBL_C_NODE_WT, scheduler_cfg.drr_node_wt);
	val |= FIELD_PREP(QCE2204_PPE_L0_FLOW_MAP_TBL_E_NODE_WT, scheduler_cfg.drr_node_wt);
	flow_map_val[0] = val;

	/* Set Word 1 fields using new macros */
	QCE2204_PPE_L0_FLOW_MAP_SET_C_DRR_ID(flow_map_val, scheduler_cfg.drr_node_id);
	QCE2204_PPE_L0_FLOW_MAP_SET_E_DRR_ID(flow_map_val, scheduler_cfg.drr_node_id);
	QCE2204_PPE_L0_FLOW_MAP_SET_C_DRR_CREDIT_UNIT(flow_map_val, scheduler_cfg.unit_is_packet);
	QCE2204_PPE_L0_FLOW_MAP_SET_E_DRR_CREDIT_UNIT(flow_map_val, scheduler_cfg.unit_is_packet);

	ret = regmap_bulk_write(priv->regmap, reg, flow_map_val, ARRAY_SIZE(flow_map_val));
	if (ret)
		return ret;

	reg = QCE2204_PPE_L0_FLOW_PORT_MAP_TBL_ADDR + node_id * QCE2204_PPE_L0_FLOW_PORT_MAP_TBL_INC;
	val = FIELD_PREP(QCE2204_PPE_L0_FLOW_PORT_MAP_TBL_PORT_NUM, port);

	ret = regmap_write(priv->regmap, reg, val);
	if (ret)
		return ret;

	reg = QCE2204_PPE_L0_COMP_CFG_TBL_ADDR + node_id * QCE2204_PPE_L0_COMP_CFG_TBL_INC;
	val = FIELD_PREP(QCE2204_PPE_L0_COMP_CFG_TBL_NODE_METER_LEN, scheduler_cfg.frame_mode);

	return regmap_update_bits(priv->regmap, reg,
				  QCE2204_PPE_L0_COMP_CFG_TBL_NODE_METER_LEN,
				  val);
}

/* Set PPE flow level scheduler configuration */
static int qce2204_ppe_scheduler_l1_queue_map_set(struct qce2204_priv *priv,
						  int node_id, int port,
						  struct qce2204_ppe_scheduler_cfg scheduler_cfg)
{
	u32 val, reg, flow_map_val[4] = {};
	int ret;

	reg = QCE2204_PPE_L1_FLOW_MAP_TBL_ADDR + node_id * QCE2204_PPE_L1_FLOW_MAP_TBL_INC;

	/* Read current values */
	ret = regmap_bulk_read(priv->regmap, reg, flow_map_val, ARRAY_SIZE(flow_map_val));
	if (ret)
		return ret;

	/* Set Word 0 fields */
	val = FIELD_PREP(QCE2204_PPE_L1_FLOW_MAP_TBL_FLOW_ID, scheduler_cfg.flow_id);
	val |= FIELD_PREP(QCE2204_PPE_L1_FLOW_MAP_TBL_C_PRI, scheduler_cfg.pri);
	val |= FIELD_PREP(QCE2204_PPE_L1_FLOW_MAP_TBL_E_PRI, scheduler_cfg.pri);
	val |= FIELD_PREP(QCE2204_PPE_L1_FLOW_MAP_TBL_C_NODE_WT, scheduler_cfg.drr_node_wt);
	val |= FIELD_PREP(QCE2204_PPE_L1_FLOW_MAP_TBL_E_NODE_WT, scheduler_cfg.drr_node_wt);
	flow_map_val[0] = val;

	/* Set Word 1 fields using new macros */
	QCE2204_PPE_L1_FLOW_MAP_SET_C_DRR_ID_LO(flow_map_val, scheduler_cfg.drr_node_id & 0x3);
	QCE2204_PPE_L1_FLOW_MAP_SET_C_DRR_ID_HI(flow_map_val, (scheduler_cfg.drr_node_id >> 2) & 0xf);
	QCE2204_PPE_L1_FLOW_MAP_SET_E_DRR_ID(flow_map_val, scheduler_cfg.drr_node_id);
	QCE2204_PPE_L1_FLOW_MAP_SET_C_DRR_CREDIT_UNIT(flow_map_val, scheduler_cfg.unit_is_packet);
	QCE2204_PPE_L1_FLOW_MAP_SET_E_DRR_CREDIT_UNIT(flow_map_val, scheduler_cfg.unit_is_packet);

	ret = regmap_bulk_write(priv->regmap, reg, flow_map_val, ARRAY_SIZE(flow_map_val));
	if (ret)
		return ret;

	val = FIELD_PREP(QCE2204_PPE_L1_FLOW_PORT_MAP_TBL_PORT_NUM, port);
	reg = QCE2204_PPE_L1_FLOW_PORT_MAP_TBL_ADDR + node_id * QCE2204_PPE_L1_FLOW_PORT_MAP_TBL_INC;

	ret = regmap_write(priv->regmap, reg, val);
	if (ret)
		return ret;

	reg = QCE2204_PPE_L1_COMP_CFG_TBL_ADDR + node_id * QCE2204_PPE_L1_COMP_CFG_TBL_INC;
	val = FIELD_PREP(QCE2204_PPE_L1_COMP_CFG_TBL_NODE_METER_LEN, scheduler_cfg.frame_mode);

	return regmap_update_bits(priv->regmap, reg, QCE2204_PPE_L1_COMP_CFG_TBL_NODE_METER_LEN, val);
}

/**
 * qce2204_ppe_queue_scheduler_set - Configure scheduler for PPE hardware queue
 * @priv: QCE2204 private data
 * @node_id: PPE queue ID or flow ID
 * @flow_level: Flow level scheduler or queue level scheduler
 * @port: PPE port ID
 * @scheduler_cfg: PPE scheduler configuration
 *
 * Return: 0 on success, negative error code on failure
 */
int qce2204_ppe_queue_scheduler_set(struct qce2204_priv *priv,
				    int node_id, bool flow_level, int port,
				    struct qce2204_ppe_scheduler_cfg scheduler_cfg)
{
	if (flow_level)
		return qce2204_ppe_scheduler_l1_queue_map_set(priv, node_id,
							      port, scheduler_cfg);

	return qce2204_ppe_scheduler_l0_queue_map_set(priv, node_id,
						      port, scheduler_cfg);
}

/**
 * qce2204_ppe_queue_ucast_base_set - Set PPE unicast queue base ID
 * @priv: QCE2204 private data
 * @queue_dst: PPE queue destination configuration
 * @queue_base: PPE queue base ID
 * @profile_id: Profile ID
 *
 * Return: 0 on success, negative error code on failure
 */
int qce2204_ppe_queue_ucast_base_set(struct qce2204_priv *priv,
				     struct qce2204_ppe_queue_ucast_dest queue_dst,
				     int queue_base, int profile_id)
{
	int index, profile_size;
	u32 val, reg;

	profile_size = queue_dst.src_profile << 8;
	if (queue_dst.service_code_en)
		index = QCE2204_PPE_QUEUE_BASE_SERVICE_CODE + profile_size +
			queue_dst.service_code;
	else if (queue_dst.cpu_code_en)
		index = QCE2204_PPE_QUEUE_BASE_CPU_CODE + profile_size +
			queue_dst.cpu_code;
	else
		index = profile_size + queue_dst.dest_port;

	val = FIELD_PREP(QCE2204_PPE_UCAST_QUEUE_MAP_TBL_PROFILE_ID, profile_id);
	val |= FIELD_PREP(QCE2204_PPE_UCAST_QUEUE_MAP_TBL_QUEUE_ID, queue_base);
	reg = QCE2204_PPE_UCAST_QUEUE_MAP_TBL_ADDR + index * QCE2204_PPE_UCAST_QUEUE_MAP_TBL_INC;

	return regmap_write(priv->regmap, reg, val);
}

/**
 * qce2204_ppe_queue_ucast_offset_pri_set - Set PPE unicast queue offset based on priority
 * @priv: QCE2204 private data
 * @profile_id: Profile ID
 * @priority: PPE internal priority
 * @queue_offset: Queue offset
 *
 * Return: 0 on success, negative error code on failure
 */
int qce2204_ppe_queue_ucast_offset_pri_set(struct qce2204_priv *priv,
					   int profile_id,
					   int priority,
					   int queue_offset)
{
	u32 val, reg;
	int index;

	index = (profile_id << 4) + priority;
	val = FIELD_PREP(QCE2204_PPE_UCAST_PRIORITY_MAP_TBL_CLASS, queue_offset);
	reg = QCE2204_PPE_UCAST_PRIORITY_MAP_TBL_ADDR + index * QCE2204_PPE_UCAST_PRIORITY_MAP_TBL_INC;

	return regmap_write(priv->regmap, reg, val);
}

/**
 * qce2204_ppe_queue_ucast_offset_hash_set - Set PPE unicast queue offset based on hash
 * @priv: QCE2204 private data
 * @profile_id: Profile ID
 * @rss_hash: Packet hash value
 * @queue_offset: Queue offset
 *
 * Return: 0 on success, negative error code on failure
 */
int qce2204_ppe_queue_ucast_offset_hash_set(struct qce2204_priv *priv,
					    int profile_id,
					    int rss_hash,
					    int queue_offset)
{
	u32 val, reg;
	int index;

	index = (profile_id << 8) + rss_hash;
	val = FIELD_PREP(QCE2204_PPE_UCAST_HASH_MAP_TBL_HASH, queue_offset);
	reg = QCE2204_PPE_UCAST_HASH_MAP_TBL_ADDR + index * QCE2204_PPE_UCAST_HASH_MAP_TBL_INC;

	return regmap_write(priv->regmap, reg, val);
}

/**
 * qce2204_ppe_port_resource_get - Get PPE resource per port
 * @priv: QCE2204 private data
 * @port: PPE port
 * @type: Resource type
 * @res_start: Resource start ID returned
 * @res_end: Resource end ID returned
 *
 * Return: 0 on success, negative error code on failure
 */
int qce2204_ppe_port_resource_get(struct qce2204_priv *priv, int port,
				  enum qce2204_ppe_resource_type type,
				  int *res_start, int *res_end)
{
	struct qce2204_ppe_port_schedule_resource res;

	if (port >= QCE2204_NUM_PORTS)
		return -EINVAL;

	res = qce2204_ppe_scheduler_res[port];
	switch (type) {
	case QCE2204_PPE_RES_UCAST:
		*res_start = res.ucastq_start;
		*res_end = res.ucastq_end;
		break;
	case QCE2204_PPE_RES_MCAST:
		*res_start = res.mcastq_start;
		*res_end = res.mcastq_end;
		break;
	case QCE2204_PPE_RES_FLOW_ID:
		*res_start = res.flow_id_start;
		*res_end = res.flow_id_end;
		break;
	case QCE2204_PPE_RES_L0_NODE:
		*res_start = res.l0node_start;
		*res_end = res.l0node_end;
		break;
	case QCE2204_PPE_RES_L1_NODE:
		*res_start = res.l1node_start;
		*res_end = res.l1node_end;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

/**
 * qce2204_ppe_sc_config_set - Set PPE service code configuration
 * @priv: QCE2204 private data
 * @sc: Service ID
 * @cfg: Service code configuration
 *
 * Return: 0 on success, negative error code on failure
 */
int qce2204_ppe_sc_config_set(struct qce2204_priv *priv, int sc,
			      struct qce2204_ppe_sc_cfg cfg)
{
	u32 val, reg, servcode_val[2] = {};
	unsigned long bitmap_value;
	int ret;

	val = FIELD_PREP(QCE2204_PPE_IN_L2_SERVICE_TBL_DST_PORT_ID_VALID, cfg.dest_port_valid);
	val |= FIELD_PREP(QCE2204_PPE_IN_L2_SERVICE_TBL_DST_PORT_ID, cfg.dest_port);
	val |= FIELD_PREP(QCE2204_PPE_IN_L2_SERVICE_TBL_DST_DIRECTION, cfg.is_src);

	bitmap_value = bitmap_read(cfg.bitmaps.egress, 0, QCE2204_PPE_SC_BYPASS_EGRESS_SIZE);
	val |= FIELD_PREP(QCE2204_PPE_IN_L2_SERVICE_TBL_DST_BYPASS_BITMAP, bitmap_value);
	val |= FIELD_PREP(QCE2204_PPE_IN_L2_SERVICE_TBL_RX_CNT_EN,
			  test_bit(QCE2204_PPE_SC_BYPASS_COUNTER_RX, cfg.bitmaps.counter));
	val |= FIELD_PREP(QCE2204_PPE_IN_L2_SERVICE_TBL_TX_CNT_EN,
			  test_bit(QCE2204_PPE_SC_BYPASS_COUNTER_TX, cfg.bitmaps.counter));
	reg = QCE2204_PPE_IN_L2_SERVICE_TBL_ADDR + QCE2204_PPE_IN_L2_SERVICE_TBL_INC * sc;

	ret = regmap_write(priv->regmap, reg, val);
	if (ret)
		return ret;

	bitmap_value = bitmap_read(cfg.bitmaps.ingress, 0, QCE2204_PPE_SC_BYPASS_INGRESS_SIZE);
	QCE2204_PPE_SERVICE_SET_BYPASS_BITMAP(servcode_val, bitmap_value);
	QCE2204_PPE_SERVICE_SET_RX_CNT_EN(servcode_val,
					  test_bit(QCE2204_PPE_SC_BYPASS_COUNTER_RX_VLAN, cfg.bitmaps.counter));
	reg = QCE2204_PPE_SERVICE_TBL_ADDR + QCE2204_PPE_SERVICE_TBL_INC * sc;

	ret = regmap_bulk_write(priv->regmap, reg,
				servcode_val, ARRAY_SIZE(servcode_val));
	if (ret)
		return ret;

	reg = QCE2204_PPE_EG_SERVICE_TBL_ADDR + QCE2204_PPE_EG_SERVICE_TBL_INC * sc;
	ret = regmap_bulk_read(priv->regmap, reg,
			       servcode_val, ARRAY_SIZE(servcode_val));
	if (ret)
		return ret;

	QCE2204_PPE_EG_SERVICE_SET_NEXT_SERVCODE(servcode_val, cfg.next_service_code);
	QCE2204_PPE_EG_SERVICE_SET_UPDATE_ACTION(servcode_val, cfg.eip_field_update_bitmap);
	QCE2204_PPE_EG_SERVICE_SET_HW_SERVICE(servcode_val, cfg.eip_hw_service);
	QCE2204_PPE_EG_SERVICE_SET_OFFSET_SEL(servcode_val, cfg.eip_offset_sel);
	QCE2204_PPE_EG_SERVICE_SET_TX_CNT_EN(servcode_val,
					     test_bit(QCE2204_PPE_SC_BYPASS_COUNTER_TX_VLAN, cfg.bitmaps.counter));

	return regmap_bulk_write(priv->regmap, reg,
				servcode_val, ARRAY_SIZE(servcode_val));
#if 0
	bitmap_value = bitmap_read(cfg.bitmaps.tunnel, 0, QCE2204_PPE_SC_BYPASS_TUNNEL_SIZE);
	val = FIELD_PREP(QCE2204_PPE_TL_SERVICE_TBL_BYPASS_BITMAP, bitmap_value);
	reg = QCE2204_PPE_TL_SERVICE_TBL_ADDR + QCE2204_PPE_TL_SERVICE_TBL_INC * sc;

	return regmap_write(priv->regmap, reg, val);
#endif
}

/**
 * qce2204_ppe_counter_enable_set - Set PPE port counter enabled
 * @priv: QCE2204 private data
 * @port: PPE port ID
 *
 * Return: 0 on success, negative error code on failure
 */
int qce2204_ppe_counter_enable_set(struct qce2204_priv *priv, int port)
{
	u32 reg, mru_mtu_val[3];
	int ret;

	reg = QCE2204_PPE_MRU_MTU_CTRL_TBL_ADDR + QCE2204_PPE_MRU_MTU_CTRL_TBL_INC * port;
	ret = regmap_bulk_read(priv->regmap, reg,
			       mru_mtu_val, ARRAY_SIZE(mru_mtu_val));
	if (ret)
		return ret;

	QCE2204_PPE_MRU_MTU_CTRL_SET_RX_CNT_EN(mru_mtu_val, true);
	QCE2204_PPE_MRU_MTU_CTRL_SET_TX_CNT_EN(mru_mtu_val, true);
	ret = regmap_bulk_write(priv->regmap, reg,
				mru_mtu_val, ARRAY_SIZE(mru_mtu_val));
	if (ret)
		return ret;

	reg = QCE2204_PPE_MC_MTU_CTRL_TBL_ADDR + QCE2204_PPE_MC_MTU_CTRL_TBL_INC * port;
	ret = regmap_set_bits(priv->regmap, reg, QCE2204_PPE_MC_MTU_CTRL_TBL_TX_CNT_EN);
	if (ret)
		return ret;

	reg = QCE2204_PPE_PORT_EG_VLAN_TBL_ADDR + QCE2204_PPE_PORT_EG_VLAN_TBL_INC * port;

	return regmap_set_bits(priv->regmap, reg, QCE2204_PPE_PORT_EG_VLAN_TBL_TX_COUNTING_EN);
}

static int qce2204_ppe_rss_hash_ipv4_config(struct qce2204_priv *priv, int index,
					    struct qce2204_ppe_rss_hash_cfg cfg)
{
	u32 reg, val;

	switch (index) {
	case 0:
		val = cfg.hash_sip_mix[0];
		break;
	case 1:
		val = cfg.hash_dip_mix[0];
		break;
	case 2:
		val = cfg.hash_protocol_mix;
		break;
	case 3:
		val = cfg.hash_dport_mix;
		break;
	case 4:
		val = cfg.hash_sport_mix;
		break;
	default:
		return -EINVAL;
	}

	reg = QCE2204_PPE_RSS_HASH_MIX_IPV4_ADDR + index * QCE2204_PPE_RSS_HASH_MIX_IPV4_INC;

	return regmap_update_bits(priv->regmap, reg,
				  QCE2204_PPE_RSS_HASH_MIX_IPV4_VAL,
				  FIELD_PREP(QCE2204_PPE_RSS_HASH_MIX_IPV4_VAL, val));
}

static int qce2204_ppe_rss_hash_ipv6_config(struct qce2204_priv *priv, int index,
					    struct qce2204_ppe_rss_hash_cfg cfg)
{
	u32 reg, val;

	switch (index) {
	case 0 ... 3:
		val = cfg.hash_sip_mix[index];
		break;
	case 4 ... 7:
		val = cfg.hash_dip_mix[index - 4];
		break;
	case 8:
		val = cfg.hash_protocol_mix;
		break;
	case 9:
		val = cfg.hash_dport_mix;
		break;
	case 10:
		val = cfg.hash_sport_mix;
		break;
	default:
		return -EINVAL;
	}

	reg = QCE2204_PPE_RSS_HASH_MIX_ADDR + index * QCE2204_PPE_RSS_HASH_MIX_INC;

	return regmap_update_bits(priv->regmap, reg,
				  QCE2204_PPE_RSS_HASH_MIX_VAL,
				  FIELD_PREP(QCE2204_PPE_RSS_HASH_MIX_VAL, val));
}

/**
 * qce2204_ppe_rss_hash_config_set - Configure PPE hash settings
 * @priv: QCE2204 private data
 * @mode: Configure RSS hash for IPv4 and/or IPv6
 * @cfg: RSS hash configuration
 *
 * Return: 0 on success, negative error code on failure
 */
int qce2204_ppe_rss_hash_config_set(struct qce2204_priv *priv, int mode,
				    struct qce2204_ppe_rss_hash_cfg cfg)
{
	u32 val, reg;
	int i, ret;

	if (mode & QCE2204_PPE_RSS_HASH_MODE_IPV4) {
		val = FIELD_PREP(QCE2204_PPE_RSS_HASH_MASK_IPV4_HASH_MASK, cfg.hash_mask);
		val |= FIELD_PREP(QCE2204_PPE_RSS_HASH_MASK_IPV4_FRAGMENT, cfg.hash_fragment_mode);
		ret = regmap_write(priv->regmap, QCE2204_PPE_RSS_HASH_MASK_IPV4_ADDR, val);
		if (ret)
			return ret;

		val = FIELD_PREP(QCE2204_PPE_RSS_HASH_SEED_IPV4_VAL, cfg.hash_seed);
		ret = regmap_write(priv->regmap, QCE2204_PPE_RSS_HASH_SEED_IPV4_ADDR, val);
		if (ret)
			return ret;

		for (i = 0; i < QCE2204_PPE_RSS_HASH_MIX_IPV4_ENTRIES; i++) {
			ret = qce2204_ppe_rss_hash_ipv4_config(priv, i, cfg);
			if (ret)
				return ret;
		}

		for (i = 0; i < QCE2204_PPE_RSS_HASH_FIN_IPV4_ENTRIES; i++) {
			val = FIELD_PREP(QCE2204_PPE_RSS_HASH_FIN_IPV4_INNER, cfg.hash_fin_inner[i]);
			val |= FIELD_PREP(QCE2204_PPE_RSS_HASH_FIN_IPV4_OUTER, cfg.hash_fin_outer[i]);
			reg = QCE2204_PPE_RSS_HASH_FIN_IPV4_ADDR + i * QCE2204_PPE_RSS_HASH_FIN_IPV4_INC;

			ret = regmap_write(priv->regmap, reg, val);
			if (ret)
				return ret;
		}
	}

	if (mode & QCE2204_PPE_RSS_HASH_MODE_IPV6) {
		val = FIELD_PREP(QCE2204_PPE_RSS_HASH_MASK_HASH_MASK, cfg.hash_mask);
		val |= FIELD_PREP(QCE2204_PPE_RSS_HASH_MASK_FRAGMENT, cfg.hash_fragment_mode);
		ret = regmap_write(priv->regmap, QCE2204_PPE_RSS_HASH_MASK_ADDR, val);
		if (ret)
			return ret;

		val = FIELD_PREP(QCE2204_PPE_RSS_HASH_SEED_VAL, cfg.hash_seed);
		ret = regmap_write(priv->regmap, QCE2204_PPE_RSS_HASH_SEED_ADDR, val);
		if (ret)
			return ret;

		for (i = 0; i < QCE2204_PPE_RSS_HASH_MIX_ENTRIES; i++) {
			ret = qce2204_ppe_rss_hash_ipv6_config(priv, i, cfg);
			if (ret)
				return ret;
		}

		for (i = 0; i < QCE2204_PPE_RSS_HASH_FIN_ENTRIES; i++) {
			val = FIELD_PREP(QCE2204_PPE_RSS_HASH_FIN_INNER, cfg.hash_fin_inner[i]);
			val |= FIELD_PREP(QCE2204_PPE_RSS_HASH_FIN_OUTER, cfg.hash_fin_outer[i]);
			reg = QCE2204_PPE_RSS_HASH_FIN_ADDR + i * QCE2204_PPE_RSS_HASH_FIN_INC;

			ret = regmap_write(priv->regmap, reg, val);
			if (ret)
				return ret;
		}
	}

	return 0;
}


/* Configure BM port threshold */
static int qce2204_ppe_config_bm_threshold(struct qce2204_priv *priv, int bm_port_id,
					   const struct qce2204_ppe_bm_port_config port_cfg)
{
	u32 reg, val, bm_fc_val[2];
	int ret;

	reg = QCE2204_PPE_BM_PORT_FC_CFG_TBL_ADDR + QCE2204_PPE_BM_PORT_FC_CFG_TBL_INC * bm_port_id;
	ret = regmap_bulk_read(priv->regmap, reg,
			       bm_fc_val, ARRAY_SIZE(bm_fc_val));
	if (ret)
		return ret;

	/* Configure BM flow control related threshold */
	QCE2204_PPE_BM_PORT_FC_SET_WEIGHT(bm_fc_val, port_cfg.weight);
	QCE2204_PPE_BM_PORT_FC_SET_RESUME_OFFSET(bm_fc_val, port_cfg.resume_offset);
	QCE2204_PPE_BM_PORT_FC_SET_RESUME_THRESHOLD(bm_fc_val, port_cfg.resume_ceil);
	QCE2204_PPE_BM_PORT_FC_SET_DYNAMIC(bm_fc_val, port_cfg.dynamic);
	QCE2204_PPE_BM_PORT_FC_SET_REACT_LIMIT(bm_fc_val, port_cfg.in_fly_buf);
	QCE2204_PPE_BM_PORT_FC_SET_PRE_ALLOC(bm_fc_val, port_cfg.pre_alloc);

	/* Configure low/high bits of ceiling */
	val = FIELD_GET(BIT(0), port_cfg.ceil);
	QCE2204_PPE_BM_PORT_FC_SET_CEILING_LOW(bm_fc_val, val);
	val = FIELD_GET(GENMASK(11, 1), port_cfg.ceil);
	QCE2204_PPE_BM_PORT_FC_SET_CEILING_HIGH(bm_fc_val, val);

	ret = regmap_bulk_write(priv->regmap, reg,
				bm_fc_val, ARRAY_SIZE(bm_fc_val));
	if (ret)
		return ret;

	/* Assign default group ID 0 to BM port */
	val = FIELD_PREP(QCE2204_PPE_BM_PORT_GROUP_ID_SHARED_GROUP_ID, 0);
	reg = QCE2204_PPE_BM_PORT_GROUP_ID_ADDR + QCE2204_PPE_BM_PORT_GROUP_ID_INC * bm_port_id;
	ret = regmap_update_bits(priv->regmap, reg,
				 QCE2204_PPE_BM_PORT_GROUP_ID_SHARED_GROUP_ID,
				 val);
	if (ret)
		return ret;

	/* Disable BM port flow control */
	reg = QCE2204_PPE_BM_PORT_FC_MODE_ADDR + QCE2204_PPE_BM_PORT_FC_MODE_INC * bm_port_id;

	return regmap_clear_bits(priv->regmap, reg, QCE2204_PPE_BM_PORT_FC_MODE_EN);
}

/* Configure buffer management */
static int qce2204_ppe_config_bm(struct qce2204_priv *priv)
{
	const struct qce2204_ppe_bm_port_config *port_cfg;
	unsigned int i, bm_port_id, port_cfg_cnt;
	u32 reg, val;
	int ret;

	/* Configure allocated buffer number for group 0 */
	reg = QCE2204_PPE_BM_SHARED_GROUP_CFG_ADDR;
	val = FIELD_PREP(QCE2204_PPE_BM_SHARED_GROUP_CFG_SHARED_LIMIT,
			 qce2204_ppe_bm_group_config);
	ret = regmap_update_bits(priv->regmap, reg,
				 QCE2204_PPE_BM_SHARED_GROUP_CFG_SHARED_LIMIT,
				 val);
	if (ret)
		goto bm_config_fail;

	/* Configure buffer thresholds for BM ports */
	port_cfg = qce2204_ppe_bm_port_config;
	port_cfg_cnt = ARRAY_SIZE(qce2204_ppe_bm_port_config);
	for (i = 0; i < port_cfg_cnt; i++) {
		for (bm_port_id = port_cfg[i].port_id_start;
		     bm_port_id <= port_cfg[i].port_id_end; bm_port_id++) {
			ret = qce2204_ppe_config_bm_threshold(priv, bm_port_id,
							      port_cfg[i]);
			if (ret)
				goto bm_config_fail;
		}
	}

	return 0;

bm_config_fail:
	dev_err(priv->dev, "PPE BM config error %d\n", ret);
	return ret;
}

/* Configure queue management */
static int qce2204_ppe_config_qm(struct qce2204_priv *priv)
{
	const struct qce2204_ppe_qm_queue_config *queue_cfg;
	int ret, i, queue_id, queue_cfg_count;
	u32 reg, multicast_queue_cfg[3];
	u32 unicast_queue_cfg[8];
	u32 group_cfg[4];

	/* Assign buffer number to group 0 */
	reg = QCE2204_PPE_AC_GRP_CFG_TBL_ADDR;
	ret = regmap_bulk_read(priv->regmap, reg,
			       group_cfg, ARRAY_SIZE(group_cfg));
	if (ret)
		goto qm_config_fail;

	QCE2204_PPE_AC_GRP_SET_BUF_LIMIT(group_cfg, qce2204_ppe_qm_group_config);

	ret = regmap_bulk_write(priv->regmap, reg,
				group_cfg, ARRAY_SIZE(group_cfg));
	if (ret)
		goto qm_config_fail;

	queue_cfg = qce2204_ppe_qm_queue_config;
	queue_cfg_count = ARRAY_SIZE(qce2204_ppe_qm_queue_config);
	for (i = 0; i < queue_cfg_count; i++) {
		queue_id = queue_cfg[i].queue_start;

		while (queue_id <= queue_cfg[i].queue_end) {
			if (queue_id < 256) {
				/* Unicast queue */
				reg = QCE2204_PPE_AC_UNICAST_QUEUE_CFG_TBL_ADDR +
				      QCE2204_PPE_AC_UNICAST_QUEUE_CFG_TBL_INC * queue_id;

				ret = regmap_bulk_read(priv->regmap, reg,
						       unicast_queue_cfg,
						       ARRAY_SIZE(unicast_queue_cfg));
				if (ret)
					goto qm_config_fail;

				QCE2204_PPE_AC_UNICAST_QUEUE_SET_EN(unicast_queue_cfg, true);
				QCE2204_PPE_AC_UNICAST_QUEUE_SET_GRP_ID(unicast_queue_cfg, 0);
				QCE2204_PPE_AC_UNICAST_QUEUE_SET_PRE_LIMIT(unicast_queue_cfg,
									   queue_cfg[i].prealloc_buf);
				QCE2204_PPE_AC_UNICAST_QUEUE_SET_DYNAMIC(unicast_queue_cfg,
									 queue_cfg[i].dynamic);
				QCE2204_PPE_AC_UNICAST_QUEUE_SET_WEIGHT(unicast_queue_cfg,
									queue_cfg[i].weight);
				QCE2204_PPE_AC_UNICAST_QUEUE_SET_THRESHOLD_LO(unicast_queue_cfg,
									   queue_cfg[i].ceil & 0x3FF);
				QCE2204_PPE_AC_UNICAST_QUEUE_SET_THRESHOLD_HI(unicast_queue_cfg,
									   (queue_cfg[i].ceil >> 10) & 0x3);
				QCE2204_PPE_AC_UNICAST_QUEUE_SET_GRN_RESUME_LO(unicast_queue_cfg,
									    queue_cfg[i].resume_offset & 0x3FF);
				QCE2204_PPE_AC_UNICAST_QUEUE_SET_GRN_RESUME_HI(unicast_queue_cfg,
									    (queue_cfg[i].resume_offset >> 10) & 0x3);

				ret = regmap_bulk_write(priv->regmap, reg,
							unicast_queue_cfg,
							ARRAY_SIZE(unicast_queue_cfg));
				if (ret)
					goto qm_config_fail;
			} else {
				/* Multicast queue */
				reg = QCE2204_PPE_AC_MULTICAST_QUEUE_CFG_TBL_ADDR +
				      QCE2204_PPE_AC_MULTICAST_QUEUE_CFG_TBL_INC * (queue_id - 256);

				ret = regmap_bulk_read(priv->regmap, reg,
						       multicast_queue_cfg,
						       ARRAY_SIZE(multicast_queue_cfg));
				if (ret)
					goto qm_config_fail;

				QCE2204_PPE_AC_MULTICAST_QUEUE_SET_EN(multicast_queue_cfg, true);
				QCE2204_PPE_AC_MULTICAST_QUEUE_SET_GRN_GRP_ID(multicast_queue_cfg, 0);
				QCE2204_PPE_AC_MULTICAST_QUEUE_SET_GRN_PRE_LIMIT(multicast_queue_cfg,
										 queue_cfg[i].prealloc_buf);
				QCE2204_PPE_AC_MULTICAST_QUEUE_SET_GRN_THRESHOLD(multicast_queue_cfg,
										 queue_cfg[i].ceil);
				QCE2204_PPE_AC_MULTICAST_QUEUE_SET_GRN_RESUME(multicast_queue_cfg,
									      queue_cfg[i].resume_offset);

				ret = regmap_bulk_write(priv->regmap, reg,
							multicast_queue_cfg,
							ARRAY_SIZE(multicast_queue_cfg));
				if (ret)
					goto qm_config_fail;
			}

			/* Enable enqueue */
			reg = QCE2204_PPE_ENQ_OPR_TBL_ADDR + QCE2204_PPE_ENQ_OPR_TBL_INC * queue_id;
			ret = regmap_clear_bits(priv->regmap, reg,
						QCE2204_PPE_ENQ_OPR_TBL_ENQ_DISABLE);
			if (ret)
				goto qm_config_fail;

			/* Enable dequeue */
			reg = QCE2204_PPE_DEQ_OPR_TBL_ADDR + QCE2204_PPE_DEQ_OPR_TBL_INC * queue_id;
			ret = regmap_clear_bits(priv->regmap, reg,
						QCE2204_PPE_DEQ_OPR_TBL_DEQ_DISABLE);
			if (ret)
				goto qm_config_fail;

			queue_id++;
		}
	}

	/* Enable queue counter */
	ret = regmap_set_bits(priv->regmap, QCE2204_PPE_EG_BRIDGE_CONFIG_ADDR,
			      QCE2204_PPE_EG_BRIDGE_CONFIG_QUEUE_CNT_EN);
	if (ret)
		goto qm_config_fail;

	/* Disable ucast enqueue for mcast traffic */
	ret = regmap_clear_bits(priv->regmap, QCE2204_PPE_MC_ENQ_CTRL_ADDR,
			      QCE2204_PPE_MC_ENQ_CTRL_UC_ENQ_EN);
	if (ret)
		goto qm_config_fail;

	return 0;

qm_config_fail:
	dev_err(priv->dev, "PPE QM config error %d\n", ret);
	return ret;
}

/* Configure node scheduler */
static int qce2204_ppe_node_scheduler_config(struct qce2204_priv *priv,
					     const struct qce2204_ppe_scheduler_port_config config)
{
	struct qce2204_ppe_scheduler_cfg sch_cfg;
	int ret, i;

	for (i = 0; i < config.loop_num; i++) {
		if (!config.pri_max) {
			/* Round robin scheduler without priority */
			sch_cfg.flow_id = config.flow_id;
			sch_cfg.pri = 0;
			sch_cfg.drr_node_id = config.drr_node_id;
		} else {
			sch_cfg.flow_id = config.flow_id +
				((config.flow_level) ? (0) : (i / config.pri_max));
			sch_cfg.pri = i % config.pri_max;
			sch_cfg.drr_node_id = config.drr_node_id +
				((config.flow_level) ? (i % config.pri_max) : (i));
		}

		/* Scheduler weight, must be more than 0 */
		sch_cfg.drr_node_wt = 1;
		/* Byte based scheduling */
		sch_cfg.unit_is_packet = false;
		/* Frame + CRC calculated */
		sch_cfg.frame_mode = QCE2204_PPE_SCH_WITH_IPG_PREAMBLE_FRAME_CRC;

		ret = qce2204_ppe_queue_scheduler_set(priv, config.node_id + i,
						      config.flow_level,
						      config.port,
						      sch_cfg);
		if (ret)
			return ret;
	}

	return 0;
}

/* Configure TDM scheduler */
static int qce2204_ppe_config_scheduler(struct qce2204_priv *priv)
{
	const struct qce2204_ppe_scheduler_port_config *port_cfg;
	const struct qce2204_ppe_scheduler_qm_config *qm_cfg;
	const struct qce2204_ppe_scheduler_bm_config *bm_cfg;
	int ret, i, count;
	u32 val, reg;

	count = ARRAY_SIZE(qce2204_ppe_sch_bm_config);
	bm_cfg = qce2204_ppe_sch_bm_config;

	/* Configure each BM scheduler entry */
	for (i = 0; i < count; i++) {
		val = FIELD_PREP(QCE2204_PPE_BM_SCH_CFG_TBL_VALID, bm_cfg[i].valid);
		val |= FIELD_PREP(QCE2204_PPE_BM_SCH_CFG_TBL_DIR, bm_cfg[i].dir);
		val |= FIELD_PREP(QCE2204_PPE_BM_SCH_CFG_TBL_PORT_NUM, bm_cfg[i].port);
		val |= FIELD_PREP(QCE2204_PPE_BM_SCH_CFG_TBL_SECOND_PORT_VALID,
				  bm_cfg[i].backup_port_valid);
		val |= FIELD_PREP(QCE2204_PPE_BM_SCH_CFG_TBL_SECOND_PORT,
				  bm_cfg[i].backup_port);

		reg = QCE2204_PPE_BM_SCH_CFG_TBL_ADDR + i * QCE2204_PPE_BM_SCH_CFG_TBL_INC;
		ret = regmap_write(priv->regmap, reg, val);
		if (ret)
			goto sch_config_fail;
	}

	count = ARRAY_SIZE(qce2204_ppe_sch_qm_config);
	qm_cfg = qce2204_ppe_sch_qm_config;

	/* Configure each QM scheduler entry */
	for (i = 0; i < count; i++) {
		val = FIELD_PREP(QCE2204_PPE_PSCH_SCH_CFG_TBL_ENS_PORT_BITMAP,
				 qm_cfg[i].ensch_port_bmp);
		val |= FIELD_PREP(QCE2204_PPE_PSCH_SCH_CFG_TBL_ENS_PORT,
				  qm_cfg[i].ensch_port);
		val |= FIELD_PREP(QCE2204_PPE_PSCH_SCH_CFG_TBL_DES_PORT,
				  qm_cfg[i].desch_port);
		val |= FIELD_PREP(QCE2204_PPE_PSCH_SCH_CFG_TBL_DES_SECOND_PORT_EN,
				  qm_cfg[i].desch_backup_port_valid);
		val |= FIELD_PREP(QCE2204_PPE_PSCH_SCH_CFG_TBL_DES_SECOND_PORT,
				  qm_cfg[i].desch_backup_port);

		reg = QCE2204_PPE_PSCH_SCH_CFG_TBL_ADDR + i * QCE2204_PPE_PSCH_SCH_CFG_TBL_INC;
		ret = regmap_write(priv->regmap, reg, val);
		if (ret)
			goto sch_config_fail;
	}

	count = ARRAY_SIZE(qce2204_ppe_port_sch_config);
	port_cfg = qce2204_ppe_port_sch_config;

	/* Configure scheduler per PPE queue or flow */
	for (i = 0; i < count; i++) {
		if (port_cfg[i].port >= QCE2204_NUM_PORTS)
			break;

		ret = qce2204_ppe_node_scheduler_config(priv, port_cfg[i]);
		if (ret)
			goto sch_config_fail;
	}

	return 0;

sch_config_fail:
	dev_err(priv->dev, "PPE scheduler config error %d\n", ret);
	return ret;
}

/* Initialize queue destinations */
static int qce2204_ppe_queue_dest_init(struct qce2204_priv *priv)
{
	int ret, port_id, index, q_base, q_offset, res_start, res_end, pri_max;
	struct qce2204_ppe_queue_ucast_dest queue_dst;

	for (port_id = 0; port_id < QCE2204_NUM_PORTS; port_id++) {
		memset(&queue_dst, 0, sizeof(queue_dst));

		ret = qce2204_ppe_port_resource_get(priv, port_id, QCE2204_PPE_RES_UCAST,
						    &res_start, &res_end);
		if (ret)
			return ret;

		q_base = res_start;
		queue_dst.dest_port = port_id;

		/* Configure queue base ID and profile ID */
		ret = qce2204_ppe_queue_ucast_base_set(priv, queue_dst,
						       q_base, port_id);
		if (ret)
			return ret;

		/* Queue priority range */
		ret = qce2204_ppe_port_resource_get(priv, port_id, QCE2204_PPE_RES_L0_NODE,
						    &res_start, &res_end);
		if (ret)
			return ret;

		pri_max = res_end - res_start;

		if (port_id == 0) {
			/* Initialize the queue base for all CPU codes */
			memset(&queue_dst, 0, sizeof(queue_dst));
			queue_dst.cpu_code_en = true;
			for (index = 0; index < QCE2204_PPE_CPU_CODE_NUM; index++) {
				queue_dst.cpu_code = index;
				ret = qce2204_ppe_queue_ucast_base_set(priv, queue_dst,
									   q_base, 0);
				if (ret)
					return ret;
			}

			/* Redirect ARP reply with max priority on CPU port */
			memset(&queue_dst, 0, sizeof(queue_dst));
			queue_dst.cpu_code_en = true;
			queue_dst.cpu_code = 101;
			ret = qce2204_ppe_queue_ucast_base_set(priv, queue_dst,
							       q_base + pri_max,
							       0);
			if (ret)
				return ret;
		}

		/* Initialize queue offset of internal priority */
		for (index = 0; index < QCE2204_PPE_QUEUE_INTER_PRI_NUM; index++) {
			q_offset = index > pri_max ? pri_max : index;

			ret = qce2204_ppe_queue_ucast_offset_pri_set(priv, port_id,
								     index, q_offset);
			if (ret)
				return ret;
		}

		/* Initialize queue offset of RSS hash as 0 */
		for (index = 0; index < QCE2204_PPE_QUEUE_HASH_NUM; index++) {
			ret = qce2204_ppe_queue_ucast_offset_hash_set(priv, port_id,
								      index, 0);
			if (ret)
				return ret;
		}
	}

	return 0;
}

/* Initialize service code */
static int __maybe_unused qce2204_ppe_servcode_init(struct qce2204_priv *priv)
{
	struct qce2204_ppe_sc_cfg sc_cfg = {};

	bitmap_zero(sc_cfg.bitmaps.counter, QCE2204_PPE_SC_BYPASS_COUNTER_SIZE);
	bitmap_zero(sc_cfg.bitmaps.tunnel, QCE2204_PPE_SC_BYPASS_TUNNEL_SIZE);

	bitmap_fill(sc_cfg.bitmaps.ingress, QCE2204_PPE_SC_BYPASS_INGRESS_SIZE);
	clear_bit(QCE2204_PPE_SC_BYPASS_INGRESS_FAKE_MAC_HEADER, sc_cfg.bitmaps.ingress);
	clear_bit(QCE2204_PPE_SC_BYPASS_INGRESS_SERVICE_CODE, sc_cfg.bitmaps.ingress);
	clear_bit(QCE2204_PPE_SC_BYPASS_INGRESS_FAKE_L2_PROTO, sc_cfg.bitmaps.ingress);

	bitmap_fill(sc_cfg.bitmaps.egress, QCE2204_PPE_SC_BYPASS_EGRESS_SIZE);
	clear_bit(QCE2204_PPE_SC_BYPASS_EGRESS_ACL_POST_ROUTING_CHECK, sc_cfg.bitmaps.egress);

	return qce2204_ppe_sc_config_set(priv, QCE2204_PPE_EDMA_SC_BYPASS_ID, sc_cfg);
}

/* Initialize port configurations */
static int qce2204_ppe_port_config_init(struct qce2204_priv *priv)
{
	u32 reg, val, mru_mtu_val[3];
	int i, ret;

	/* Enable ports counter */
	for (i = 0; i < QCE2204_NUM_PORTS; i++) {
		ret = qce2204_ppe_counter_enable_set(priv, i);
		if (ret)
			return ret;
	}

	/* CPU port 0 MTU and MRU set to max framesize, ethernet ports MTU/MRC set by mtu ops */
	reg = QCE2204_PPE_MRU_MTU_CTRL_TBL_ADDR + QCE2204_PPE_MRU_MTU_CTRL_TBL_INC * QCE2204_CPU_PORT_ID;
	ret = regmap_bulk_read(priv->regmap, reg,
				   mru_mtu_val, ARRAY_SIZE(mru_mtu_val));
	if (ret)
		return ret;

	/* Redirect packet to CPU when size > MTU/MRU */
	QCE2204_PPE_MRU_MTU_CTRL_SET_MRU(mru_mtu_val, QCE2204_MAX_FRAME_SIZE);
	QCE2204_PPE_MRU_MTU_CTRL_SET_MRU_CMD(mru_mtu_val, QCE2204_PPE_ACTION_REDIRECT_TO_CPU);
	QCE2204_PPE_MRU_MTU_CTRL_SET_MTU(mru_mtu_val, QCE2204_MAX_FRAME_SIZE);
	QCE2204_PPE_MRU_MTU_CTRL_SET_MTU_CMD(mru_mtu_val, QCE2204_PPE_ACTION_REDIRECT_TO_CPU);
	ret = regmap_bulk_write(priv->regmap, reg,
				mru_mtu_val, ARRAY_SIZE(mru_mtu_val));
	if (ret)
		return ret;

	reg = QCE2204_PPE_MC_MTU_CTRL_TBL_ADDR + QCE2204_PPE_MC_MTU_CTRL_TBL_INC * QCE2204_CPU_PORT_ID;
	val = FIELD_PREP(QCE2204_PPE_MC_MTU_CTRL_TBL_MTU_CMD, QCE2204_PPE_ACTION_REDIRECT_TO_CPU);
	return regmap_update_bits(priv->regmap, reg,
				 QCE2204_PPE_MC_MTU_CTRL_TBL_MTU_CMD,
				 val);
}

/* Initialize RSS hash */
static int qce2204_ppe_rss_hash_init(struct qce2204_priv *priv)
{
	u16 fins[QCE2204_PPE_RSS_HASH_TUPLES] = { 0x205, 0x264, 0x227, 0x245, 0x201 };
	u8 ips[QCE2204_PPE_RSS_HASH_IP_LENGTH] = { 0x13, 0xb, 0x13, 0xb };
	struct qce2204_ppe_rss_hash_cfg hash_cfg;
	int i, ret;

	hash_cfg.hash_seed = get_random_u32();
	hash_cfg.hash_mask = 0xfff;
	hash_cfg.hash_fragment_mode = false;

	/* Final common seed configs */
	for (i = 0; i < ARRAY_SIZE(fins); i++) {
		hash_cfg.hash_fin_inner[i] = fins[i] & 0x1f;
		hash_cfg.hash_fin_outer[i] = fins[i] >> 5;
	}

	/* RSS seeds for protocol, ports, and IPs */
	hash_cfg.hash_protocol_mix = 0x13;
	hash_cfg.hash_dport_mix = 0xb;
	hash_cfg.hash_sport_mix = 0x13;
	hash_cfg.hash_dip_mix[0] = 0xb;
	hash_cfg.hash_sip_mix[0] = 0x13;

	/* Configure RSS for IPv4 */
	ret = qce2204_ppe_rss_hash_config_set(priv, QCE2204_PPE_RSS_HASH_MODE_IPV4, hash_cfg);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(ips); i++) {
		hash_cfg.hash_sip_mix[i] = ips[i];
		hash_cfg.hash_dip_mix[i] = ips[i];
	}

	/* Configure RSS for IPv6 */
	return qce2204_ppe_rss_hash_config_set(priv, QCE2204_PPE_RSS_HASH_MODE_IPV6, hash_cfg);
}

/* Initialize bridge */
static int qce2204_ppe_bridge_init(struct qce2204_priv *priv)
{
	struct dsa_switch *ds = priv->ds;
	u32 reg, mask, bridge_cfg, port_cfg[4], vsi_cfg[2];
	u8 user_ports_mask = dsa_user_ports(ds);
	u8 cpu_ports_mask = dsa_cpu_ports(ds);
	int ret, i;

	for (i = 0; i < QCE2204_NUM_PORTS; i++) {
		/* Configure port bridge control register */
		reg = QCE2204_PPE_PORT_BRIDGE_CTRL_ADDR + QCE2204_PPE_PORT_BRIDGE_CTRL_INC * i;

		bridge_cfg = (i == priv->cpu_port) ?
			FIELD_PREP(QCE2204_PPE_PORT_BRIDGE_ISOL_BITMAP, user_ports_mask) :
			FIELD_PREP(QCE2204_PPE_PORT_BRIDGE_ISOL_BITMAP, (user_ports_mask & ~BIT(i)) | cpu_ports_mask);
		bridge_cfg |= QCE2204_PPE_PORT_BRIDGE_TXMAC_EN;

		mask = QCE2204_PPE_PORT_BRIDGE_ISOL_BITMAP | QCE2204_PPE_PORT_BRIDGE_TXMAC_EN;
		ret = regmap_update_bits(priv->regmap, reg, mask, bridge_cfg);
		if (ret)
			return ret;

		/* Enable invalid VSI forwarding for physical ports to CPU */
		if (i == 0)
			continue;

		reg = QCE2204_PPE_L2_VP_PORT_TBL_ADDR + QCE2204_PPE_L2_VP_PORT_TBL_INC * i;
		ret = regmap_bulk_read(priv->regmap, reg,
				       port_cfg, ARRAY_SIZE(port_cfg));
		if (ret)
			return ret;

		QCE2204_PPE_L2_PORT_SET_INVALID_VSI_FWD_EN(port_cfg, true);
		QCE2204_PPE_L2_PORT_SET_DST_INFO(port_cfg, 0);

		ret = regmap_bulk_write(priv->regmap, reg,
					port_cfg, ARRAY_SIZE(port_cfg));
		if (ret)
			return ret;
	}

	for (i = 0; i < QCE2204_PPE_VSI_TBL_ENTRIES_NUM; i++) {
		/* Set VSI forward membership to include only CPU port0 */
		reg = QCE2204_PPE_VSI_TBL_ADDR + QCE2204_PPE_VSI_TBL_INC * i;
		ret = regmap_bulk_read(priv->regmap, reg,
				       vsi_cfg, ARRAY_SIZE(vsi_cfg));
		if (ret)
			return ret;

		QCE2204_PPE_VSI_SET_MEMBER_PORT_BITMAP(vsi_cfg, BIT(0));
		QCE2204_PPE_VSI_SET_UUC_BITMAP(vsi_cfg, BIT(0));
		QCE2204_PPE_VSI_SET_UMC_BITMAP(vsi_cfg, BIT(0));
		QCE2204_PPE_VSI_SET_BC_BITMAP_LO(vsi_cfg, BIT(0));
		QCE2204_PPE_VSI_SET_NEW_ADDR_LRN_EN(vsi_cfg, true);
		QCE2204_PPE_VSI_SET_NEW_ADDR_FWD_CMD(vsi_cfg, QCE2204_PPE_ACTION_FORWARD);
		QCE2204_PPE_VSI_SET_STATION_MOVE_LRN_EN(vsi_cfg, true);
		QCE2204_PPE_VSI_SET_STATION_MOVE_FWD_CMD(vsi_cfg, QCE2204_PPE_ACTION_FORWARD);

		ret = regmap_bulk_write(priv->regmap, reg,
					vsi_cfg, ARRAY_SIZE(vsi_cfg));
		if (ret)
			return ret;
	}

	/* Enable edit cpy/rdt cpu pkt */
	return regmap_set_bits(priv->regmap, QCE2204_PPE_EG_BRIDGE_CONFIG_ADDR,
			      QCE2204_PPE_EG_BRIDGE_CONFIG_PKT_L2_EDIT_EN);
}

/**
 * qce2204_ppe_vsi_member_set - Configure VSI member ports
 * @priv: Driver private data
 * @vsi_id: VSI index
 * @cfg: Configuration structure
 *
 * Return: 0 on success, negative error code on failure
 */
int qce2204_ppe_vsi_member_set(struct qce2204_priv *priv,
				u32 vsi_id,
				struct qce2204_ppe_vsi_member_cfg *cfg)
{
	u32 reg_addr;
	u32 tbl_data[QCE2204_PPE_VSI_TBL_INC / 4];
	int ret;

	if (!priv || !cfg)
		return -EINVAL;

	if (vsi_id >= QCE2204_PPE_VSI_TBL_ENTRIES)
		return -EINVAL;

	reg_addr = QCE2204_PPE_VSI_TBL_ADDR +
		   vsi_id * QCE2204_PPE_VSI_TBL_INC;

	ret = regmap_bulk_read(priv->regmap, reg_addr, tbl_data,
			       QCE2204_PPE_VSI_TBL_INC / 4);
	if (ret)
		return ret;

	/* Update fields */
	QCE2204_PPE_VSI_SET_MEMBER_PORT_BITMAP(tbl_data, cfg->member_port_bitmap);
	QCE2204_PPE_VSI_SET_UUC_BITMAP(tbl_data, cfg->uuc_bitmap);
	QCE2204_PPE_VSI_SET_UMC_BITMAP(tbl_data, cfg->umc_bitmap);
	QCE2204_PPE_VSI_SET_BC_BITMAP_LO(tbl_data, cfg->bc_bitmap & 0x1f);
	QCE2204_PPE_VSI_SET_BC_BITMAP_HI(tbl_data, (cfg->bc_bitmap >> 5) & 0xf);

	return regmap_bulk_write(priv->regmap, reg_addr, tbl_data,
				 QCE2204_PPE_VSI_TBL_INC / 4);
}

/**
 * qce2204_ppe_port_vsi_set - Configure VSI for port
 * @priv: Driver private data
 * @port_id: Port ID
 * @cfg: Port VSI configuration
 *
 * Configures VSI (Virtual Switch Instance) settings for port.
 * This allows binding a port to a specific VSI for L3 forwarding.
 *
 * Return: 0 on success, negative error code on failure
 */
int qce2204_ppe_port_vsi_set(struct qce2204_priv *priv,
			      u32 port_id,
			      struct qce2204_ppe_port_vsi_cfg *cfg)
{
	u32 reg_addr;
	u32 tbl_data[QCE2204_PPE_L3_VP_PORT_TBL_INC / 4];
	int ret;

	if (!priv || !cfg)
		return -EINVAL;

	if (port_id >= QCE2204_PPE_L3_VP_PORT_TBL_ENTRIES)
		return -EINVAL;

	reg_addr = QCE2204_PPE_L3_VP_PORT_TBL_ADDR +
		   port_id * QCE2204_PPE_L3_VP_PORT_TBL_INC;

	ret = regmap_bulk_read(priv->regmap, reg_addr, tbl_data,
			       QCE2204_PPE_L3_VP_PORT_TBL_INC / 4);
	if (ret)
		return ret;

	/* Update VSI fields using SET macros */
	QCE2204_PPE_L3_VP_PORT_SET_VSI_VALID(tbl_data, cfg->vsi_valid ? 1 : 0);
	QCE2204_PPE_L3_VP_PORT_SET_VSI(tbl_data, cfg->vsi);

	return regmap_bulk_write(priv->regmap, reg_addr, tbl_data,
				 QCE2204_PPE_L3_VP_PORT_TBL_INC / 4);
}

/**
 * qce2204_setup_none_tag_vsi - Setup VSI configuration for none tag mode
 * @priv: QCE2204 private data
 *
 * Configures VSI settings for none tag mode:
 * 1. Set all ports (CPU + user ports) to use default VSI
 * 2. Configure default VSI member ports to include all user and CPU ports
 * 3. Configure default VSI unknown/broadcast traffic to CPU ports only
 *
 * Return: 0 on success, negative error code on failure
 */
int qce2204_setup_none_tag_vsi(struct qce2204_priv *priv)
{
	struct qce2204_ppe_port_vsi_cfg port_vsi_cfg = {};
	struct qce2204_ppe_vsi_member_cfg vsi_member_cfg = {};
	struct dsa_switch *ds = priv->ds;
	u8 user_ports_mask = dsa_user_ports(ds);
	u8 cpu_ports_mask = dsa_cpu_ports(ds);
	struct dsa_port *dp;
	int ret;

	dev_info(priv->dev, "Setting up none tag VSI configuration\n");

	/* Step 1: Set all ports to use default VSI */
	port_vsi_cfg.vsi_valid = true;
	port_vsi_cfg.vsi = QCE2204_DEFAULT_VSI;

	dsa_switch_for_each_available_port(dp, ds) {
		ret = qce2204_ppe_port_vsi_set(priv, dp->index, &port_vsi_cfg);
		if (ret) {
			dev_err(priv->dev, "Failed to set VSI for port %d: %d\n",
				dp->index, ret);
			goto cleanup;
		}
	}

	dev_dbg(priv->dev, "User ports mask: 0x%x, CPU ports mask: 0x%x\n",
		user_ports_mask, cpu_ports_mask);

	/* Step 2: Configure default VSI membership */
	vsi_member_cfg.member_port_bitmap = user_ports_mask | cpu_ports_mask;
	vsi_member_cfg.uuc_bitmap = user_ports_mask | cpu_ports_mask;
	vsi_member_cfg.umc_bitmap = user_ports_mask | cpu_ports_mask;
	vsi_member_cfg.bc_bitmap = user_ports_mask | cpu_ports_mask;

	ret = qce2204_ppe_vsi_member_set(priv, QCE2204_DEFAULT_VSI, &vsi_member_cfg);
	if (ret) {
		dev_err(priv->dev, "Failed to set default VSI membership: %d\n", ret);
		goto cleanup;
	}

	dev_info(priv->dev, "None tag VSI configuration completed\n");
	return 0;

cleanup:
	/* Attempt to clean up on error */
	qce2204_teardown_none_tag_vsi(priv);
	return ret;
}

/**
 * qce2204_teardown_none_tag_vsi - Teardown VSI configuration for none tag mode
 * @priv: QCE2204 private data
 *
 * Restores VSI settings by:
 * 1. Clearing VSI configuration for all ports
 * 2. Clearing default VSI membership
 *
 * Return: 0 on success, negative error code on failure
 */
int qce2204_teardown_none_tag_vsi(struct qce2204_priv *priv)
{
	struct qce2204_ppe_port_vsi_cfg port_vsi_cfg = {};
	struct qce2204_ppe_vsi_member_cfg vsi_member_cfg = {};
	struct dsa_switch *ds = priv->ds;
	u8 cpu_ports_mask = dsa_cpu_ports(ds);
	struct dsa_port *dp;
	int ret;

	dev_info(priv->dev, "Tearing down none tag VSI configuration\n");

	/* Step 1: Clear VSI configuration for all ports */
	port_vsi_cfg.vsi_valid = false;
	port_vsi_cfg.vsi = 0;

	dsa_switch_for_each_available_port(dp, ds) {
		ret = qce2204_ppe_port_vsi_set(priv, dp->index, &port_vsi_cfg);
		if (ret) {
			dev_err(priv->dev, "Failed to clear VSI for port %d: %d\n",
				dp->index, ret);
			return ret;
		}
	}

	/* Step 2: Reset default VSI membership */
	vsi_member_cfg.member_port_bitmap = cpu_ports_mask;
	vsi_member_cfg.uuc_bitmap = cpu_ports_mask;
	vsi_member_cfg.umc_bitmap = cpu_ports_mask;
	vsi_member_cfg.bc_bitmap = cpu_ports_mask;
	ret = qce2204_ppe_vsi_member_set(priv, QCE2204_DEFAULT_VSI, &vsi_member_cfg);
	if (ret) {
		dev_err(priv->dev, "Failed to clear default VSI membership: %d\n", ret);
		return ret;
	}

	dev_info(priv->dev, "None tag VSI configuration torn down\n");
	return 0;
}

/**
 * qce2204_setup_none_tag_rstp - Setup RSTP packet redirect for none tag mode
 * @priv: QCE2204 private data
 *
 * Configures the PPE to intercept RSTP BPDUs (dst MAC 01:80:c2:00:00:00)
 * from all user ports and redirect them to the CPU via a dedicated virtual
 * port (QCE2204_PPE_RSTP_VP) with an Atheros tag type of
 * QCE2204_RSTP_ATHTAG_TYPE so the CPU can distinguish RSTP frames from
 * normal traffic.
 *
 * Return: 0 on success, negative error code on failure
 */
int qce2204_setup_none_tag_rstp(struct qce2204_priv *priv)
{
	struct qce2204_ppe_acl_rule_cfg acl_cfg = {};
	struct qce2204_ppe_l2_vp_port_post_cfg post_cfg = {};
	struct qce2204_ppe_eg_vp_athtag_cfg tx_cfg = {};
	struct qce2204_ppe_eg_gen_ctrl_cfg hdrt_cfg = {};
	struct qce2204_ppe_port_athtag_rx_cfg rx_cfg = {};
	struct dsa_switch *ds = priv->ds;
	int ret;

	dev_dbg(priv->dev, "Setting up none tag RSTP configuration\n");

	/* Step 1: Configure ACL rule to match RSTP BPDUs from all user ports */
	acl_cfg.is_delete = false;
	acl_cfg.mac[0] = 0x01;
	acl_cfg.mac[1] = 0x80;
	acl_cfg.mac[2] = 0xc2;
	acl_cfg.mac[3] = 0x00;
	acl_cfg.mac[4] = 0x00;
	acl_cfg.mac[5] = 0x00;
	acl_cfg.hw_rule_type = 0;
	acl_cfg.src_type = 0;
	acl_cfg.src = dsa_user_ports(ds);
	acl_cfg.dest_valid = true;
	acl_cfg.dest_info = QCE2204_PPE_DEST_INFO(QCE2204_PPE_DEST_INFO_PORT_ID,
						   QCE2204_PPE_RSTP_VP);
	ret = qce2204_ppe_acl_rule_set(priv, QCE2204_PPE_RSTP_ACL,
				       QCE2204_PPE_ACL_MAC_DA_RULE, &acl_cfg);
	if (ret) {
		dev_err(priv->dev, "Failed to set RSTP ACL rule: %d\n", ret);
		return ret;
	}

	/* Step 2: Map RSTP virtual port to CPU physical port */
	post_cfg.pport = QCE2204_CPU_PORT_ID;
	ret = qce2204_ppe_l2_vp_port_post_set(priv, QCE2204_PPE_RSTP_VP, &post_cfg);
	if (ret) {
		dev_err(priv->dev, "Failed to set RSTP VP port post: %d\n", ret);
		return ret;
	}

	/* Step 3: Enable Atheros tag insertion on RSTP virtual port */
	tx_cfg.athtag_en = true;
	ret = qce2204_ppe_eg_vp_athtag_set(priv, QCE2204_PPE_RSTP_VP, &tx_cfg);
	if (ret) {
		dev_err(priv->dev, "Failed to set RSTP VP athtag: %d\n", ret);
		return ret;
	}

	/* Step 4: Set RSTP-specific Atheros header type */
	hdrt_cfg.ath_type = QCE2204_RSTP_ATHTAG_TYPE;
	ret = qce2204_ppe_eg_gen_ctrl_set(priv, &hdrt_cfg);
	if (ret) {
		dev_err(priv->dev, "Failed to set TX RSTP athtag type: %d\n", ret);
		return ret;
	}

	/* Step 5: Update AthTag RX on CPU port */
	rx_cfg.athtag_type = QCE2204_RSTP_ATHTAG_TYPE;
	rx_cfg.athtag_en = true;
	rx_cfg.version = 0;
	ret = qce2204_ppe_port_athtag_rx_set(priv, QCE2204_CPU_PORT_ID, &rx_cfg);
	if (ret) {
		dev_err(priv->dev, "Failed to set RX RSTP athtag type: %d\n", ret);
		return ret;
	}

	dev_dbg(priv->dev, "None tag RSTP configuration set up\n");
	return 0;
}

/**
 * qce2204_teardown_none_tag_rstp - Teardown RSTP packet redirect for none tag mode
 * @priv: QCE2204 private data
 *
 * Reverses the configuration applied by qce2204_setup_none_tag_rstp():
 * clears the RSTP ACL rule, resets the RSTP virtual port mapping, disables
 * Atheros tag insertion on the RSTP virtual port, and restores the global
 * Atheros header type to QCE2204_ATHTAG_TYPE.
 *
 * Return: 0 on success, negative error code on failure
 */
int qce2204_teardown_none_tag_rstp(struct qce2204_priv *priv)
{
	struct qce2204_ppe_acl_rule_cfg acl_cfg = {};
	struct qce2204_ppe_l2_vp_port_post_cfg post_cfg = {};
	struct qce2204_ppe_eg_vp_athtag_cfg tx_cfg = {};
	struct qce2204_ppe_eg_gen_ctrl_cfg hdrt_cfg = {};
	struct qce2204_ppe_port_athtag_rx_cfg rx_cfg = {};
	int ret;

	dev_dbg(priv->dev, "Tearing down none tag RSTP configuration\n");

	/* Step 1: Clear RSTP ACL rule (all-zero cfg deletes the entry) */
	acl_cfg.is_delete = true;
	ret = qce2204_ppe_acl_rule_set(priv, QCE2204_PPE_RSTP_ACL,
				       QCE2204_PPE_ACL_MAC_DA_RULE, &acl_cfg);
	if (ret) {
		dev_err(priv->dev, "Failed to clear RSTP ACL rule: %d\n", ret);
		return ret;
	}

	/* Step 2: Clear RSTP virtual port mapping */
	ret = qce2204_ppe_l2_vp_port_post_set(priv, QCE2204_PPE_RSTP_VP, &post_cfg);
	if (ret) {
		dev_err(priv->dev, "Failed to clear RSTP VP port post: %d\n", ret);
		return ret;
	}

	/* Step 3: Disable Atheros tag insertion on RSTP virtual port */
	ret = qce2204_ppe_eg_vp_athtag_set(priv, QCE2204_PPE_RSTP_VP, &tx_cfg);
	if (ret) {
		dev_err(priv->dev, "Failed to clear RSTP VP athtag: %d\n", ret);
		return ret;
	}

	/* Step 4: Restore global Atheros header type */
	hdrt_cfg.ath_type = QCE2204_ATHTAG_TYPE;
	ret = qce2204_ppe_eg_gen_ctrl_set(priv, &hdrt_cfg);
	if (ret) {
		dev_err(priv->dev, "Failed to restore TX athtag type: %d\n", ret);
		return ret;
	}

	/* Step 5: Update AthTag RX on CPU port */
	rx_cfg.athtag_type = QCE2204_ATHTAG_TYPE;
	rx_cfg.athtag_en = true;
	rx_cfg.version = 0;
	ret = qce2204_ppe_port_athtag_rx_set(priv, QCE2204_CPU_PORT_ID, &rx_cfg);
	if (ret) {
		dev_err(priv->dev, "Failed to restore RX RSTP athtag type: %d\n", ret);
		return ret;
	}

	dev_dbg(priv->dev, "None tag RSTP configuration torn down\n");
	return 0;
}

/**
 * qce2204_ppe_port_athtag_rx_set - Configure port Atheros tag RX parsing
 * @priv: QCE2204 private data
 * @port_id: Port ID
 * @cfg: Atheros tag RX configuration
 *
 * Return: 0 on success, negative error code on failure
 */
int qce2204_ppe_port_athtag_rx_set(struct qce2204_priv *priv,
				    u32 port_id,
				    struct qce2204_ppe_port_athtag_rx_cfg *cfg)
{
	u32 reg_addr;
	u32 reg_val;
	int ret;

	if (!priv || !cfg)
		return -EINVAL;

	if (port_id >= QCE2204_PPE_PRX_PORT_TO_VP_MAPPING_ENTRIES)
		return -EINVAL;

	reg_addr = QCE2204_PPE_PRX_PORT_TO_VP_MAPPING_ADDR +
		   port_id * QCE2204_PPE_PRX_PORT_TO_VP_MAPPING_INC;

	ret = regmap_read(priv->regmap, reg_addr, &reg_val);
	if (ret)
		return ret;

	reg_val &= ~QCE2204_PPE_PRX_PORT_TO_VP_MAPPING_ATHEROS_HDR_TYPE;
	reg_val |= FIELD_PREP(QCE2204_PPE_PRX_PORT_TO_VP_MAPPING_ATHEROS_HDR_TYPE,
			      cfg->athtag_type);

	reg_val &= ~QCE2204_PPE_PRX_PORT_TO_VP_MAPPING_ATHEROS_HDR_EN;
	reg_val |= FIELD_PREP(QCE2204_PPE_PRX_PORT_TO_VP_MAPPING_ATHEROS_HDR_EN,
			      cfg->athtag_en ? 1 : 0);

	reg_val &= ~QCE2204_PPE_PRX_PORT_TO_VP_MAPPING_ATHEROS_HDR_VER;
	reg_val |= FIELD_PREP(QCE2204_PPE_PRX_PORT_TO_VP_MAPPING_ATHEROS_HDR_VER,
			      cfg->version ? 1 : 0);

	return regmap_write(priv->regmap, reg_addr, reg_val);
}

/**
 * qce2204_ppe_athtag_dst_port_mapping_set - Configure Atheros tag destination port mapping
 * @priv: QCE2204 private data
 * @port_id: Port ID
 * @cfg: Atheros tag destination port mapping configuration
 *
 * Return: 0 on success, negative error code on failure
 */
int qce2204_ppe_athtag_dst_port_mapping_set(struct qce2204_priv *priv,
					     u32 port_id,
					     struct qce2204_ppe_athtag_dst_port_mapping_cfg *cfg)
{
	u32 reg_addr;
	u32 tbl_data[QCE2204_PPE_DST_PORT_MAPPING_TBL_INC / 4];
	int ret;

	if (!priv || !cfg)
		return -EINVAL;

	if (port_id >= QCE2204_PPE_DST_PORT_MAPPING_TBL_ENTRIES)
		return -EINVAL;

	reg_addr = QCE2204_PPE_DST_PORT_MAPPING_TBL_ADDR +
		   port_id * QCE2204_PPE_DST_PORT_MAPPING_TBL_INC;

	ret = regmap_bulk_read(priv->regmap, reg_addr, tbl_data,
			       QCE2204_PPE_DST_PORT_MAPPING_TBL_INC / 4);
	if (ret)
		return ret;

	QCE2204_PPE_DST_PORT_MAPPING_TBL_SET_DST_INFO_VALID(tbl_data,
							     cfg->dest_info_valid ? 1 : 0);
	QCE2204_PPE_DST_PORT_MAPPING_TBL_SET_DST_INFO(tbl_data, cfg->dest_info);

	return regmap_bulk_write(priv->regmap, reg_addr, tbl_data,
				 QCE2204_PPE_DST_PORT_MAPPING_TBL_INC / 4);
}

/**
 * qce2204_ppe_eg_vp_athtag_set - Configure egress VP Atheros tag insertion
 * @priv: QCE2204 private data
 * @port_id: Port ID
 * @cfg: Egress VP Atheros tag configuration
 *
 * Return: 0 on success, negative error code on failure
 */
int qce2204_ppe_eg_vp_athtag_set(struct qce2204_priv *priv,
				  u32 port_id,
				  struct qce2204_ppe_eg_vp_athtag_cfg *cfg)
{
	u32 reg_addr;
	u32 tbl_data[QCE2204_PPE_EG_VP_TBL_INC / 4];
	int ret;

	if (!priv || !cfg)
		return -EINVAL;

	if (port_id >= QCE2204_PPE_EG_VP_TBL_ENTRIES)
		return -EINVAL;

	reg_addr = QCE2204_PPE_EG_VP_TBL_ADDR +
		   port_id * QCE2204_PPE_EG_VP_TBL_INC;

	ret = regmap_bulk_read(priv->regmap, reg_addr, tbl_data,
			       QCE2204_PPE_EG_VP_TBL_INC / 4);
	if (ret)
		return ret;

	QCE2204_PPE_EG_VP_TBL_SET_ATH_HDR_INSERT(tbl_data, cfg->athtag_en ? 1 : 0);
	QCE2204_PPE_EG_VP_TBL_SET_ATH_HDR_VER(tbl_data, cfg->version);
	QCE2204_PPE_EG_VP_TBL_SET_ATH_HDR_DEFAULT_TYPE(tbl_data, cfg->action_type);
	QCE2204_PPE_EG_VP_TBL_SET_ATH_PORT_BITMAP_LO(tbl_data, cfg->dest_info & 0x3F);
	QCE2204_PPE_EG_VP_TBL_SET_ATH_PORT_BITMAP_HI(tbl_data, (cfg->dest_info >> 6) & 0x1);
	QCE2204_PPE_EG_VP_TBL_SET_ATH_HDR_FROM_CPU(tbl_data, cfg->from_cpu ? 1 : 0);

	return regmap_bulk_write(priv->regmap, reg_addr, tbl_data,
				 QCE2204_PPE_EG_VP_TBL_INC / 4);
}

/**
 * qce2204_ppe_eg_gen_ctrl_set - Configure egress general control for Atheros header type
 * @priv: QCE2204 private data
 * @cfg: Egress general control configuration
 *
 * Return: 0 on success, negative error code on failure
 */
int qce2204_ppe_eg_gen_ctrl_set(struct qce2204_priv *priv,
				 struct qce2204_ppe_eg_gen_ctrl_cfg *cfg)
{
	u32 reg_val;
	int ret;

	if (!priv || !cfg)
		return -EINVAL;

	ret = regmap_read(priv->regmap, QCE2204_PPE_EG_GEN_CTRL_ADDR, &reg_val);
	if (ret)
		return ret;

	reg_val &= ~QCE2204_PPE_EG_GEN_CTRL_ATH_HDR_TYPE;
	reg_val |= FIELD_PREP(QCE2204_PPE_EG_GEN_CTRL_ATH_HDR_TYPE, cfg->ath_type);

	return regmap_write(priv->regmap, QCE2204_PPE_EG_GEN_CTRL_ADDR, reg_val);
}

static int qce2204_config_mdio_backpressure_gpio(struct qce2204_priv *priv)
{
	int ret;

	ret = qce2204_mdio_write(priv->bus, QCE2204_MDIO_SOC_PHY_ADDR,
				 QCE2204_GPIO18_CFG_REG,
				 QCE2204_GPIO_MDIO_BACKPRESSURE_CFG);
	if (ret) {
		dev_err(priv->dev,
			"Failed to configure GPIO18 MDIO backpressure, reg=0x%x val=0x%x ret=%d\n",
			QCE2204_GPIO18_CFG_REG,
			QCE2204_GPIO_MDIO_BACKPRESSURE_CFG, ret);
		return ret;
	}

	ret = qce2204_mdio_write(priv->bus, QCE2204_MDIO_SOC_PHY_ADDR,
				 QCE2204_GPIO19_CFG_REG,
				 QCE2204_GPIO_MDIO_BACKPRESSURE_CFG);
	if (ret) {
		dev_err(priv->dev,
			"Failed to configure GPIO19 MDIO backpressure, reg=0x%x val=0x%x ret=%d\n",
			QCE2204_GPIO19_CFG_REG,
			QCE2204_GPIO_MDIO_BACKPRESSURE_CFG, ret);
		return ret;
	}

	dev_info(priv->dev, "Configured GPIO18/19 for MDIO backpressure\n");

	return 0;
}

/**
 * qce2204_ppe_mdio_backpressure_set - Configure MDIO backpressure
 * @priv: QCE2204 private data
 * @cfg: MDIO backpressure configuration
 *
 * Return: 0 on success, negative error code on failure
 */
int qce2204_ppe_mdio_backpressure_set(struct qce2204_priv *priv,
				      struct qce2204_ppe_mdio_backpressure_cfg *cfg)
{
	u32 reg_val;
	int ret;

	if (!priv || !cfg)
		return -EINVAL;

	ret = regmap_read(priv->regmap, QCE2204_PPE_MDIO_MASTER_CTRL0_ADDR, &reg_val);
	if (ret)
		return ret;

	reg_val &= ~QCE2204_PPE_MDIO_MASTER_CTRL0_TRIGGER_EN;
	reg_val |= FIELD_PREP(QCE2204_PPE_MDIO_MASTER_CTRL0_TRIGGER_EN,
			      cfg->trigger_en ? 1 : 0);
	reg_val &= ~QCE2204_PPE_MDIO_MASTER_CTRL0_TIMER_EN;
	reg_val |= FIELD_PREP(QCE2204_PPE_MDIO_MASTER_CTRL0_TIMER_EN,
			      cfg->timer_en ? 1 : 0);
	reg_val &= ~QCE2204_PPE_MDIO_MASTER_CTRL0_DIV_FACTOR;
	reg_val |= FIELD_PREP(QCE2204_PPE_MDIO_MASTER_CTRL0_DIV_FACTOR,
			      cfg->div_factor);
	reg_val &= ~QCE2204_PPE_MDIO_MASTER_CTRL0_PREAMBLE;
	reg_val |= FIELD_PREP(QCE2204_PPE_MDIO_MASTER_CTRL0_PREAMBLE,
			      cfg->preamble);

	ret = regmap_write(priv->regmap, QCE2204_PPE_MDIO_MASTER_CTRL0_ADDR, reg_val);
	if (ret)
		return ret;

	ret = regmap_read(priv->regmap, QCE2204_PPE_MDIO_MASTER_CTRL1_ADDR, &reg_val);
	if (ret)
		return ret;

	reg_val &= ~QCE2204_PPE_MDIO_MASTER_CTRL1_TIMER_CNT;
	reg_val |= FIELD_PREP(QCE2204_PPE_MDIO_MASTER_CTRL1_TIMER_CNT,
			      cfg->timer_cnt);

	ret = regmap_write(priv->regmap, QCE2204_PPE_MDIO_MASTER_CTRL1_ADDR, reg_val);
	if (ret)
		return ret;

	ret = regmap_read(priv->regmap, QCE2204_PPE_CROSSCHIP_BP_CTRL_ADDR, &reg_val);
	if (ret)
		return ret;

	reg_val &= ~QCE2204_PPE_CROSSCHIP_BP_CTRL_CROSSCHIP_BP_EN;
	reg_val |= FIELD_PREP(QCE2204_PPE_CROSSCHIP_BP_CTRL_CROSSCHIP_BP_EN,
			      cfg->crosschip_bp_en ? 1 : 0);
	reg_val &= ~QCE2204_PPE_CROSSCHIP_BP_CTRL_CROSSCHIP_BP_MODE;
	reg_val |= FIELD_PREP(QCE2204_PPE_CROSSCHIP_BP_CTRL_CROSSCHIP_BP_MODE,
			      cfg->crosschip_bp_mode ? 1 : 0);

	return regmap_write(priv->regmap, QCE2204_PPE_CROSSCHIP_BP_CTRL_ADDR, reg_val);
}

/* Initialize mdio backpressure config */
static int qce2204_mdio_backpressure_init(struct qce2204_priv *priv)
{
	struct qce2204_ppe_mdio_backpressure_cfg cfg = {};
	int ret;

	ret = qce2204_config_mdio_backpressure_gpio(priv);
	if (ret)
		return ret;

	cfg.trigger_en = true;
	cfg.timer_en = true;
	cfg.div_factor = 3;
	cfg.preamble = 2;
	cfg.timer_cnt = 0xc8;
	cfg.crosschip_bp_en = true;
	cfg.crosschip_bp_mode = priv->bp_mode;

	return qce2204_ppe_mdio_backpressure_set(priv, &cfg);
}

/**
 * qce2204_ppe_port_mtu_set - Configure port MTU
 * @priv: QCE2204 private data
 * @port_id: Port ID
 * @cfg: MTU configuration
 *
 * Return: 0 on success, negative error code on failure
 */
int qce2204_ppe_port_mtu_set(struct qce2204_priv *priv,
			      u32 port_id,
			      struct qce2204_ppe_port_mtu_cfg *cfg)
{
	u32 reg_addr;
	u32 tbl_data[QCE2204_PPE_MRU_MTU_CTRL_TBL_INC / 4];
	int ret;

	if (!priv || !cfg)
		return -EINVAL;

	if (port_id >= QCE2204_PPE_MRU_MTU_CTRL_TBL_ENTRIES)
		return -EINVAL;

	reg_addr = QCE2204_PPE_MRU_MTU_CTRL_TBL_ADDR +
		   port_id * QCE2204_PPE_MRU_MTU_CTRL_TBL_INC;

	ret = regmap_bulk_read(priv->regmap, reg_addr, tbl_data,
			       QCE2204_PPE_MRU_MTU_CTRL_TBL_INC / 4);
	if (ret)
		return ret;

	/* Update MTU fields */
	QCE2204_PPE_MRU_MTU_CTRL_SET_MTU(tbl_data, cfg->mtu);
	QCE2204_PPE_MRU_MTU_CTRL_SET_MTU_CMD(tbl_data, cfg->mtu_cmd);

	return regmap_bulk_write(priv->regmap, reg_addr, tbl_data,
				 QCE2204_PPE_MRU_MTU_CTRL_TBL_INC / 4);
}

/**
 * qce2204_ppe_port_mru_set - Configure port MRU
 * @priv: QCE2204 private data
 * @port_id: Port ID
 * @cfg: MRU configuration
 *
 * Return: 0 on success, negative error code on failure
 */
int qce2204_ppe_port_mru_set(struct qce2204_priv *priv,
			      u32 port_id,
			      struct qce2204_ppe_port_mru_cfg *cfg)
{
	u32 reg_addr;
	u32 tbl_data[QCE2204_PPE_MRU_MTU_CTRL_TBL_INC / 4];
	int ret;

	if (!priv || !cfg)
		return -EINVAL;

	if (port_id >= QCE2204_PPE_MRU_MTU_CTRL_TBL_ENTRIES)
		return -EINVAL;

	reg_addr = QCE2204_PPE_MRU_MTU_CTRL_TBL_ADDR +
		   port_id * QCE2204_PPE_MRU_MTU_CTRL_TBL_INC;

	ret = regmap_bulk_read(priv->regmap, reg_addr, tbl_data,
			       QCE2204_PPE_MRU_MTU_CTRL_TBL_INC / 4);
	if (ret)
		return ret;

	/* Update MRU fields */
	QCE2204_PPE_MRU_MTU_CTRL_SET_MRU(tbl_data, cfg->mru);
	QCE2204_PPE_MRU_MTU_CTRL_SET_MRU_CMD(tbl_data, cfg->mru_cmd);

	return regmap_bulk_write(priv->regmap, reg_addr, tbl_data,
				 QCE2204_PPE_MRU_MTU_CTRL_TBL_INC / 4);
}

/**
 * qce2204_setup_cpu_port_athtag - Configure AthTag for CPU port uplink/downlink
 * @priv: QCE2204 private data
 *
 * This config enables:
 * - Downlink (RX on CPU port): AthTag parsing enabled with fixed type.
 * - Destination port mapping: enable valid mapping for panel ports 1-4 to themselves.
 * - Uplink (TX on CPU port): AthTag insertion enabled on CPU port.
 *
 * Return: 0 on success, negative error code on failure
 */
int qce2204_setup_cpu_port_athtag(struct qce2204_priv *priv)
{
	struct qce2204_ppe_eg_vp_athtag_cfg tx_cfg = {};
	struct qce2204_ppe_eg_gen_ctrl_cfg hdrt_cfg = {};
	int ret;

	/* Enable AthTag TX insertion on CPU port (uplink), other fields unchanged (zero) */
	tx_cfg.athtag_en = true;
	tx_cfg.version = 0;
	tx_cfg.action_type = 0;
	tx_cfg.dest_info = 0;
	tx_cfg.from_cpu = false;

	ret = qce2204_ppe_eg_vp_athtag_set(priv, QCE2204_CPU_PORT_ID, &tx_cfg);
	if (ret)
		return ret;

	/* Set AthType for the header */
	hdrt_cfg.ath_type = QCE2204_ATHTAG_TYPE;

	return qce2204_ppe_eg_gen_ctrl_set(priv, &hdrt_cfg);
}

/**
 * qce2204_teardown_cpu_port_athtag - Restore AthTag configuration for CPU port
 * @priv: QCE2204 private data
 *
 * This restores configuration by:
 * - Disabling AthTag RX on CPU port.
 * - Clearing destination port mapping validity for panel ports 1-4.
 * - Disabling AthTag TX insertion on CPU port.
 *
 * Return: 0 on success, negative error code on failure
 */
int qce2204_teardown_cpu_port_athtag(struct qce2204_priv *priv)
{
	struct qce2204_ppe_eg_vp_athtag_cfg tx_cfg = {};
	struct qce2204_ppe_eg_gen_ctrl_cfg hdrt_cfg = {};
	int ret;

	/* Disable AthTag TX insertion on CPU port */
	tx_cfg.athtag_en = false;
	tx_cfg.version = 0;
	tx_cfg.action_type = 0;
	tx_cfg.dest_info = 0;
	tx_cfg.from_cpu = false;

	ret = qce2204_ppe_eg_vp_athtag_set(priv, QCE2204_CPU_PORT_ID, &tx_cfg);
	if (ret)
		return ret;

	/* Clear AthType for the header */
	hdrt_cfg.ath_type = 0;

	return qce2204_ppe_eg_gen_ctrl_set(priv, &hdrt_cfg);
}

/* Initialize athtag rx on CPU port */
static int qce2204_ppe_athtag_init(struct qce2204_priv *priv)
{
	struct qce2204_ppe_athtag_dst_port_mapping_cfg dst_cfg = {};
	struct qce2204_ppe_port_athtag_rx_cfg rx_cfg = {};
	struct dsa_switch *ds = priv->ds;
	struct dsa_port *dp;
	int ret;

	/* Setup destination port 1:1 mapping for panel ports */
	dsa_switch_for_each_user_port(dp, ds) {
		dst_cfg.dest_info_valid = true;
		dst_cfg.dest_info = QCE2204_PPE_DEST_INFO(QCE2204_PPE_DEST_INFO_PORT_ID, dp->index);

		ret = qce2204_ppe_athtag_dst_port_mapping_set(priv, dp->index, &dst_cfg);
		if (ret)
			return ret;
	}

	/* Enable AthTag RX on CPU port by default (downlink) */
	rx_cfg.athtag_type = QCE2204_ATHTAG_TYPE;
	rx_cfg.athtag_en = true;
	rx_cfg.version = 0;

	return qce2204_ppe_port_athtag_rx_set(priv, QCE2204_CPU_PORT_ID, &rx_cfg);
}

/**
 * qce2204_ppe_vlan_in_vlan_xlt_set - Configure ingress VLAN translation
 * @priv: QCE2204 private data
 * @index: Translation table index
 * @cfg: VLAN translation configuration
 *
 * Configures both rule and action tables for ingress VLAN translation.
 * When cfg is all zeros, clears the entry (deletes rule/action).
 * Automatically sets VALID bit when cfg is non-zero.
 * Converts port_id to PORT_BITMAP and PORT_TYPE based on value.
 *
 * Return: 0 on success, negative error code on failure
 */
int qce2204_ppe_vlan_in_vlan_xlt_set(struct qce2204_priv *priv,
				     u32 index,
				     struct qce2204_ppe_in_vlan_xlt_cfg *cfg)
{
	u32 reg_addr;
	u32 rule_data[QCE2204_PPE_IN_VLAN_XLT_RULE_INC / 4] = {};
	u32 action_data[QCE2204_PPE_IN_VLAN_XLT_ACTION_INC / 4] = {};
	struct qce2204_ppe_in_vlan_xlt_cfg zero_cfg = {};
	int ret;

	if (!priv || !cfg)
		return -EINVAL;

	if (index >= QCE2204_PPE_IN_VLAN_XLT_RULE_ENTRIES)
		return -EINVAL;

	/* Check if this is a delete operation (all fields zero) */
	if (memcmp(cfg, &zero_cfg, sizeof(*cfg)) == 0) {
		/* Clear rule table entry */
		reg_addr = QCE2204_PPE_IN_VLAN_XLT_RULE_ADDR +
			   index * QCE2204_PPE_IN_VLAN_XLT_RULE_INC;
		ret = regmap_bulk_write(priv->regmap, reg_addr, rule_data,
					QCE2204_PPE_IN_VLAN_XLT_RULE_INC / 4);
		if (ret)
			return ret;

		/* Clear action table entry */
		reg_addr = QCE2204_PPE_IN_VLAN_XLT_ACTION_ADDR +
			   index * QCE2204_PPE_IN_VLAN_XLT_ACTION_INC;
		return regmap_bulk_write(priv->regmap, reg_addr, action_data,
					 QCE2204_PPE_IN_VLAN_XLT_ACTION_INC / 4);
	}

	/* Configure rule table */
	reg_addr = QCE2204_PPE_IN_VLAN_XLT_RULE_ADDR +
		   index * QCE2204_PPE_IN_VLAN_XLT_RULE_INC;

	ret = regmap_bulk_read(priv->regmap, reg_addr, rule_data,
			       QCE2204_PPE_IN_VLAN_XLT_RULE_INC / 4);
	if (ret)
		return ret;

	/* Set rule fields */
	QCE2204_PPE_IN_VLAN_XLT_RULE_SET_VALID(rule_data, 1);
	QCE2204_PPE_IN_VLAN_XLT_RULE_SET_PORT_BITMAP(rule_data,
						      cfg->port_id < 64 ? BIT(cfg->port_id) : cfg->port_id);
	QCE2204_PPE_IN_VLAN_XLT_RULE_SET_SKEY_FMT(rule_data, cfg->svid_fmt);
	QCE2204_PPE_IN_VLAN_XLT_RULE_SET_SKEY_VID_INCL(rule_data, cfg->svid_inc ? 1 : 0);
	QCE2204_PPE_IN_VLAN_XLT_RULE_SET_SKEY_VID(rule_data, cfg->svid);
	QCE2204_PPE_IN_VLAN_XLT_RULE_SET_CKEY_FMT(rule_data, cfg->cvid_fmt);
	QCE2204_PPE_IN_VLAN_XLT_RULE_SET_CKEY_VID_INCL(rule_data, cfg->cvid_inc ? 1 : 0);
	QCE2204_PPE_IN_VLAN_XLT_RULE_SET_CKEY_VID(rule_data, cfg->cvid);
	QCE2204_PPE_IN_VLAN_XLT_RULE_SET_PORT_TYPE(rule_data, cfg->port_id < 64 ? 0 : 1);
	QCE2204_PPE_IN_VLAN_XLT_RULE_SET_DHCP_TYPE(rule_data, 0x7);
	QCE2204_PPE_IN_VLAN_XLT_RULE_SET_MC_TYPE(rule_data, 0x7);

	ret = regmap_bulk_write(priv->regmap, reg_addr, rule_data,
				QCE2204_PPE_IN_VLAN_XLT_RULE_INC / 4);
	if (ret)
		return ret;

	/* Configure action table */
	reg_addr = QCE2204_PPE_IN_VLAN_XLT_ACTION_ADDR +
		   index * QCE2204_PPE_IN_VLAN_XLT_ACTION_INC;

	ret = regmap_bulk_read(priv->regmap, reg_addr, action_data,
			       QCE2204_PPE_IN_VLAN_XLT_ACTION_INC / 4);
	if (ret)
		return ret;

	/* Set action fields */
	QCE2204_PPE_IN_VLAN_XLT_ACTION_SET_XLT_SVID_CMD(action_data, cfg->svid_xlt_cmd);
	QCE2204_PPE_IN_VLAN_XLT_ACTION_SET_XLT_SVID(action_data, cfg->svid_xlt);
	QCE2204_PPE_IN_VLAN_XLT_ACTION_SET_XLT_CVID_CMD(action_data, cfg->cvid_xlt_cmd);
	QCE2204_PPE_IN_VLAN_XLT_ACTION_SET_XLT_CVID(action_data, cfg->cvid_xlt);
	QCE2204_PPE_IN_VLAN_XLT_ACTION_SET_XLT_CPCP_CMD(action_data, cfg->cpcp_xlt_cmd);
	QCE2204_PPE_IN_VLAN_XLT_ACTION_SET_TAGS_TO_REMOVE(action_data, cfg->tags_rmv);
	QCE2204_PPE_IN_VLAN_XLT_ACTION_SET_COUNTER_EN(action_data, cfg->cnt_en ? 1 : 0);
	QCE2204_PPE_IN_VLAN_XLT_ACTION_SET_COUNTER_ID(action_data, cfg->cnt_id);
	QCE2204_PPE_IN_VLAN_XLT_ACTION_SET_VSI_CMD(action_data, cfg->vsi_cmd ? 1 : 0);
	QCE2204_PPE_IN_VLAN_XLT_ACTION_SET_VSI(action_data, cfg->vsi);
	QCE2204_PPE_IN_VLAN_XLT_ACTION_SET_SRC_INFO_VALID(action_data, cfg->src_valid ? 1 : 0);
	QCE2204_PPE_IN_VLAN_XLT_ACTION_SET_SRC_INFO_TYPE(action_data, cfg->src_type ? 1 : 0);
	QCE2204_PPE_IN_VLAN_XLT_ACTION_SET_SRC_INFO(action_data, cfg->src_info);
	QCE2204_PPE_IN_VLAN_XLT_ACTION_SET_FWD_CMD(action_data, cfg->fwd_cmd);
	QCE2204_PPE_IN_VLAN_XLT_ACTION_SET_DEST_INFO_VALID(action_data, cfg->dest_valid ? 1 : 0);
	QCE2204_PPE_IN_VLAN_XLT_ACTION_SET_DEST_INFO_LO(action_data, cfg->dest_info & 0x7);
	QCE2204_PPE_IN_VLAN_XLT_ACTION_SET_DEST_INFO_HI(action_data, (cfg->dest_info >> 3) & 0x1F);

	return regmap_bulk_write(priv->regmap, reg_addr, action_data,
				 QCE2204_PPE_IN_VLAN_XLT_ACTION_INC / 4);
}

/**
 * qce2204_ppe_vlan_eg_vlan_xlt_set - Configure egress VLAN translation
 * @priv: QCE2204 private data
 * @index: Translation table index
 * @cfg: VLAN translation configuration
 *
 * Configures both rule and action tables for egress VLAN translation.
 * When cfg is all zeros, clears the entry (deletes rule/action).
 * Automatically sets VALID bit when cfg is non-zero.
 * Converts port_id to PORT_BITMAP and PORT_TYPE based on value.
 *
 * Return: 0 on success, negative error code on failure
 */
int qce2204_ppe_vlan_eg_vlan_xlt_set(struct qce2204_priv *priv,
				     u32 index,
				     struct qce2204_ppe_eg_vlan_xlt_cfg *cfg)
{
	u32 reg_addr;
	u32 rule_data[QCE2204_PPE_EG_VLAN_XLT_RULE_INC / 4] = {};
	u32 action_data[QCE2204_PPE_EG_VLAN_XLT_ACTION_INC / 4] = {};
	struct qce2204_ppe_eg_vlan_xlt_cfg zero_cfg = {};
	int ret;

	if (!priv || !cfg)
		return -EINVAL;

	if (index >= QCE2204_PPE_EG_VLAN_XLT_RULE_ENTRIES)
		return -EINVAL;

	/* Check if this is a delete operation (all fields zero) */
	if (memcmp(cfg, &zero_cfg, sizeof(*cfg)) == 0) {
		/* Clear rule table entry */
		reg_addr = QCE2204_PPE_EG_VLAN_XLT_RULE_ADDR +
			   index * QCE2204_PPE_EG_VLAN_XLT_RULE_INC;
		ret = regmap_bulk_write(priv->regmap, reg_addr, rule_data,
					QCE2204_PPE_EG_VLAN_XLT_RULE_INC / 4);
		if (ret)
			return ret;

		/* Clear action table entry */
		reg_addr = QCE2204_PPE_EG_VLAN_XLT_ACTION_ADDR +
			   index * QCE2204_PPE_EG_VLAN_XLT_ACTION_INC;
		return regmap_bulk_write(priv->regmap, reg_addr, action_data,
					 QCE2204_PPE_EG_VLAN_XLT_ACTION_INC / 4);
	}

	/* Configure rule table */
	reg_addr = QCE2204_PPE_EG_VLAN_XLT_RULE_ADDR +
		   index * QCE2204_PPE_EG_VLAN_XLT_RULE_INC;

	ret = regmap_bulk_read(priv->regmap, reg_addr, rule_data,
			       QCE2204_PPE_EG_VLAN_XLT_RULE_INC / 4);
	if (ret)
		return ret;

	/* Set rule fields */
	QCE2204_PPE_EG_VLAN_XLT_RULE_SET_VALID(rule_data, 1);
	QCE2204_PPE_EG_VLAN_XLT_RULE_SET_PORT_BITMAP(rule_data,
						      cfg->port_id < 64 ? BIT(cfg->port_id) : cfg->port_id);
	QCE2204_PPE_EG_VLAN_XLT_RULE_SET_VSI_INCL(rule_data, cfg->vsi_inc ? 1 : 0);
	QCE2204_PPE_EG_VLAN_XLT_RULE_SET_VSI(rule_data, cfg->vsi);
	QCE2204_PPE_EG_VLAN_XLT_RULE_SET_VSI_VALID(rule_data, cfg->vsi_valid ? 1 : 0);
	QCE2204_PPE_EG_VLAN_XLT_RULE_SET_SKEY_FMT(rule_data, cfg->svid_fmt);
	QCE2204_PPE_EG_VLAN_XLT_RULE_SET_SKEY_VID_INCL(rule_data, cfg->svid_inc ? 1 : 0);
	QCE2204_PPE_EG_VLAN_XLT_RULE_SET_SKEY_VID_LO(rule_data, cfg->svid & 0x3FF);
	QCE2204_PPE_EG_VLAN_XLT_RULE_SET_SKEY_VID_HI(rule_data, (cfg->svid >> 10) & 0x3);
	QCE2204_PPE_EG_VLAN_XLT_RULE_SET_CKEY_FMT(rule_data, cfg->cvid_fmt);
	QCE2204_PPE_EG_VLAN_XLT_RULE_SET_CKEY_VID_INCL(rule_data, cfg->cvid_inc ? 1 : 0);
	QCE2204_PPE_EG_VLAN_XLT_RULE_SET_CKEY_VID(rule_data, cfg->cvid);
	QCE2204_PPE_EG_VLAN_XLT_RULE_SET_PORT_TYPE(rule_data, cfg->port_id < 64 ? 0 : 1);
	QCE2204_PPE_EG_VLAN_XLT_RULE_SET_DHCP_TYPE(rule_data, 0x7);
	QCE2204_PPE_EG_VLAN_XLT_RULE_SET_MC_TYPE(rule_data, 0x7);

	ret = regmap_bulk_write(priv->regmap, reg_addr, rule_data,
				QCE2204_PPE_EG_VLAN_XLT_RULE_INC / 4);
	if (ret)
		return ret;

	/* Configure action table */
	reg_addr = QCE2204_PPE_EG_VLAN_XLT_ACTION_ADDR +
		   index * QCE2204_PPE_EG_VLAN_XLT_ACTION_INC;

	ret = regmap_bulk_read(priv->regmap, reg_addr, action_data,
			       QCE2204_PPE_EG_VLAN_XLT_ACTION_INC / 4);
	if (ret)
		return ret;

	/* Set action fields */
	QCE2204_PPE_EG_VLAN_XLT_ACTION_SET_XLT_SVID_CMD(action_data, cfg->svid_xlt_cmd);
	QCE2204_PPE_EG_VLAN_XLT_ACTION_SET_XLT_SVID(action_data, cfg->svid_xlt);
	QCE2204_PPE_EG_VLAN_XLT_ACTION_SET_XLT_CVID_CMD(action_data, cfg->cvid_xlt_cmd);
	QCE2204_PPE_EG_VLAN_XLT_ACTION_SET_XLT_CVID(action_data, cfg->cvid_xlt);
	QCE2204_PPE_EG_VLAN_XLT_ACTION_SET_TAGS_TO_REMOVE(action_data, cfg->tags_rmv);
	QCE2204_PPE_EG_VLAN_XLT_ACTION_SET_COUNTER_EN(action_data, cfg->cnt_en ? 1 : 0);
	QCE2204_PPE_EG_VLAN_XLT_ACTION_SET_COUNTER_ID(action_data, cfg->cnt_id);
	QCE2204_PPE_EG_VLAN_XLT_ACTION_SET_FWD_CMD(action_data, cfg->fwd_cmd);

	return regmap_bulk_write(priv->regmap, reg_addr, action_data,
				 QCE2204_PPE_EG_VLAN_XLT_ACTION_INC / 4);
}

/**
 * qce2204_ppe_l2_vp_port_post_set - Configure L2 VP port post table
 * @priv: Driver private data
 * @vport: Virtual port index
 * @cfg: Configuration containing the physical port ID
 *
 * Writes PHYSICAL_PORT for the given virtual port in L2_VP_PORT_POST_TBL,
 * then maps the virtual port's unicast queue base to the physical port's
 * queue base so that traffic destined to @vport is scheduled on the
 * correct physical port queues.
 *
 * Return: 0 on success, negative error code on failure
 */
int qce2204_ppe_l2_vp_port_post_set(struct qce2204_priv *priv,
				     u32 vport,
				     struct qce2204_ppe_l2_vp_port_post_cfg *cfg)
{
	struct qce2204_ppe_queue_ucast_dest queue_dst = {};
	u32 reg_addr;
	u32 tbl_data[QCE2204_PPE_L2_VP_PORT_POST_TBL_INC / 4];
	int q_base, q_end, ret;

	if (!priv || !cfg)
		return -EINVAL;

	if (vport >= QCE2204_PPE_L2_VP_PORT_POST_TBL_ENTRIES)
		return -EINVAL;

	/* Step 1: Configure PHYSICAL_PORT in L2_VP_PORT_POST_TBL */
	reg_addr = QCE2204_PPE_L2_VP_PORT_POST_TBL_ADDR +
		   vport * QCE2204_PPE_L2_VP_PORT_POST_TBL_INC;

	ret = regmap_bulk_read(priv->regmap, reg_addr, tbl_data,
			       QCE2204_PPE_L2_VP_PORT_POST_TBL_INC / 4);
	if (ret)
		return ret;

	QCE2204_PPE_L2_VP_PORT_POST_SET_PHYSICAL_PORT(tbl_data, cfg->pport);

	ret = regmap_bulk_write(priv->regmap, reg_addr, tbl_data,
				QCE2204_PPE_L2_VP_PORT_POST_TBL_INC / 4);
	if (ret)
		return ret;

	/* Step 2: Get the unicast queue base for the physical port */
	ret = qce2204_ppe_port_resource_get(priv, cfg->pport,
					    QCE2204_PPE_RES_UCAST,
					    &q_base, &q_end);
	if (ret)
		return ret;

	/* Step 3: Map vport's queue base to the physical port's queue base */
	queue_dst.dest_port = vport;

	return qce2204_ppe_queue_ucast_base_set(priv, queue_dst, q_base, 0);
}

/**
 * qce2204_ppe_acl_rule_set - Configure ACL rule, mask and action tables
 * @priv: Driver private data
 * @index: Table entry index
 * @rule_type: ACL rule match type (e.g. QCE2204_PPE_ACL_MAC_DA_RULE)
 * @cfg: Rule configuration; pass all-zero to delete the entry
 *
 * Writes the IPO rule, mask and action tables at the given index.
 * When @cfg is all-zero all three table entries are cleared.
 *
 * Return: 0 on success, negative error code on failure
 */
int qce2204_ppe_acl_rule_set(struct qce2204_priv *priv,
			      u32 index,
			      enum qce2204_ppe_acl_rule_type rule_type,
			      struct qce2204_ppe_acl_rule_cfg *cfg)
{
	u32 rule_addr, mask_addr, action_addr;
	u32 rule_data[QCE2204_PPE_IPO_MAC_DA_RULE_INC / 4];
	u32 mask_data[QCE2204_PPE_IPO_MAC_DA_MASK_INC / 4];
	u32 action_data[QCE2204_PPE_IPO_ACTION_INC / 4];
	int ret;

	if (!priv || !cfg)
		return -EINVAL;

	if (index >= QCE2204_PPE_IPO_MAC_DA_RULE_ENTRIES)
		return -EINVAL;

	rule_addr = QCE2204_PPE_IPO_RULE_ADDR +
		    index * QCE2204_PPE_IPO_MAC_DA_RULE_INC;
	mask_addr = QCE2204_PPE_IPO_MAC_DA_MASK_ADDR +
		    index * QCE2204_PPE_IPO_MAC_DA_MASK_INC;
	action_addr = QCE2204_PPE_IPO_ACTION_ADDR +
		      index * QCE2204_PPE_IPO_ACTION_INC;

	if (cfg->is_delete) {
		memset(rule_data, 0, sizeof(rule_data));
		memset(mask_data, 0, sizeof(mask_data));
		memset(action_data, 0, sizeof(action_data));

		ret = regmap_bulk_write(priv->regmap, rule_addr, rule_data,
					QCE2204_PPE_IPO_MAC_DA_RULE_INC / 4);
		if (ret)
			return ret;

		ret = regmap_bulk_write(priv->regmap, mask_addr, mask_data,
					QCE2204_PPE_IPO_MAC_DA_MASK_INC / 4);
		if (ret)
			return ret;

		return regmap_bulk_write(priv->regmap, action_addr, action_data,
					 QCE2204_PPE_IPO_ACTION_INC / 4);
	}

	/* Configure rule table */
	ret = regmap_bulk_read(priv->regmap, rule_addr, rule_data,
			       QCE2204_PPE_IPO_MAC_DA_RULE_INC / 4);
	if (ret)
		return ret;

	switch (rule_type) {
	case QCE2204_PPE_ACL_MAC_DA_RULE:
		/* MAC_DA[31:0] -> W0, MAC_DA[47:32] -> W1[15:0] */
		QCE2204_PPE_IPO_MAC_DA_RULE_SET_MAC_DA(rule_data,
			(u32)cfg->mac[2] << 24 | (u32)cfg->mac[3] << 16 |
			(u32)cfg->mac[4] << 8  | (u32)cfg->mac[5]);
		QCE2204_PPE_IPO_MAC_DA_RULE_SET_MAC_DA_HI(rule_data,
			(u32)cfg->mac[0] << 8 | (u32)cfg->mac[1]);
		break;
	default:
		return -EINVAL;
	}

	/* Common rule fields (shared across all IPO rule types) */
	QCE2204_PPE_IPO_RULE_SET_INVERSE_EN(rule_data, cfg->inverse ? 1 : 0);
	QCE2204_PPE_IPO_RULE_SET_RULE_TYPE(rule_data, cfg->hw_rule_type);
	QCE2204_PPE_IPO_RULE_SET_SRC_TYPE(rule_data, cfg->src_type);
	QCE2204_PPE_IPO_RULE_SET_SRC_LO(rule_data, cfg->src & 0x1);
	QCE2204_PPE_IPO_RULE_SET_SRC_HI(rule_data, (cfg->src >> 1) & 0xFF);

	ret = regmap_bulk_write(priv->regmap, rule_addr, rule_data,
				QCE2204_PPE_IPO_MAC_DA_RULE_INC / 4);
	if (ret)
		return ret;

	/* Configure mask table */
	ret = regmap_bulk_read(priv->regmap, mask_addr, mask_data,
			       QCE2204_PPE_IPO_MAC_DA_MASK_INC / 4);
	if (ret)
		return ret;

	switch (rule_type) {
	case QCE2204_PPE_ACL_MAC_DA_RULE:
		/* Full MAC mask: all 48 bits set */
		QCE2204_PPE_IPO_MAC_DA_MASK_SET_MAC_DA_MASK(mask_data,
							     0xFFFFFFFF);
		QCE2204_PPE_IPO_MAC_DA_MASK_SET_MAC_DA_MASK_HI(mask_data,
								0xFFFF);
		break;
	default:
		return -EINVAL;
	}

	ret = regmap_bulk_write(priv->regmap, mask_addr, mask_data,
				QCE2204_PPE_IPO_MAC_DA_MASK_INC / 4);
	if (ret)
		return ret;

	/* Configure action table */
	ret = regmap_bulk_read(priv->regmap, action_addr, action_data,
			       QCE2204_PPE_IPO_ACTION_INC / 4);
	if (ret)
		return ret;

	QCE2204_PPE_IPO_ACTION_SET_DEST_INFO_CHANGE_EN(action_data,
						cfg->dest_valid ? 1 : 0);
	QCE2204_PPE_IPO_ACTION_SET_FWD_CMD(action_data, cfg->fwd_cmd);
	QCE2204_PPE_IPO_ACTION_SET_DEST_INFO(action_data, cfg->dest_info);

	return regmap_bulk_write(priv->regmap, action_addr, action_data,
				 QCE2204_PPE_IPO_ACTION_INC / 4);
}

/**
 * qce2204_ppe_hw_init - Initialize PPE hardware
 * @priv: QCE2204 private data
 *
 * Return: 0 on success, negative error code on failure
 */
/**
 * qce2204_ppe_vlan_tpid_set - Configure VLAN TPID registers
 * @priv: Driver private data
 * @dir: Direction (ingress or egress)
 * @cfg: VLAN TPID configuration
 *
 * Configure VLAN TPID values for ingress or egress direction.
 * For ingress: configures VLAN_TPID, VLAN_TPID_EXT0, VLAN_TPID_EXT1
 * For egress: configures EG_VLAN_TPID, EG_VLAN_TPID_EXT0, EG_VLAN_TPID_EXT1
 *
 * Return: 0 on success, negative error code on failure
 */
int qce2204_ppe_vlan_tpid_set(struct qce2204_priv *priv,
			       enum qce2204_ppe_direction dir,
			       struct qce2204_ppe_vlan_tpid_cfg *cfg)
{
	u32 reg_addr, reg_val;
	int ret;

	if (!priv || !cfg)
		return -EINVAL;

	if (dir == QCE2204_PPE_INGRESS) {
		/* Configure VLAN_TPID register */
		reg_addr = QCE2204_PPE_VLAN_TPID_ADDR;
		ret = regmap_read(priv->regmap, reg_addr, &reg_val);
		if (ret)
			return ret;

		reg_val &= ~QCE2204_PPE_VLAN_TPID_CTAG_TPID;
		reg_val |= FIELD_PREP(QCE2204_PPE_VLAN_TPID_CTAG_TPID, cfg->ctpid);
		reg_val &= ~QCE2204_PPE_VLAN_TPID_STAG_TPID;
		reg_val |= FIELD_PREP(QCE2204_PPE_VLAN_TPID_STAG_TPID, cfg->stpid);

		ret = regmap_write(priv->regmap, reg_addr, reg_val);
		if (ret)
			return ret;

		/* Configure VLAN_TPID_EXT0 register */
		reg_addr = QCE2204_PPE_VLAN_TPID_EXT0_ADDR;
		ret = regmap_read(priv->regmap, reg_addr, &reg_val);
		if (ret)
			return ret;

		reg_val &= ~QCE2204_PPE_VLAN_TPID_EXT0_CTAG_TPID;
		reg_val |= FIELD_PREP(QCE2204_PPE_VLAN_TPID_EXT0_CTAG_TPID, cfg->ctpid_ext);
		reg_val &= ~QCE2204_PPE_VLAN_TPID_EXT0_STAG_TPID;
		reg_val |= FIELD_PREP(QCE2204_PPE_VLAN_TPID_EXT0_STAG_TPID, cfg->stpid_ext);

		ret = regmap_write(priv->regmap, reg_addr, reg_val);
		if (ret)
			return ret;

		/* Configure VLAN_TPID_EXT1 register */
		reg_addr = QCE2204_PPE_VLAN_TPID_EXT1_ADDR;
		ret = regmap_read(priv->regmap, reg_addr, &reg_val);
		if (ret)
			return ret;

		reg_val &= ~QCE2204_PPE_VLAN_TPID_EXT1_CTAG_TPID_MAP;
		reg_val |= FIELD_PREP(QCE2204_PPE_VLAN_TPID_EXT1_CTAG_TPID_MAP, cfg->ctpid_map);
		reg_val &= ~QCE2204_PPE_VLAN_TPID_EXT1_STAG_TPID_MAP;
		reg_val |= FIELD_PREP(QCE2204_PPE_VLAN_TPID_EXT1_STAG_TPID_MAP, cfg->stpid_map);

		return regmap_write(priv->regmap, reg_addr, reg_val);
	} else if (dir == QCE2204_PPE_EGRESS) {
		/* Configure EG_VLAN_TPID register */
		reg_addr = QCE2204_PPE_EG_VLAN_TPID_ADDR;
		ret = regmap_read(priv->regmap, reg_addr, &reg_val);
		if (ret)
			return ret;

		reg_val &= ~QCE2204_PPE_EG_VLAN_TPID_STPID;
		reg_val |= FIELD_PREP(QCE2204_PPE_EG_VLAN_TPID_STPID, cfg->stpid);
		reg_val &= ~QCE2204_PPE_EG_VLAN_TPID_CTPID;
		reg_val |= FIELD_PREP(QCE2204_PPE_EG_VLAN_TPID_CTPID, cfg->ctpid);

		ret = regmap_write(priv->regmap, reg_addr, reg_val);
		if (ret)
			return ret;

		/* Configure EG_VLAN_TPID_EXT0 register */
		reg_addr = QCE2204_PPE_EG_VLAN_TPID_EXT0_ADDR;
		ret = regmap_read(priv->regmap, reg_addr, &reg_val);
		if (ret)
			return ret;

		reg_val &= ~QCE2204_PPE_EG_VLAN_TPID_EXT0_STPID;
		reg_val |= FIELD_PREP(QCE2204_PPE_EG_VLAN_TPID_EXT0_STPID, cfg->stpid_ext);
		reg_val &= ~QCE2204_PPE_EG_VLAN_TPID_EXT0_CTPID;
		reg_val |= FIELD_PREP(QCE2204_PPE_EG_VLAN_TPID_EXT0_CTPID, cfg->ctpid_ext);

		ret = regmap_write(priv->regmap, reg_addr, reg_val);
		if (ret)
			return ret;

		/* Configure EG_VLAN_TPID_EXT1 register */
		reg_addr = QCE2204_PPE_EG_VLAN_TPID_EXT1_ADDR;
		ret = regmap_read(priv->regmap, reg_addr, &reg_val);
		if (ret)
			return ret;

		reg_val &= ~QCE2204_PPE_EG_VLAN_TPID_EXT1_CTAG_TPID_MAP;
		reg_val |= FIELD_PREP(QCE2204_PPE_EG_VLAN_TPID_EXT1_CTAG_TPID_MAP, cfg->ctpid_map);
		reg_val &= ~QCE2204_PPE_EG_VLAN_TPID_EXT1_STAG_TPID_MAP;
		reg_val |= FIELD_PREP(QCE2204_PPE_EG_VLAN_TPID_EXT1_STAG_TPID_MAP, cfg->stpid_map);

		return regmap_write(priv->regmap, reg_addr, reg_val);
	}

	return -EINVAL;
}

/**
 * qce2204_ppe_port_vlan_role_set - Configure port VLAN role
 * @priv: Driver private data
 * @port_id: Port ID
 * @direction: Direction (ingress or egress)
 * @cfg: Port VLAN role configuration
 *
 * Configure port role for ingress (PORT_PARSING) or egress (PORT_EG_VLAN).
 * Port role: 0=edge port, 1=core port (for QinQ processing).
 *
 * Return: 0 on success, negative error code on failure
 */
int qce2204_ppe_port_vlan_role_set(struct qce2204_priv *priv,
				    u32 port_id,
				    enum qce2204_ppe_direction direction,
				    struct qce2204_ppe_port_vlan_role_cfg *cfg)
{
	u32 reg_addr, reg_val;
	int ret;

	if (!priv || !cfg)
		return -EINVAL;

	if (direction == QCE2204_PPE_INGRESS) {
		/* Configure PORT_PARSING_REG for ingress */
		if (port_id >= QCE2204_PPE_PORT_PARSING_REG_ENTRIES)
			return -EINVAL;

		reg_addr = QCE2204_PPE_PORT_PARSING_REG_ADDR +
			   port_id * QCE2204_PPE_PORT_PARSING_REG_INC;

		ret = regmap_read(priv->regmap, reg_addr, &reg_val);
		if (ret)
			return ret;

		reg_val &= ~QCE2204_PPE_PORT_PARSING_REG_PORT_ROLE;
		reg_val |= FIELD_PREP(QCE2204_PPE_PORT_PARSING_REG_PORT_ROLE,
				      cfg->port_role ? 1 : 0);

		return regmap_write(priv->regmap, reg_addr, reg_val);
	} else if (direction == QCE2204_PPE_EGRESS) {
		/* Configure PORT_EG_VLAN for egress */
		if (port_id >= QCE2204_PPE_PORT_EG_VLAN_TBL_ENTRIES)
			return -EINVAL;

		reg_addr = QCE2204_PPE_PORT_EG_VLAN_TBL_ADDR +
			   port_id * QCE2204_PPE_PORT_EG_VLAN_TBL_INC;

		ret = regmap_read(priv->regmap, reg_addr, &reg_val);
		if (ret)
			return ret;

		reg_val &= ~QCE2204_PPE_PORT_EG_VLAN_TBL_PORT_VLAN_TYPE;
		reg_val |= FIELD_PREP(QCE2204_PPE_PORT_EG_VLAN_TBL_PORT_VLAN_TYPE,
				      cfg->port_role ? 1 : 0);

		return regmap_write(priv->regmap, reg_addr, reg_val);
	}

	return -EINVAL;
}

/**
 * qce2204_setup_8021q_global - Setup global 8021Q configuration
 * @priv: QCE2204 private data
 *
 * Configures global settings for DSA 8021Q tagging (called once):
 * 1. Set VLAN TPID to 0x8100 for ingress/egress
 * 2. Set CPU port as core port (QinQ role)
 *
 * Return: 0 on success, negative error code on failure
 */
int qce2204_setup_8021q_global(struct qce2204_priv *priv)
{
	struct qce2204_ppe_vlan_tpid_cfg tpid_cfg = {};
	struct qce2204_ppe_port_vlan_role_cfg role_cfg = {};
	int ret;

	dev_info(priv->dev, "Setting up global 8021Q configuration\n");

	/* Step 1: Configure VLAN TPID for ingress */
	tpid_cfg.ctpid = 0x8100;
	tpid_cfg.stpid = 0x8100;
	tpid_cfg.ctpid_ext = 0x8100;
	tpid_cfg.stpid_ext = 0x8100;
	tpid_cfg.ctpid_map = 0x5;
	tpid_cfg.stpid_map = 0xa;

	ret = qce2204_ppe_vlan_tpid_set(priv, QCE2204_PPE_INGRESS, &tpid_cfg);
	if (ret) {
		dev_err(priv->dev, "Failed to set ingress VLAN TPID: %d\n", ret);
		return ret;
	}

	/* Configure VLAN TPID for egress */
	ret = qce2204_ppe_vlan_tpid_set(priv, QCE2204_PPE_EGRESS, &tpid_cfg);
	if (ret) {
		dev_err(priv->dev, "Failed to set egress VLAN TPID: %d\n", ret);
		return ret;
	}

	/* Step 2: Set CPU port as core port (QinQ role) */
	role_cfg.port_role = true;  /* 1 = core port */

	ret = qce2204_ppe_port_vlan_role_set(priv, QCE2204_CPU_PORT_ID,
					      QCE2204_PPE_INGRESS, &role_cfg);
	if (ret) {
		dev_err(priv->dev, "Failed to set CPU port ingress role: %d\n", ret);
		return ret;
	}

	ret = qce2204_ppe_port_vlan_role_set(priv, QCE2204_CPU_PORT_ID,
					      QCE2204_PPE_EGRESS, &role_cfg);
	if (ret) {
		dev_err(priv->dev, "Failed to set CPU port egress role: %d\n", ret);
		return ret;
	}

	dev_info(priv->dev, "Global 8021Q configuration completed\n");
	return 0;
}

/**
 * qce2204_teardown_8021q_global - Teardown global 8021Q configuration
 * @priv: QCE2204 private data
 *
 * Restores global settings (called once when switching protocols):
 * 1. Restore CPU port to edge port (default role)
 * 2. Restore VLAN TPID to default values (0x88a8/0x8100)
 *
 * Return: 0 on success, negative error code on failure
 */
int qce2204_teardown_8021q_global(struct qce2204_priv *priv)
{
	struct qce2204_ppe_vlan_tpid_cfg tpid_cfg = {};
	struct qce2204_ppe_port_vlan_role_cfg role_cfg = {};
	int ret;

	dev_info(priv->dev, "Tearing down global 8021Q configuration\n");

	/* Step 1: Restore CPU port to edge port (default role) */
	role_cfg.port_role = false;  /* 0 = edge port */

	ret = qce2204_ppe_port_vlan_role_set(priv, QCE2204_CPU_PORT_ID,
					      QCE2204_PPE_INGRESS, &role_cfg);
	if (ret) {
		dev_err(priv->dev, "Failed to restore CPU port ingress role: %d\n", ret);
		return ret;
	}

	ret = qce2204_ppe_port_vlan_role_set(priv, QCE2204_CPU_PORT_ID,
					      QCE2204_PPE_EGRESS, &role_cfg);
	if (ret) {
		dev_err(priv->dev, "Failed to restore CPU port egress role: %d\n", ret);
		return ret;
	}

	/* Step 2: Restore VLAN TPID to default values for ingress */
	tpid_cfg.ctpid = 0x8100;
	tpid_cfg.stpid = 0x88a8;
	tpid_cfg.ctpid_ext = 0x8100;
	tpid_cfg.stpid_ext = 0x88a8;
	tpid_cfg.ctpid_map = 0x5;
	tpid_cfg.stpid_map = 0xa;

	ret = qce2204_ppe_vlan_tpid_set(priv, QCE2204_PPE_INGRESS, &tpid_cfg);
	if (ret) {
		dev_err(priv->dev, "Failed to restore ingress VLAN TPID: %d\n", ret);
		return ret;
	}

	/* Restore VLAN TPID for egress */
	ret = qce2204_ppe_vlan_tpid_set(priv, QCE2204_PPE_EGRESS, &tpid_cfg);
	if (ret) {
		dev_err(priv->dev, "Failed to restore egress VLAN TPID: %d\n", ret);
		return ret;
	}

	dev_info(priv->dev, "Global 8021Q configuration restored\n");
	return 0;
}

/**
 * qce2204_setup_8021q_tagging - Setup 8021Q tagging for a port
 * @priv: QCE2204 private data
 * @port: Port number (user port 1-4)
 *
 * Configures per-port VLAN translation rules for DSA 8021Q tagging:
 * 1. Set user port as core port (QinQ role)
 * 2. User port RX: Add standalone VID as SVLAN, forward to CPU port
 * 3. CPU port TX: Remove standalone VID, forward to user port
 *
 * Note: Global configuration (VLAN TPID and CPU port role) should be
 * done once via qce2204_setup_8021q_global() before calling this function.
 *
 * Return: 0 on success, negative error code on failure
 */
int qce2204_setup_8021q_tagging(struct qce2204_priv *priv, int port)
{
	struct qce2204_ppe_in_vlan_xlt_cfg xlt_cfg = {};
	struct qce2204_ppe_port_vlan_role_cfg role_cfg = {};
	struct qce2204_port_ppe_res *port_res;
	int ret;

	if (port >= QCE2204_NUM_PORTS) {
		dev_err(priv->dev, "Invalid port %d for 8021Q tagging\n", port);
		return -EINVAL;
	}

	port_res = &priv->port_ppe_res[port];
	if (!port_res->allocated) {
		dev_err(priv->dev, "Port %d PPE resources not allocated\n", port);
		return -EINVAL;
	}

	dev_info(priv->dev, "Setting up 8021Q tagging for port %d (VID 0x%x)\n",
		 port, port_res->standalone_vid);

	/* Step 1: Set user port as core port (QinQ role) */
	role_cfg.port_role = true;  /* 1 = core port */

	ret = qce2204_ppe_port_vlan_role_set(priv, port,
					      QCE2204_PPE_INGRESS, &role_cfg);
	if (ret) {
		dev_err(priv->dev, "Failed to set port %d ingress role: %d\n", port, ret);
		return ret;
	}

	ret = qce2204_ppe_port_vlan_role_set(priv, port,
					      QCE2204_PPE_EGRESS, &role_cfg);
	if (ret) {
		dev_err(priv->dev, "Failed to set port %d egress role: %d\n", port, ret);
		return ret;
	}

	/* Step 2: Configure user port RX rule: Add standalone VID for one tagged frame */
	memset(&xlt_cfg, 0, sizeof(xlt_cfg));
	xlt_cfg.port_id = port;
	xlt_cfg.svid_fmt = 0x4;		/* Match one Tagged */
	xlt_cfg.cvid_fmt = 0x1;		/* Match Untag */
	xlt_cfg.svid_xlt_cmd = 1;	/* Add SVLAN */
	xlt_cfg.svid_xlt = port_res->standalone_vid;
	xlt_cfg.cvid_xlt_cmd = 3;	/* Keep CVLAN from orginal outer VLAN */
	xlt_cfg.cpcp_xlt_cmd = 6;	/* To add CVLAN tag, also means pcp from spcp */
	xlt_cfg.dest_valid = true;
	xlt_cfg.dest_info = QCE2204_PPE_DEST_INFO(QCE2204_PPE_DEST_INFO_PORT_ID,
						   QCE2204_CPU_PORT_ID);

	ret = qce2204_ppe_vlan_in_vlan_xlt_set(priv, port_res->in_vlan_xlt_idx, &xlt_cfg);
	if (ret) {
		dev_err(priv->dev, "Failed to set user port %d RX VLAN translation1: %d\n",
			port, ret);
		return ret;
	}

	/* Step 3: Configure user port RX rule: Add standalone VID for untagged frame */
	memset(&xlt_cfg, 0, sizeof(xlt_cfg));
	xlt_cfg.port_id = port;
	xlt_cfg.svid_fmt = 0x1;		/* Match Untag */
	xlt_cfg.cvid_fmt = 0x1;		/* Match Untag */
	xlt_cfg.svid_xlt_cmd = 1;	/* Add SVLAN */
	xlt_cfg.svid_xlt = port_res->standalone_vid;
	xlt_cfg.dest_valid = true;
	xlt_cfg.dest_info = QCE2204_PPE_DEST_INFO(QCE2204_PPE_DEST_INFO_PORT_ID,
						   QCE2204_CPU_PORT_ID);

	ret = qce2204_ppe_vlan_in_vlan_xlt_set(priv, port_res->in_vlan_xlt_idx + 1, &xlt_cfg);
	if (ret) {
		dev_err(priv->dev, "Failed to set user port %d RX VLAN translation2: %d\n",
			port, ret);
		return ret;
	}

	/* Step 4: Configure CPU port TX rule: Remove standalone VID */
	memset(&xlt_cfg, 0, sizeof(xlt_cfg));
	xlt_cfg.port_id = QCE2204_CPU_PORT_ID;
	xlt_cfg.svid_fmt = 0x7;		/* Match any SVLAN format */
	xlt_cfg.cvid_fmt = 0x7;		/* Match any CVLAN format */
	xlt_cfg.svid_inc = true;	/* Match specific SVID */
	xlt_cfg.svid = port_res->standalone_vid;
	xlt_cfg.svid_xlt_cmd = 2;	/* Remove SVLAN */
	xlt_cfg.dest_valid = true;
	xlt_cfg.dest_info = QCE2204_PPE_DEST_INFO(QCE2204_PPE_DEST_INFO_PORT_ID, port);

	ret = qce2204_ppe_vlan_in_vlan_xlt_set(priv, port_res->cpu_in_vlan_xlt_idx, &xlt_cfg);
	if (ret) {
		dev_err(priv->dev, "Failed to set CPU port TX VLAN translation for port %d: %d\n",
			port, ret);
		/* Cleanup user port rule */
		memset(&xlt_cfg, 0, sizeof(xlt_cfg));
		qce2204_ppe_vlan_in_vlan_xlt_set(priv, port_res->in_vlan_xlt_idx, &xlt_cfg);
		return ret;
	}

	dev_info(priv->dev, "8021Q tagging configured for port %d\n", port);
	return 0;
}

/**
 * qce2204_teardown_8021q_tagging - Teardown 8021Q tagging for a port
 * @priv: QCE2204 private data
 * @port: Port number (user port 1-4)
 *
 * Removes per-port VLAN translation rules and restores port settings:
 * 1. Clear VLAN translation rules
 * 2. Restore user port to edge port (default role)
 *
 * Note: Global configuration (VLAN TPID and CPU port role) should be
 * restored once via qce2204_teardown_8021q_global() after all ports are done.
 *
 * Return: 0 on success, negative error code on failure
 */
int qce2204_teardown_8021q_tagging(struct qce2204_priv *priv, int port)
{
	struct qce2204_ppe_in_vlan_xlt_cfg xlt_cfg = {};
	struct qce2204_ppe_port_vlan_role_cfg role_cfg = {};
	struct qce2204_port_ppe_res *port_res;
	int ret;

	if (port >= QCE2204_NUM_PORTS) {
		dev_err(priv->dev, "Invalid port %d for 8021Q tagging\n", port);
		return -EINVAL;
	}

	port_res = &priv->port_ppe_res[port];
	if (!port_res->allocated) {
		dev_dbg(priv->dev, "Port %d PPE resources not allocated\n", port);
		return 0;
	}

	dev_info(priv->dev, "Tearing down 8021Q tagging for port %d\n", port);

	/* Step 1: Clear user port RX rule */
	memset(&xlt_cfg, 0, sizeof(xlt_cfg));
	ret = qce2204_ppe_vlan_in_vlan_xlt_set(priv, port_res->in_vlan_xlt_idx, &xlt_cfg);
	if (ret) {
		dev_err(priv->dev, "Failed to clear user port %d RX VLAN translation1: %d\n",
			port, ret);
		return ret;
	}

	ret = qce2204_ppe_vlan_in_vlan_xlt_set(priv, port_res->in_vlan_xlt_idx + 1, &xlt_cfg);
	if (ret) {
		dev_err(priv->dev, "Failed to clear user port %d RX VLAN translation2: %d\n",
			port, ret);
		return ret;
	}

	/* Clear CPU port TX rule */
	memset(&xlt_cfg, 0, sizeof(xlt_cfg));
	ret = qce2204_ppe_vlan_in_vlan_xlt_set(priv, port_res->cpu_in_vlan_xlt_idx, &xlt_cfg);
	if (ret) {
		dev_err(priv->dev, "Failed to clear CPU port TX VLAN translation for port %d: %d\n",
			port, ret);
		return ret;
	}

	/* Step 2: Restore user port to edge port (default role) */
	role_cfg.port_role = false;  /* 0 = edge port */

	ret = qce2204_ppe_port_vlan_role_set(priv, port,
					      QCE2204_PPE_INGRESS, &role_cfg);
	if (ret) {
		dev_err(priv->dev, "Failed to restore port %d ingress role: %d\n", port, ret);
		return ret;
	}

	ret = qce2204_ppe_port_vlan_role_set(priv, port,
					      QCE2204_PPE_EGRESS, &role_cfg);
	if (ret) {
		dev_err(priv->dev, "Failed to restore port %d egress role: %d\n", port, ret);
		return ret;
	}

	dev_info(priv->dev, "8021Q tagging torn down for port %d\n", port);
	return 0;
}

/**
 * qce2204_ppe_stp_state_set - Configure STP state for a port
 * @priv: Driver private data
 * @port_id: Port index
 * @cfg: STP state configuration
 *
 * Configures the Spanning Tree Protocol state for the specified port.
 * The STP state controls packet forwarding behavior:
 * - 0 (Disabled): Port is disabled
 * - 1 (Blocking): Port blocks all traffic except BPDUs
 * - 2 (Learning): Port learns MAC addresses but doesn't forward
 * - 3 (Forwarding): Port forwards all traffic (default)
 *
 * Return: 0 on success, negative error code on failure
 */
int qce2204_ppe_stp_state_set(struct qce2204_priv *priv,
			       u32 port_id,
			       struct qce2204_ppe_stp_state_cfg *cfg)
{
	u32 reg_addr;
	u32 reg_val;
	int ret;

	if (!priv || !cfg)
		return -EINVAL;

	if (port_id >= QCE2204_PPE_CST_STATE_ENTRIES)
		return -EINVAL;

	/* Calculate register address */
	reg_addr = QCE2204_PPE_CST_STATE_ADDR +
		   port_id * QCE2204_PPE_CST_STATE_INC;

	/* Read current value */
	ret = regmap_read(priv->regmap, reg_addr, &reg_val);
	if (ret)
		return ret;

	/* Modify PORT_STATE field */
	reg_val &= ~QCE2204_PPE_CST_STATE_PORT_STATE;
	reg_val |= FIELD_PREP(QCE2204_PPE_CST_STATE_PORT_STATE, cfg->stp_state);

	/* Write back */
	return regmap_write(priv->regmap, reg_addr, reg_val);
}

/**
 * qce2204_ppe_tdm_depth_init - Configure TDM depth for TM and BM schedulers
 * @priv: QCE2204 private data
 *
 * This function must be called during the switch reset sequence, after
 * GCC_SWITCH_CFG_ARES is deasserted (CSR accessible) but before
 * GCC_SWITCH_CORE_ARES is deasserted.
 *
 * Design recommended TDM config sequence:
 * 1. Set TM TDM depth = 62 (TM TDM_DEPTH_CFG @ 0x400000)
 * 2. Set BM TDM depth = 248, enable scheduler (BM SCH_CTRL @ 0x00b000)
 * 3. Set BM TDM offset = 142, depth = 248, enable scheduler (BM SCH_CTRL @ 0x00b000)
 *
 * Return: 0 on success, negative error code on failure
 */
int qce2204_ppe_tdm_depth_init(struct qce2204_priv *priv)
{
	struct device *dev = priv->dev;
	int ret;

	/* Set TM TDM depth */
	ret = regmap_write(priv->regmap, QCE2204_PPE_PSCH_SCH_DEPTH_CFG_ADDR,
			   FIELD_PREP(QCE2204_PPE_PSCH_SCH_DEPTH_CFG_SCH_DEPTH,
				      ARRAY_SIZE(qce2204_ppe_sch_qm_config)));
	if (ret) {
		dev_err(dev, "Failed to set TM TDM depth: %d\n", ret);
		return ret;
	}

	/* Set BM TDM depth, enable scheduler */
	ret = regmap_write(priv->regmap, QCE2204_PPE_BM_SCH_CTRL_ADDR,
			   FIELD_PREP(QCE2204_PPE_BM_SCH_CTRL_SCH_EN, true) |
			   FIELD_PREP(QCE2204_PPE_BM_SCH_CTRL_SCH_DEPTH,
				      ARRAY_SIZE(qce2204_ppe_sch_bm_config)));
	if (ret) {
		dev_err(dev, "Failed to set BM TDM depth: %d\n", ret);
		return ret;
	}

	/* Set BM TDM offset and depth, enable scheduler, per design sequence */
	ret = regmap_write(priv->regmap, QCE2204_PPE_BM_SCH_CTRL_ADDR,
			   FIELD_PREP(QCE2204_PPE_BM_SCH_CTRL_SCH_EN, true) |
			   FIELD_PREP(QCE2204_PPE_BM_SCH_CTRL_SCH_OFFSET,
				      QCE2204_PPE_BM_TDM_OFFSET) |
			   FIELD_PREP(QCE2204_PPE_BM_SCH_CTRL_SCH_DEPTH,
				      ARRAY_SIZE(qce2204_ppe_sch_bm_config)));
	if (ret) {
		dev_err(dev, "Failed to set BM TDM ctrl: %d\n", ret);
		return ret;
	}

	dev_info(dev, "TDM depth initialized (TM depth=%zu, BM depth=%zu, offset=%d)\n",
		 ARRAY_SIZE(qce2204_ppe_sch_qm_config),
		 ARRAY_SIZE(qce2204_ppe_sch_bm_config),
		 QCE2204_PPE_BM_TDM_OFFSET);
	return 0;
}

int qce2204_ppe_hw_init(struct qce2204_priv *priv)
{
	int ret;

	dev_info(priv->dev, "Initializing PPE hardware\n");

	ret = qce2204_ppe_config_bm(priv);
	if (ret)
		return ret;

	ret = qce2204_ppe_config_qm(priv);
	if (ret)
		return ret;

	ret = qce2204_ppe_config_scheduler(priv);
	if (ret)
		return ret;

	ret = qce2204_ppe_queue_dest_init(priv);
	if (ret)
		return ret;

	ret = qce2204_ppe_port_config_init(priv);
	if (ret)
		return ret;

	ret = qce2204_ppe_rss_hash_init(priv);
	if (ret)
		return ret;

	ret = qce2204_ppe_bridge_init(priv);
	if (ret)
		return ret;

	ret = qce2204_ppe_athtag_init(priv);
	if (ret)
		return ret;

	ret = qce2204_mdio_backpressure_init(priv);
	if (ret) {
		return ret;
	}

	dev_info(priv->dev, "PPE hardware initialized successfully\n");
	return 0;
}
