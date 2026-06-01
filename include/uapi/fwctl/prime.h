/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. */

/*
 * Header exposes userspace/kernel module API for IO_CTL
 */

#ifndef __QCOM_PRIME_DEV_H__
#define __QCOM_PRIME_DEV_H__

#include <linux/types.h>

enum prime_irq2l_enum {
	PRIME_IRQ2L_ERROR = 0,
	PRIME_IRQ2L_BOOT_DONE = 1,
	PRIME_IRQ2L_TASK_DONE = 2,
	PRIME_IRQ2L_MISC = 3,
	PRIME_IRQ2L_MAX = PRIME_IRQ2L_MISC
};

enum prime_irq_pend_enum {
	PRIME_IRQ_PEND_ERROR = 0,
	PRIME_IRQ_PEND_IPCC = 1,
	PRIME_IRQ_PEND_HIGH_PRI = 2,
	PRIME_IRQ_PEND_LOW_PRI = 3,
	PRIME_IRQ_PEND_TOP = 4,
	PRIME_IRQ_PEND_MAX = PRIME_IRQ_PEND_LOW_PRI
};


enum prime_mem_regions_enum {
	PRIME_MEM_REGION_MODEL,
	PRIME_MEM_REGION_TENSOR,
	PRIME_MEM_REGION_SWIF,
	PRIME_MEM_REGION_PDMEM,
	PRIME_MEM_REGION_MLM,
	__PRIME_MEM_REGION_MAX
};

enum prime_mem_dma_map_enum {
	PRIME_MEM_DMA_ALLOC,
	PRIME_MEM_DMA_FREE
};

enum prime_dma_direction {
	PRIME_DMA_BIDIRECTIONAL = 0,
	PRIME_DMA_TO_DEVICE = 1,
	PRIME_DMA_FROM_DEVICE = 2,
};

struct prime_mem_region {
	/* Populated by App */
	enum prime_mem_regions_enum region;
	/* Populated by IOCTL call */
	__u64 base;
	__kernel_size_t sz;
};

struct prime_mem_dma_map {
	/* Populated by App */
	enum prime_mem_regions_enum region;
	enum prime_mem_dma_map_enum action;
	enum prime_dma_direction direction;
	int dma_fd;
	/* Populated by IOCTL call on ALLOC, provided by App on FREE */
	__u64 map_key;
	__u64 device_dma_base_addr;
};

struct prime_event_subscribe {
	/* Populated by App: 1 subscribe this FD, 0 unsubscribe this FD */
	__u32 enable;
	/* Reserved, must be 0 */
	__u32 flags;
};

struct prime_irq_event {
	/* Populated by kernel and returned by read() on /dev/prime1 */
	__u32 irq;
	/* Reserved for future event metadata */
	__u32 flags;
	/* Monotonic sequence number assigned by kernel */
	__u64 seqno;
	/* Event timestamp (CLOCK_MONOTONIC) in ns */
	__u64 timestamp_ns;
};

#define PRIME_IRQ_EVENT_FLAG_OVERFLOW	(1U << 0)

enum prime_mem_pil_action {
	PRIME_MEM_PIL_PREPARE,
	PRIME_MEM_PIL_RELEASE,
	PRIME_MEM_PIL_SYNC
};

struct prime_mem_pil_map {
	/* Populated by App */
	enum prime_mem_regions_enum region;
	enum prime_mem_pil_action action;
	enum prime_dma_direction direction;
	__u64 offset;		/* Offset within the region */
	__kernel_size_t size;	/* Size of sub-region */
	/* Populated by IOCTL call on PREPARE, provided by App on RELEASE */
	__u64 map_key;
	__u64 device_dma_base_addr;
};

enum prime_clk_level {
	PRIME_CLK_LEVEL_OFF = 0,
	PRIME_CLK_LEVEL_LSVS = 1,
	PRIME_CLK_LEVEL_SVS = 2,
	PRIME_CLK_LEVEL_SVS_L1 = 3,
	PRIME_CLK_LEVEL_NOM = 4,
	PRIME_CLK_LEVEL_TURBO = 5,
};

struct prime_clk_bw_config {
	/* Populated by App */
	enum prime_clk_level clk_level;	/* Clock level */
	__u32 ddr_bw;	/* DDR bandwidth in KB/s */
	__u32 cpu_bw;	/* CPU bandwidth in KB/s */
};

#define PRIME_MEM_MAGIC 'P'
#define PRIME_MEM_GET_REGION _IOWR(PRIME_MEM_MAGIC, 0x1, struct prime_mem_region)
#define PRIME_MEM_EVENT_SUBSCRIBE _IOW(PRIME_MEM_MAGIC, 0x10, struct prime_event_subscribe)
#define PRIME_MEM_PIL _IOWR(PRIME_MEM_MAGIC, 0x20, struct prime_mem_pil_map)
#define PRIME_MEM_DMA _IOWR(PRIME_MEM_MAGIC, 0x30, struct prime_mem_dma_map)
#define PRIME_MEM_SET_CLK_BW _IOW(PRIME_MEM_MAGIC, 0x40, struct prime_clk_bw_config)

#endif /* PRIME_DEV_H */
