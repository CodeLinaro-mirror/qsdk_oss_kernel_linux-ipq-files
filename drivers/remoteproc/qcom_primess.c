// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. */

#include <linux/dma-direction.h>
#include <linux/dma-buf.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioctl.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>
#include <linux/firmware.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/sched/signal.h>
#include <linux/iopoll.h>
#include <linux/delay.h>
#include <linux/version.h>
#include <linux/spinlock.h>
#include <linux/ktime.h>

#ifndef BUILDROOT
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/interconnect.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/soc/qcom/mdt_loader.h>
#if IS_ENABLED(CONFIG_QCOM_TMELCOM)
#include <linux/tmelcom_ipc.h>
#endif
#endif

#include <uapi/fwctl/prime.h>

#define IPQ9650_GCC_BASE 0x01800000
#define IPQ9676_GCC_SIZE 0x40000

/* GCC register offsets (base: ipq9650_GCC_BASE) */
#define GCC_SNOC_PRIMESS_AXIM_CBCR		0x2E0E0
#define GCC_CNOC_PRIMESS_AHBS_CBCR		0x310BC
#define PRIME_IRQ_EVENT_RING_SIZE		4
#define PRIME_NUM_DEVS				3

struct iuss_dev {
	struct device *dev;
	void __iomem *reg_ss_base;      /* HWIO base address for IUSS */
	void __iomem *reg_cmn_base;     /* HWIO base address for IUSS common CSR */
	void __iomem *reg_core_csr[1];  /* HWIO base address for CoreX IUSS CSRs */
	void __iomem *pdmem_base;       /* Virtual PDMEM base address */
	u32 __iomem *intf_table;        /* Virtual address of interface table in PDMEM */
};

static const struct file_operations prime_mem_fops;

struct qcom_prime {
	void __iomem *reg_ss_base;   /* HWIO base address for IUSS */

	void __iomem *pdmem_base;       /* Virtual PDMEM base address */
	phys_addr_t pdmem_base_phys;    /* Physical address of PDMEM base */
	size_t pdmem_size;              /* PDMEM size in bytes */
	u32 __iomem *intf_table;        /* Virtual address of interface table in PDMEM */
	phys_addr_t intf_table_phys;    /* Physical address of interface table */

	phys_addr_t mem_mlm_base_phys;  /* Physical address of ML TOP memory */
	size_t mem_mlm_size;            /* Size of MLM */

	char *name;                     /* Name of IUSS for debug */

	struct rproc *rproc;
	struct device *dev;   /* Device pointer */

	int prime_task_done_irq; /* Input IRQ line to ARM from devtree*/
	int prime_boot_done_irq; /* Input IRQ line to ARM from devtree*/
	int prime_error_irq; /* Input IRQ line to ARM from devtree*/
	int prime_wdog_irq; /* Input IRQ line to ARM from devtree*/
	int prime_misc_irq; /* Input IRQ line to ARM from devtree*/

	int pas_id;
	bool tmelcom_support;

	struct completion start_done;

	void *metadata;
	size_t metadata_len;

	/* PRIME lifecycle and SWIF mapping protection */
	/* True after successful prime_start(), false after prime_stop() */
	bool prime_started;
	atomic_t swif_mmap_count;        /* Count of active SWIF mappings */

	phys_addr_t mem_phys;  /* Base address of PIL carveout */
	phys_addr_t mem_reloc; /* Copy of mem_phys */
	/* Base address of model storage. Must be within mem_phys < mem_phys+mem_size */
	phys_addr_t mem_model_base;
	void *mem_region; /* Kernel driver vm remapped region*/
	size_t mem_size; /* Carveout size*/
	size_t mem_model_size; /*Size of model memory within carveout */

	struct cdev cdev;
	dev_t cdev_dev;
	struct class *cdev_class;

	struct iuss_dev iuss_dev;

	/* IRQ forwarding via ring queue drained by blocking read(/dev/prime1) */
	spinlock_t irq_event_lock;
	struct prime_irq_event irq_event_ring[PRIME_IRQ_EVENT_RING_SIZE];
	u32 irq_event_head;
	u32 irq_event_tail;
	u32 irq_event_count;
	bool irq_event_overflow;
	u64 irq_event_seqno;
	wait_queue_head_t irq_event_wq;
	struct mutex irq_sub_lock;
	struct file *irq_sub_file;
	pid_t irq_sub_owner_tgid;

	struct clk *core_clk;
	struct icc_path *icc_ddr;
	struct icc_path *icc_cpu;

	/* Clock state tracking */
	bool clk_enabled;
	enum prime_clk_level clk_level;
	u32 clk_ddr_bw;
	u32 clk_cpu_bw;

	int mem_count;

	bool config_mmu;

	/* For PRIMESS_RIF */
	u32 primess_rif_last_val;

	/* DMA-BUF map tracking (opaque key -> kernel mapping state) */
	struct mutex dma_map_lock;
	struct list_head dma_mappings;
	atomic64_t dma_map_key_next;

	/* PIL mapping tracking (opaque key -> MODEL sub-region mapping state) */
	struct mutex pil_map_lock;
	struct list_head pil_mappings;
	atomic64_t pil_map_key_next;
};

struct prime_dma_mapping {
	struct list_head node;
	u64 map_key;
	pid_t owner_tgid;
	enum dma_data_direction direction;
	u64 device_dma_base_addr;
	struct dma_buf *dma_buf;
	struct dma_buf_attachment *dma_attachment;
	struct sg_table *sg_table;
};

struct prime_pil_mapping {
	struct list_head node;
	u64 map_key;
	pid_t owner_tgid;
	u64 offset;
	size_t size;
	enum dma_data_direction direction;
	dma_addr_t device_dma_base_addr;
};

//TODO: Move these definitions to device tree?
/* PRIME SS Condition Status Registers (Relative Byte Offset) */
enum PrimessCsr {
	PRIMESS_IRQ2L_EN                    = 0x54,
	PRIMESS_IRQ2L_PEND_CLR              = 0x58,
	PRIMESS_IRQ2L_PEND_SET              = 0x5c,
	PRIMESS_IRQ2L_PEND_STAT             = 0x60,
	PRIMESS_IRQ_IN_MASK                 = 0x64,
	PRIMESS_IRQ_IN_EN                   = 0x68,
	PRIMESS_IUSS_IRQ_PEND_WDATA_WORD0   = 0x6C,
	PRIMESS_VOTER_FW_VOTE_DONE          = 0x4030,
	PRIMESS_VOTER_FW_VOTE_SET           = 0x4040,
	PRIMESS_VOTER_FW_VOTE_CLEAR         = 0x4050,
	PRIMESS_VOTER_FW_EN_CFG             = 0x4090,
};

#define FW_PRIMESS_REG_ADDR(_base, _offset) ((u32 *)((uintptr_t)(_base) + (_offset)))

/*
 * QCOM PRIME Device Driver.
 */

struct prime_data {
	const char *firmware_name;
	int pas_id;
	const char *ssr_name;
	const char *sysmon_name;
	bool auto_boot;
	const char *qmp_name;
	bool config_mmu;
	bool tmelcom_support;
};

static void prime_irq_queue_flush(struct qcom_prime *prime)
{
	unsigned long flags;

	spin_lock_irqsave(&prime->irq_event_lock, flags);
	prime->irq_event_head = 0;
	prime->irq_event_tail = 0;
	prime->irq_event_count = 0;
	prime->irq_event_overflow = false;
	spin_unlock_irqrestore(&prime->irq_event_lock, flags);
}

static void prime_irq_event_notify(struct qcom_prime *prime, int irq_id)
{
	struct prime_irq_event evt;
	unsigned long flags;

	memset(&evt, 0, sizeof(evt));
	evt.irq = irq_id;
	evt.timestamp_ns = ktime_get_ns();

	spin_lock_irqsave(&prime->irq_event_lock, flags);

	prime->irq_event_seqno++;
	evt.seqno = prime->irq_event_seqno;

	if (prime->irq_event_count == PRIME_IRQ_EVENT_RING_SIZE) {
		prime->irq_event_tail = (prime->irq_event_tail + 1) %
					PRIME_IRQ_EVENT_RING_SIZE;
		prime->irq_event_count--;
		prime->irq_event_overflow = true;
	}
	if (prime->irq_event_overflow) {
		evt.flags |= PRIME_IRQ_EVENT_FLAG_OVERFLOW;
		prime->irq_event_overflow = false;
	}
	prime->irq_event_ring[prime->irq_event_head] = evt;
	prime->irq_event_head = (prime->irq_event_head + 1) %
				PRIME_IRQ_EVENT_RING_SIZE;
	prime->irq_event_count++;

	wake_up_interruptible(&prime->irq_event_wq);

	spin_unlock_irqrestore(&prime->irq_event_lock, flags);
}

static int prime_irq_event_pop(struct qcom_prime *prime, struct prime_irq_event *evt)
{
	unsigned long flags;

	spin_lock_irqsave(&prime->irq_event_lock, flags);
	if (prime->irq_event_count == 0) {
		spin_unlock_irqrestore(&prime->irq_event_lock, flags);
		return -EAGAIN;
	}

	*evt = prime->irq_event_ring[prime->irq_event_tail];
	prime->irq_event_tail = (prime->irq_event_tail + 1) %
				PRIME_IRQ_EVENT_RING_SIZE;
	prime->irq_event_count--;
	spin_unlock_irqrestore(&prime->irq_event_lock, flags);

	return 0;
}

static bool prime_irq_event_available(struct qcom_prime *prime)
{
	bool available;
	unsigned long flags;

	spin_lock_irqsave(&prime->irq_event_lock, flags);
	available = (prime->irq_event_count > 0);
	spin_unlock_irqrestore(&prime->irq_event_lock, flags);
	return available;
}

static bool prime_irq_reader_is_subscribed(struct qcom_prime *prime, struct file *file)
{
	return READ_ONCE(prime->irq_sub_file) == file;
}

static irqreturn_t prime_irq_boot_done(int irq, void *data)
{
	struct qcom_prime *prime = data;
	int irq_id = PRIME_IRQ2L_BOOT_DONE;

	dev_dbg(prime->dev, "PRIME CMD/RSP IRQ handler (%d) triggered!\n", irq);
	writel_relaxed(1 << (irq_id), prime->reg_ss_base + PRIMESS_IRQ2L_PEND_CLR);
	complete(&prime->start_done);
	prime_irq_event_notify(prime, irq_id);
	return IRQ_HANDLED;
}

static irqreturn_t prime_irq_task_done(int irq, void *data)
{
	struct qcom_prime *prime = data;
	int irq_id = PRIME_IRQ2L_TASK_DONE;

	dev_dbg(prime->dev, "PRIME Task Complete IRQ handler (%d) triggered!\n", irq);
	writel_relaxed(1 << (irq_id), prime->reg_ss_base + PRIMESS_IRQ2L_PEND_CLR);
	prime_irq_event_notify(prime, irq_id);

	return IRQ_HANDLED;
}

static irqreturn_t prime_irq_error(int irq, void *data)
{
	struct qcom_prime *prime = data;
	int irq_id = PRIME_IRQ2L_ERROR;

	dev_err(prime->dev, "PRIME Error IRQ handler (%d) triggered!\n", irq);
	writel_relaxed(1 << (irq_id), prime->reg_ss_base + PRIMESS_IRQ2L_PEND_CLR);
	complete(&prime->start_done);
	prime_irq_event_notify(prime, irq_id);

	return IRQ_HANDLED;
}

static irqreturn_t prime_irq_wdog(int irq, void *data)
{
	struct qcom_prime *prime = data;
	int irq_id = PRIME_IRQ2L_ERROR; //TODO: fix this

	dev_err(prime->dev, "PRIME Watchdog IRQ handler (%d) triggered!\n", irq);
	/* Note: Watchdog interrupt cannot be cleared without additional action */

	prime_irq_event_notify(prime, irq_id);

	return IRQ_HANDLED;
}

static irqreturn_t prime_irq_misc(int irq, void *data)
{
	struct qcom_prime *prime = data;
	int irq_id = PRIME_IRQ2L_MISC;

	dev_dbg(prime->dev, "PRIME Misc IRQ handler (%d) triggered!\n", irq);
	writel_relaxed(1 << (irq_id), prime->reg_ss_base + PRIMESS_IRQ2L_PEND_CLR);

	prime_irq_event_notify(prime, irq_id);

	return IRQ_HANDLED;
}

static void mask_irqs(struct qcom_prime *prime)
{
	/* Disable IRQs in PRIME SS */
	writel_relaxed(0, prime->reg_ss_base + PRIMESS_IRQ2L_EN); // IUSS-> ARM
	writel_relaxed(0, prime->reg_ss_base + PRIMESS_IRQ_IN_EN); //ARM -> IUSS
}

static void unmask_irqs(struct qcom_prime *prime, u32 clr_mask)
{
	/* Enable IRQs in PRIME SS */
	writel_relaxed(clr_mask, prime->reg_ss_base + PRIMESS_IRQ2L_PEND_CLR);
	writel_relaxed(0xF, prime->reg_ss_base + PRIMESS_IRQ2L_EN); // IUSS-> ARM
	writel_relaxed(0xFF, prime->reg_ss_base + PRIMESS_IRQ_IN_EN); //ARM -> IUSS
}

/* Permission defines for sysfs attributes */
#define PRIME_ATTR_PERM_RW  0640  /* Read-Write */
#define PRIME_ATTR_PERM_RO  0440  /* Read-Only */
#define PRIME_ATTR_PERM_WO  0200  /* Write-Only */

/**
 *   Declare a R/W SysFs attribute corresponding to a register.
 *
 *   @_reg: Register name from prime_hwio_reg.h
 *   @_perm: Permission mode (PRIME_ATTR_PERM_RW, PRIME_ATTR_PERM_RO, or PRIME_ATTR_PERM_WO)
 */
#define DECL_PRIME_RW_ATTR(_reg, _perm) \
	static ssize_t _reg##_show(struct device *dev, \
				       struct device_attribute *attr, char *buf) \
	{ \
		struct qcom_prime *vp = dev_get_drvdata(dev); \
		u32 val = readl_relaxed(FW_PRIMESS_REG_ADDR(vp->reg_ss_base, (_reg))); \
		return scnprintf(buf, PAGE_SIZE, "0x%x\n", val); \
	} \
	static ssize_t _reg##_store(struct device *dev, \
					struct device_attribute *attr, \
					const char *buf, size_t len) \
	{ \
		struct qcom_prime *vp = dev_get_drvdata(dev); \
		u32 val; \
		if (kstrtou32(buf, 0, &val) != 0) { \
			if (len == sizeof(u32)) { \
				val = *(u32 *)buf; \
			} else if (len == sizeof(u8)) { \
				val = *(u8 *)buf; \
			} else { \
				return -EINVAL; \
			} \
		} \
		writel(val, FW_PRIMESS_REG_ADDR(vp->reg_ss_base, (_reg))); \
		return len; \
	} \
	static DEVICE_ATTR(_reg, (_perm), _reg##_show, _reg##_store)

/** Declare R/W SysFs attributes */
DECL_PRIME_RW_ATTR(PRIMESS_IRQ2L_PEND_CLR, PRIME_ATTR_PERM_WO);
DECL_PRIME_RW_ATTR(PRIMESS_IRQ2L_PEND_SET, PRIME_ATTR_PERM_WO);
DECL_PRIME_RW_ATTR(PRIMESS_IRQ2L_PEND_STAT, PRIME_ATTR_PERM_RO);
DECL_PRIME_RW_ATTR(PRIMESS_IUSS_IRQ_PEND_WDATA_WORD0, PRIME_ATTR_PERM_WO);

static ssize_t primess_rif_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct qcom_prime *vp = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "0x%08x\n", vp->primess_rif_last_val);
}

/**
 * PRIMESS_RIF - Simplified register interface
 *
 * Store function:
 *  - "ADDR:VAL": Writes VAL to register at ADDR
 *  - "ADDR": Reads from register at ADDR and stores the value internally
 *
 * Show function:
 *  - Returns the last value read by the store function
 */
static ssize_t primess_rif_store(struct device *dev, struct device_attribute *attr,
					     const char *buf, size_t len)
{
	struct qcom_prime *vp = dev_get_drvdata(dev);
	u32 addr, val;
	char *colon_pos;

	/* Enforce max input: "ADDR:VAL\n" = 10:10 chars + colon + newline */
	if (len > 22)
		return -EINVAL;

	/* Check if this is a write (contains ':') or read operation */
	colon_pos = strnchr(buf, len, ':');

	if (colon_pos) {
		/* Parse "ADDR:VAL" format */
		char addr_str[11], val_str[11];
		int addr_len = colon_pos - buf;

		if (addr_len > 10)
			return -EINVAL;

		memcpy(addr_str, buf, addr_len);
		addr_str[addr_len] = '\0';

		if (kstrtou32(addr_str, 0, &addr) != 0)
			return -EINVAL;

		/* Validate 4-byte alignment */
		if (addr & 0x3) {
			dev_err(dev, "Address 0x%08x is not 4-byte aligned\n", addr);
			return -EINVAL;
		}

		/* Parse value after colon */
		if (strscpy(val_str, colon_pos + 1, sizeof(val_str)) < 0)
			return -EINVAL;

		if (kstrtou32(val_str, 0, &val) != 0)
			return -EINVAL;

		/* Write to register */
		writel(val, FW_PRIMESS_REG_ADDR(vp->reg_ss_base, addr));

	} else {
		/* Parse "ADDR" format for reading */
		if (kstrtou32(buf, 0, &addr) != 0)
			return -EINVAL;

		/* Validate 4-byte alignment */
		if (addr & 0x3) {
			dev_err(dev, "Address 0x%08x is not 4-byte aligned\n", addr);
			return -EINVAL;
		}

		/* Read from register */
		val = readl_relaxed(FW_PRIMESS_REG_ADDR(vp->reg_ss_base, addr));

		dev_info(dev, "PRIMESS_RIF read: ADDR=0x%08x, VAL=0x%08x\n", addr, val);

		/* Update the shared value atomically */
		vp->primess_rif_last_val = val;
	}

	/* Return the number of bytes consumed */
	return len;
}

static DEVICE_ATTR(PRIMESS_RIF, PRIME_ATTR_PERM_RW, primess_rif_show, primess_rif_store);

#ifndef BUILDROOT

/* Clock level to frequency mapping - available for both BUILDROOT and non-BUILDROOT */
static u32 prime_get_clk(enum prime_clk_level clk_rate)
{
	switch (clk_rate) {
	case PRIME_CLK_LEVEL_OFF:
		return 0;
	case PRIME_CLK_LEVEL_LSVS:
		return 300000000;
	case PRIME_CLK_LEVEL_SVS:
		return 533000000;
	case PRIME_CLK_LEVEL_SVS_L1:
		return 600000000;
	case PRIME_CLK_LEVEL_NOM:
		return 806400000;
	case PRIME_CLK_LEVEL_TURBO:
		return 933000000;
	default:
		return 300000000;
	}
}

/* Bandwidth table */
static struct prime_bw_table_val {
	u32 mem_bw; /* prime-ddr (KB/s) */
	u32 cfg_bw; /* cpu-prime (KB/s) */
} prime_bw_table[PRIME_CLK_LEVEL_TURBO + 1] = {
	[PRIME_CLK_LEVEL_OFF]    = {      0,      0 },

	/* Linear scaling, 320 MB/S @ TURBO */
	[PRIME_CLK_LEVEL_LSVS]   = { 105421,  16452 },
	[PRIME_CLK_LEVEL_SVS]    = { 187085,  29262 },
	[PRIME_CLK_LEVEL_SVS_L1] = { 210842,  32904 },
	[PRIME_CLK_LEVEL_NOM]    = { 283443,  44261 },
	[PRIME_CLK_LEVEL_TURBO]  = { 327680,  51200 },
};

/* Forward declaration for prime_set_clk_bw (defined in non-BUILDROOT section) */
static int prime_set_clk_bw(struct qcom_prime *prime, enum prime_clk_level clk_level,
			    u32 ddr_bw, u32 cpu_bw);
#endif

/**
 * PRIMESS_CLK_BW - Clock and Bandwidth control sysfs attribute
 *
 * Read: Returns current clock level, DDR bandwidth, and CPU bandwidth
 * Write: Accepts 1 or 3 space-separated integers:
 *   - 1 argument: clk_level (looks up bandwidth from table)
 *   - 3 arguments: clk_level ddr_bw cpu_bw (explicit values)
 *
 * Note: For BUILDROOT builds, this is a NOP (no operation)
 */
static ssize_t PRIMESS_CLK_BW_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct qcom_prime *vp = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%d %u %u\n",
			 vp->clk_level, vp->clk_ddr_bw, vp->clk_cpu_bw);
}

static ssize_t PRIMESS_CLK_BW_store(struct device *dev, struct device_attribute *attr,
					const char *buf, size_t len)
{
#ifdef BUILDROOT
	/* NOP for BUILDROOT - just return success */
	dev_dbg(dev, "PRIMESS_CLK_BW: NOP in BUILDROOT mode\n");
	return len;
#else
	struct qcom_prime *vp = dev_get_drvdata(dev);
	int clk_level;
	u32 ddr_bw, cpu_bw;
	int ret;
	int num_args;

	/* Try to parse 3 arguments first */
	num_args = sscanf(buf, "%d %u %u", &clk_level, &ddr_bw, &cpu_bw);

	if (num_args == 1) {
		/* Only clock level provided - lookup bandwidth from table */

		/* Validate clock level is within valid range */
		if (clk_level < PRIME_CLK_LEVEL_OFF || clk_level > PRIME_CLK_LEVEL_TURBO) {
			dev_err(dev, "Invalid clock level %d (valid range: %d-%d)\n",
				clk_level, PRIME_CLK_LEVEL_OFF, PRIME_CLK_LEVEL_TURBO);
			return -EINVAL;
		}

		/* Lookup bandwidth values from table */
		ddr_bw = prime_bw_table[clk_level].mem_bw;
		cpu_bw = prime_bw_table[clk_level].cfg_bw;

		dev_dbg(dev, "PRIMESS_CLK_BW: clk_level=%d (looked up ddr_bw=%u, cpu_bw=%u)\n",
			clk_level, ddr_bw, cpu_bw);
	} else if (num_args == 3) {
		/* All three arguments provided - use explicit values */

		/* Validate clock level is within valid range */
		if (clk_level < PRIME_CLK_LEVEL_OFF || clk_level > PRIME_CLK_LEVEL_TURBO) {
			dev_err(dev, "Invalid clock level %d (valid range: %d-%d)\n",
				clk_level, PRIME_CLK_LEVEL_OFF, PRIME_CLK_LEVEL_TURBO);
			return -EINVAL;
		}

		dev_dbg(dev, "PRIMESS_CLK_BW: clk_level=%d, ddr_bw=%u, cpu_bw=%u (explicit)\n",
			clk_level, ddr_bw, cpu_bw);
	} else {
		/* Invalid number of arguments */
		dev_err(dev, "Invalid format. Expected: <clk_level> or <clk_level> <ddr_bw> <cpu_bw>\n");
		return -EINVAL;
	}

	/* Call prime_set_clk_bw with the determined values */
	ret = prime_set_clk_bw(vp, clk_level, ddr_bw, cpu_bw);
	if (ret) {
		dev_err(dev, "prime_set_clk_bw failed: %d\n", ret);
		return ret;
	}

	return len;
#endif /* BUILDROOT */
}

static DEVICE_ATTR_RW(PRIMESS_CLK_BW);

/** All R/W attributes */
static struct attribute *prime_attributes[] = {
	&dev_attr_PRIMESS_IRQ2L_PEND_CLR.attr,
	&dev_attr_PRIMESS_IRQ2L_PEND_SET.attr,
	&dev_attr_PRIMESS_IRQ2L_PEND_STAT.attr,
	&dev_attr_PRIMESS_IUSS_IRQ_PEND_WDATA_WORD0.attr,
	&dev_attr_PRIMESS_RIF.attr,
	&dev_attr_PRIMESS_CLK_BW.attr,
	NULL,
};

/** R/W attribut group. */
static const struct attribute_group prime_attr_group = {
	.attrs = prime_attributes,
};

#ifdef BUILDROOT
#include "prime_iu_intf_km.h"

static int qcom_scm_pas_shutdown(struct rproc *rproc)
{
	int ret;
	struct qcom_prime *prime = (struct qcom_prime *)rproc->priv;

	ret = fw_iu_rproc_stop_ss(&prime->iuss_dev);
	if (ret != 0) {
		dev_dbg(prime->dev, "Error Stopping Core\n");
		return ret;
	}
	ret = fw_iu_rproc_enable_pdmem(&prime->iuss_dev, 0);
	if (ret != 0)
		dev_dbg(prime->dev, "Error UnPreparing Core\n");
	return ret;
}

static int qcom_scm_pas_auth_and_reset(struct rproc *rproc)
{
	int ret;
	struct qcom_prime *prime = (struct qcom_prime *)rproc->priv;
	void *pdmem_base = prime->pdmem_base;

	dev_dbg(prime->dev,
		"Memcpy PRIME 0x%08llx -> 0x%08llx (%zu)byte\n",
		(uint64_t)prime->mem_region, (uint64_t)pdmem_base,
		prime->pdmem_size);

	//Enable PDMEM
	ret = fw_iu_rproc_enable_pdmem(&prime->iuss_dev, 1);
	if (ret != 0) {
		dev_dbg(prime->dev, "Error Preparing Core\n");
		return ret;
	}

	// Old model thows an exception if we write the last word of PDMEM
	// Check to make sure model is good
	writel(0xcafebabe, FW_PRIMESS_REG_ADDR(prime->pdmem_base, prime->pdmem_size - 4));
	memcpy_toio(pdmem_base, prime->mem_region, prime->pdmem_size);
	/* Ensure all PDMEM writes are visible to PRIME hardware before boot */
	wmb();

	ret = fw_iu_rproc_boot_ss(&prime->iuss_dev);
	if (ret < 0)
		dev_dbg(prime->dev, "Error Starting Core\n");

	return ret;
}
#endif

#ifndef BUILDROOT
#define PRIME_KM_VOTE_BIT (1)

static int prime_set_clk_bw(struct qcom_prime *prime, enum prime_clk_level clk_level,
			    u32 ddr_bw, u32 cpu_bw)
{
	struct device *dev = prime->dev;
	unsigned long clk_rate;
	int ret;

	/* Handle PRIME_CLK_LEVEL_OFF - disable clock and clear bandwidth */
	if (clk_level == PRIME_CLK_LEVEL_OFF) {
		if (prime->clk_enabled) {
			dev_dbg(dev, "Disabling clock\n");
			clk_disable_unprepare(prime->core_clk);
			prime->clk_enabled = false;
		}

		if (prime->icc_ddr)
			icc_set_bw(prime->icc_ddr, 0, ddr_bw);
		if (prime->icc_cpu)
			icc_set_bw(prime->icc_cpu, 0, cpu_bw);

		/* Update state */
		prime->clk_level = PRIME_CLK_LEVEL_OFF;
		prime->clk_ddr_bw = ddr_bw;
		prime->clk_cpu_bw = cpu_bw;

		dev_dbg(dev, "Clock disabled, ddr_bw=%u, cpu_bw=%u\n", ddr_bw, cpu_bw);
		return 0;
	}

	/* Enable clock if not already enabled */
	bool newly_enabled = false;

	if (!prime->clk_enabled) {
		if (!prime->core_clk) {
			dev_err(dev, "core_clk is NULL\n");
			return -EINVAL;
		}
		dev_dbg(dev, "Enabling clock\n");
		ret = clk_prepare_enable(prime->core_clk);
		if (ret != 0) {
			dev_err(dev, "clk_prepare_enable failed. ret: %d\n", ret);
			return ret;
		}
		prime->clk_enabled = true;
		newly_enabled = true;
	}

	/* Convert clock level to actual rate */
	clk_rate = prime_get_clk(clk_level);

	ret = clk_set_rate(prime->core_clk, clk_rate);
	if (ret != 0) {
		dev_err(dev, "clk_set_rate failed. ret: %d\n", ret);
		goto r_clk;
	}

	if (prime->icc_ddr && prime->icc_cpu) {
		ret = icc_set_bw(prime->icc_ddr, 0, ddr_bw);
		if (ret != 0) {
			dev_err(dev, "failed to set ddr bandwidth request: %d\n", ret);
			goto r_clk;
		}

		ret = icc_set_bw(prime->icc_cpu, 0, cpu_bw);
		if (ret != 0) {
			dev_err(dev, "failed to set cpu bandwidth request: %d\n", ret);
			goto r_clk;
		}
	}

	/* Update state */
	prime->clk_level = clk_level;
	prime->clk_ddr_bw = ddr_bw;
	prime->clk_cpu_bw = cpu_bw;

	dev_dbg(dev, "Set clk_level=%d, clk_rate=%lu, ddr_bw=%u, cpu_bw=%u\n",
		clk_level, clk_rate, ddr_bw, cpu_bw);

	return 0;

r_clk:
	if (newly_enabled) {
		clk_disable_unprepare(prime->core_clk);
		prime->clk_enabled = false;
	}
	return ret;
}

static int prime_init_clock_power(struct qcom_prime *prime)
{
	struct device *dev = prime->dev;
	int ret;

	prime->core_clk = devm_clk_get(dev, "core-clk");
	if (IS_ERR_OR_NULL(prime->core_clk)) {
		dev_dbg(dev, "devm_clk_get failed for core-clk\n");
		return PTR_ERR(prime->core_clk);
	}

	prime->icc_ddr = devm_of_icc_get(dev, "prime-ddr");
	if (IS_ERR(prime->icc_ddr)) {
		dev_dbg(dev, "No interconnects in DT\n");
		prime->icc_ddr = NULL;
		prime->icc_cpu = NULL;
	} else {
		prime->icc_cpu = devm_of_icc_get(dev, "cpu-prime");
		if (IS_ERR(prime->icc_cpu))
			return dev_err_probe(dev, PTR_ERR(prime->icc_cpu),
						"failed to acquire interconnect cpu\n");

		/* Include these votes as part of probing since interconnect driver
		 * holds a proxy vote setting clks to turbo until we vote for ourselves.
		 */
		ret = icc_set_bw(prime->icc_ddr, 0, 0);
		if (ret != 0) {
			dev_dbg(dev, "failed to set ddr bandwidth request: %d\n", ret);
			goto r_bw;
		}

		ret = icc_set_bw(prime->icc_cpu, 0, 0);
		if (ret != 0) {
			dev_dbg(dev, "failed to set cpu bandwidth request: %d\n", ret);
			goto r_bw;
		}
	}

	return 0;

r_bw:

	return ret;
}

static int prime_start_clock(struct qcom_prime *prime, enum prime_clk_level clk_level)
{
	struct device *dev = prime->dev;
	struct prime_bw_table_val mem_cfg_bw = prime_bw_table[clk_level];
	u32 val;
	int ret;

	dev_dbg(dev, "Setting BW\n");
	ret = prime_set_clk_bw(prime, clk_level, mem_cfg_bw.mem_bw, mem_cfg_bw.cfg_bw);
	if (ret != 0) {
		dev_err(dev, "prime_set_clk_bw failed. ret: %d\n", ret);
		return ret;
	}

	if (prime->tmelcom_support) {
		void __iomem *gcc_base;

		/* These clocks are supposed to be configured by ARCG, temporarily
		 * configure them here till ARCG enables these.
		 */
		gcc_base = ioremap(IPQ9650_GCC_BASE, IPQ9676_GCC_SIZE);
		if (IS_ERR_OR_NULL(gcc_base)) {
			dev_err(dev, "Failed to ioremap gcc region\n");
			return PTR_ERR(gcc_base);
		}

		/* Configure GCC_SNOC_PRIMESS_AXIM_CBCR */
		val = readl(gcc_base + GCC_SNOC_PRIMESS_AXIM_CBCR);
		val |= 0x1;
		writel(val, gcc_base + GCC_SNOC_PRIMESS_AXIM_CBCR);
		fsleep(1000);

		/* Configure GCC_CNOC_PRIMESS_AHBS_CBCR */
		val = readl(gcc_base + GCC_CNOC_PRIMESS_AHBS_CBCR);
		val |= 0x1;
		writel(val, gcc_base + GCC_CNOC_PRIMESS_AHBS_CBCR);
		fsleep(1000);

		iounmap(gcc_base);
	}
	dev_dbg(dev, "Enable Voter\n");
	// Enable bit 1 in the voter
	writel(PRIME_KM_VOTE_BIT, prime->reg_ss_base + PRIMESS_VOTER_FW_EN_CFG);
	// readback to allow time for cfg to flush.
	readl(prime->reg_ss_base + PRIMESS_VOTER_FW_EN_CFG);
	dev_dbg(dev, "Powerup PRIME SS\n");
	// Vote to start power up sequence
	writel(PRIME_KM_VOTE_BIT, prime->reg_ss_base + PRIMESS_VOTER_FW_VOTE_SET);

	ret = readl_poll_timeout_atomic(prime->reg_ss_base + PRIMESS_VOTER_FW_VOTE_DONE,
					val, val & PRIME_KM_VOTE_BIT, 1, 100);
	if (ret != 0) {
		dev_err(dev, "Voter Powerup Timed out!\n");
		goto r_bw;
	} else {
		dev_dbg(dev, "Voter Done Set.\n");
	}

	return 0;

r_bw:
	prime_set_clk_bw(prime, PRIME_CLK_LEVEL_OFF, 0, 0);
	return ret;
}

static int prime_cleanup_clock_power(struct qcom_prime *prime)
{
	writel(0, prime->reg_ss_base + PRIMESS_VOTER_FW_EN_CFG);
	writel(PRIME_KM_VOTE_BIT, prime->reg_ss_base + PRIMESS_VOTER_FW_VOTE_CLEAR);
	readl(prime->reg_ss_base + PRIMESS_VOTER_FW_EN_CFG);

	// Do we need to guarantee SS is powered down before stopping clocks?  How?
	if (prime->clk_enabled) {
		clk_disable_unprepare(prime->core_clk);
		prime->clk_enabled = false;
	}

	if (prime->icc_ddr)
		icc_set_bw(prime->icc_ddr, 0, 0);
	if (prime->icc_cpu)
		icc_set_bw(prime->icc_cpu, 0, 0);

	/* Reset state */
	prime->clk_level = PRIME_CLK_LEVEL_OFF;
	prime->clk_ddr_bw = 0;
	prime->clk_cpu_bw = 0;

	return 0;
}
#endif /* BUILDROOT */

#ifndef BUILDROOT
#if !IS_ENABLED(CONFIG_QCOM_TMELCOM)
static int tmelcom_secboot_teardown(u32 sw_id, u32 secondary_sw_id)
{
	return 0;
}

static int tmelcom_secboot_sec_auth_v2(u32 sw_id, void *metadata, size_t size)
{
	return 0;
}
#endif
#endif /* BUILDROOT */

#ifndef BUILDROOT
static int prime_load(struct rproc *rproc, const struct firmware *fw)
{
	int ret;
	struct qcom_prime *prime = (struct qcom_prime *)rproc->priv;

	/* Use tmelcom path for IPQ platforms, standard PIL/PAS path for others */
	if (prime->tmelcom_support) {
		dev_dbg(prime->dev, "Loading firmware via tmelcom (qcom_mdt_load_no_init)\n");

		/* Read metadata for tmelcom authentication */
		prime->metadata = qcom_mdt_read_metadata(fw, &prime->metadata_len,
							 rproc->firmware, prime->dev);
		if (IS_ERR(prime->metadata)) {
			ret = PTR_ERR(prime->metadata);
			dev_err(prime->dev, "error %d reading firmware %s metadata\n",
				ret, rproc->firmware);
			return ret;
		}
#if IS_ENABLED(CONFIG_QCOM_TMELCOM)
		ret = qcom_mdt_load_no_init(prime->dev, fw, rproc->firmware, prime->pas_id,
			prime->mem_region, prime->mem_phys, prime->mem_size, &prime->mem_reloc);
#else
		return -EINVAL;
#endif
	} else {
		dev_dbg(prime->dev, "Loading firmware via standard PIL/PAS (qcom_mdt_load)\n");
		ret = qcom_mdt_load(prime->dev, fw, rproc->firmware, prime->pas_id,
			prime->mem_region, prime->mem_phys, prime->mem_size, &prime->mem_reloc);
	}

	return ret;
}
#endif

static int prime_stop(struct rproc *rproc)
{
	struct qcom_prime *prime = (struct qcom_prime *)rproc->priv;
	int ret;
	int mmap_count;

	/* Check if SWIF memory is still mapped */
	mmap_count = atomic_read(&prime->swif_mmap_count);
	if (mmap_count > 0) {
		dev_err(prime->dev,
			"Cannot stop PRIME: %d active SWIF mapping(s) exist. Userspace must munmap()\n",
			mmap_count);
		return -EBUSY;
	}

	/* Mark PRIME as stopped - prevent new SWIF mappings */
	prime->prime_started = false;

	/* Decrement module reference to allow rmmod */
	module_put(THIS_MODULE);

#ifdef BUILDROOT
	ret = qcom_scm_pas_shutdown(rproc);
	if (ret < 0) {
		dev_dbg(prime->dev, "Error Shutting Down Core\n");
		goto done;
	}
	mask_irqs(prime);
#else
	/* Use tmelcom path for IPQ platforms, standard PIL/PAS path for others */
	if (prime->tmelcom_support) {
		dev_dbg(prime->dev, "Stopping firmware via tmelcom (tmelcom_secboot_teardown)\n");
		ret = tmelcom_secboot_teardown(prime->pas_id, 0);
	} else {
		dev_dbg(prime->dev, "Stopping firmware via standard PIL/PAS (qcom_scm_pas_shutdown)\n");
		ret = qcom_scm_pas_shutdown(prime->pas_id);
	}

	if (ret < 0) {
		dev_dbg(prime->dev, "Error Shutting Down Core\n");
		goto done;
	}
	mask_irqs(prime);
	ret = prime_cleanup_clock_power(prime);
	if (ret < 0) {
		dev_dbg(prime->dev, "Error Cleaning Clocks\n");
		goto done;
	}
#endif

	/* Set state as OFFLINE */
	rproc->state = RPROC_OFFLINE;
	reinit_completion(&prime->start_done);

done:
	return ret;
}

/**
 * prime_rproc_da_to_va() - ELF "Device Addr" -> "Virtual Addr" Conversion.
 *
 * The RPROC framework requires us to remap the Phys Addr in the ELF Segments to
 * the virtual address on to which they were remapped by the device driver probe
 * function. Since, PRIME ELFs are addressed zero-relative to PDMEM start, we
 * simply add the PDMEM's HWIO offset to the overall virtual address base to get
 * the absolute virtual address of PDMEM corresponding to the provided device
 * address.
 */
static void *prime_rproc_da_to_va(struct rproc *rproc, u64 da,
				  size_t len, bool *is_iomem)
{
	struct qcom_prime *prime = rproc->priv;
	*is_iomem = false;
	int offset;

	//If less than 1MB, assume addresses are relative
	#define PRIME_ELF_ABS_BASE (0x8C180000)
	offset = (da < 0x100000) ? da : da - PRIME_ELF_ABS_BASE;

	//Allow overflow of PDMEM into MLM region if using prime->mem_size
	//(Carveout size vs pdmem_size)
	if (offset < 0 || offset + len > prime->pdmem_size) {
		dev_err(prime->dev, "da[0x%llX] + len[0x%zX] > 0x%zX\n",
			da, len, prime->pdmem_size);
		return NULL;
	}

	return prime->mem_region + offset;
}

static int prime_start(struct rproc *rproc)
{
	struct qcom_prime *prime = (struct qcom_prime *)rproc->priv;
	int ret = 0;

#ifdef BUILDROOT
	//Initial boot clear pending
	unmask_irqs(prime, 0xff);

	ret = qcom_scm_pas_auth_and_reset(rproc);
	if (ret) {
		dev_err(prime->dev, "Auth and reset failed for remoteproc %s: %d\n",
			rproc->name, ret);
		return ret;
	}
#else
	ret = prime_start_clock(prime, PRIME_CLK_LEVEL_LSVS);
	if (ret < 0) {
		dev_dbg(prime->dev, "Error setting clocks\n");
		return ret;
	}

	//Initial boot clear pending
	unmask_irqs(prime, 0xff);

	/* Use tmelcom path for IPQ platforms, standard PIL/PAS path for others */
	if (prime->tmelcom_support) {
		dev_dbg(prime->dev, "Starting firmware via tmelcom (tmelcom_secboot_sec_auth)\n");
		ret = tmelcom_secboot_sec_auth_v2(prime->pas_id,
				prime->metadata, prime->metadata_len);
	} else {
		dev_dbg(prime->dev, "Starting firmware via standard PIL/PAS (qcom_scm_pas_auth_and_reset)\n");
		ret = qcom_scm_pas_auth_and_reset(prime->pas_id);
	}
	if (ret) {
		dev_err(prime->dev, "Auth and reset failed for remoteproc %s: %d\n",
			rproc->name, ret);
		if (prime->tmelcom_support)
			(void)tmelcom_secboot_teardown(prime->pas_id, 0);
		return ret;
	}
#endif

	/* This isn't a guaranteed sync point as there will potentially be 3 IRQs from PRIME...
	 * Treated more as a proof of life since auth_and_reset will be blocking until booted.
	 */
	if (!wait_for_completion_timeout(&prime->start_done, msecs_to_jiffies(10000))) {
		dev_err(prime->dev, "Boot completion timeout after 5 seconds\n");
		return -ETIMEDOUT;
	}

	/* Mark PRIME as started - SWIF mapping now allowed */
	prime->prime_started = true;

	/* Increment module reference to prevent rmmod while PRIME is running */
	if (!try_module_get(THIS_MODULE)) {
		dev_err(prime->dev, "Failed to increment module reference\n");
		prime->prime_started = false;
		return -ENODEV;
	}

	return ret;
}

/**
 * prime_swif_vma_open - Called when VMA is duplicated (e.g., fork)
 * Increments the active mapping count
 */
static void prime_swif_vma_open(struct vm_area_struct *vma)
{
	struct qcom_prime *prime = vma->vm_private_data;
	int count = atomic_inc_return(&prime->swif_mmap_count);

	dev_dbg(prime->dev, "SWIF VMA opened (fork/dup), active mappings: %d\n", count);
}

/**
 * prime_swif_vma_close - Called when VMA is unmapped
 * Decrements the active mapping count
 */
static void prime_swif_vma_close(struct vm_area_struct *vma)
{
	struct qcom_prime *prime = vma->vm_private_data;
	int count = atomic_dec_return(&prime->swif_mmap_count);

	dev_dbg(prime->dev, "SWIF VMA closed (munmap), active mappings: %d\n", count);
}

static const struct vm_operations_struct prime_swif_vm_ops = {
	.open = prime_swif_vma_open,
	.close = prime_swif_vma_close,
};

#if IS_ENABLED(CONFIG_QCOM_SECURE_BUFFER)
/* PRIME VMID - not in upstream kernel headers */
#define QCOM_SCM_VMID_PRIME (93)

/**
 * prime_mem_mmu_add_prime() - Add PRIME VMID to memory region
 * @prime: PRIME device
 * @phys_addr: Physical address of region
 * @size: Size of region
 *
 * Assigns memory region to both HLOS and PRIME with RW permissions.
 * Returns 0 on success, negative error code on failure.
 */
static int prime_mem_mmu_add_prime(struct qcom_prime *prime,
				   phys_addr_t phys_addr, size_t size)
{
	struct qcom_scm_mem_map_info *mem_region;
	struct qcom_scm_current_perm_info *dest_perms;
	u32 *src_vmids;
	void *buf;
	size_t buf_size;
	int ret;

	buf_size = sizeof(*mem_region) + sizeof(u32) + 2 * sizeof(*dest_perms);
	buf = kmalloc(buf_size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	mem_region = (struct qcom_scm_mem_map_info *)buf;
	src_vmids = (u32 *)(mem_region + 1);
	dest_perms = (struct qcom_scm_current_perm_info *)(src_vmids + 1);

	qcom_scm_populate_mem_map_info(mem_region, phys_addr, size);
	src_vmids[0] = QCOM_SCM_VMID_HLOS;
	qcom_scm_populate_vmperm_info(&dest_perms[0], QCOM_SCM_VMID_HLOS, QCOM_SCM_PERM_RW);
	qcom_scm_populate_vmperm_info(&dest_perms[1], QCOM_SCM_VMID_PRIME, QCOM_SCM_PERM_RW);

	ret = qcom_scm_assign_mem_regions(mem_region, sizeof(*mem_region),
					  src_vmids, sizeof(u32),
					  dest_perms, 2 * sizeof(*dest_perms));
	kfree(buf);

	if (ret)
		dev_err(prime->dev, "Failed to assign memory to PRIME: %d\n", ret);

	return ret;
}

/**
 * prime_mem_mmu_remove_prime() - Remove PRIME VMID from memory region
 * @prime: PRIME device
 * @phys_addr: Physical address of region
 * @size: Size of region
 *
 * Returns memory region to HLOS-only with full permissions.
 * Returns 0 on success, negative error code on failure.
 */
static int prime_mem_mmu_remove_prime(struct qcom_prime *prime,
				      phys_addr_t phys_addr, size_t size)
{
	struct qcom_scm_mem_map_info *mem_region;
	struct qcom_scm_current_perm_info *dest_perms;
	u32 *src_vmids;
	void *buf;
	size_t buf_size;
	int ret;

	buf_size = sizeof(*mem_region) + 2 * sizeof(u32) + sizeof(*dest_perms);
	buf = kmalloc(buf_size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	mem_region = (struct qcom_scm_mem_map_info *)buf;
	src_vmids = (u32 *)(mem_region + 1);
	dest_perms = (struct qcom_scm_current_perm_info *)(src_vmids + 2);

	qcom_scm_populate_mem_map_info(mem_region, phys_addr, size);
	src_vmids[0] = QCOM_SCM_VMID_HLOS;
	src_vmids[1] = QCOM_SCM_VMID_PRIME;
	qcom_scm_populate_vmperm_info(dest_perms, QCOM_SCM_VMID_HLOS,
				      QCOM_SCM_PERM_READ | QCOM_SCM_PERM_WRITE |
				      QCOM_SCM_PERM_EXEC);

	ret = qcom_scm_assign_mem_regions(mem_region, sizeof(*mem_region),
					  src_vmids, 2 * sizeof(u32),
					  dest_perms, sizeof(*dest_perms));
	kfree(buf);

	if (ret)
		dev_err(prime->dev, "Failed to unassign memory from PRIME: %d\n", ret);

	return ret;
}
#else
static inline int prime_mem_mmu_add_prime(struct qcom_prime *prime,
					  phys_addr_t phys_addr, size_t size)
{
	return 0;
}

static inline int prime_mem_mmu_remove_prime(struct qcom_prime *prime,
					     phys_addr_t phys_addr, size_t size)
{
	return 0;
}
#endif

/**
 * prime_model_mem_prepare() - Prepare MODEL memory sub-region for PRIME access
 * @prime: PRIME device
 * @offset: Offset within MODEL memory region
 * @size: Size of sub-region
 * @direction: DMA direction for cache operations
 * @ret_dma_addr: Output DMA address for release
 *
 * Performs cache flush and adds PRIME VMID to the specified sub-region.
 * Returns 0 on success, negative error code on failure.
 */
static int prime_model_mem_prepare(struct qcom_prime *prime,
				   u64 offset, size_t size,
				   enum dma_data_direction direction,
				   dma_addr_t *ret_dma_addr)
{
	int ret;
	phys_addr_t phys_addr;

	if (offset + size > prime->mem_model_size) {
		dev_err(prime->dev, "Sub-region exceeds MODEL memory bounds\n");
		return -EINVAL;
	}

	phys_addr = prime->mem_model_base + offset;

	if (!PAGE_ALIGNED(phys_addr) || !PAGE_ALIGNED(size)) {
		dev_err(prime->dev, "Sub-region must be page-aligned\n");
		return -EINVAL;
	}

	/* Return the physical address as the DMA address.
	 * The carveout is mapped write-combining (pgprot_writecombine);
	 */
	*ret_dma_addr = phys_addr;

	if (direction == DMA_TO_DEVICE || direction == DMA_BIDIRECTIONAL)
		wmb(); /* Barrier so PRIME hardware sees all CPU writes */

	if (prime->config_mmu) {
		ret = prime_mem_mmu_add_prime(prime, phys_addr, size);
		if (ret)
			return ret;
	}

	dev_dbg(prime->dev,
		"MODEL sub-region prepared: offset=0x%llx, phys=0x%llx, dma=0x%llx, size=0x%zx, dir=%d\n",
		offset, (unsigned long long)phys_addr,
		(unsigned long long)*ret_dma_addr, size, direction);

	return 0;
}

/**
 * prime_model_mem_release() - Release MODEL memory sub-region from PRIME access
 * @prime: PRIME device
 * @offset: Offset within MODEL memory region
 * @size: Size of sub-region
 * @direction: DMA direction (must match prepare)
 * @dma_addr: DMA address from prepare operation
 *
 * Unmaps DMA and removes PRIME VMID from the specified sub-region.
 * Returns 0 on success, negative error code on failure.
 */
static int prime_model_mem_release(struct qcom_prime *prime,
				   u64 offset, size_t size,
				   enum dma_data_direction direction,
				   dma_addr_t dma_addr)
{
	phys_addr_t phys_addr;

	phys_addr = prime->mem_model_base + offset;

	if (direction == DMA_FROM_DEVICE || direction == DMA_BIDIRECTIONAL)
		rmb(); /* Barrier so CPU sees data written by hardware */

	if (prime->config_mmu) {
		int ret = prime_mem_mmu_remove_prime(prime, phys_addr, size);

		if (ret)
			dev_err(prime->dev, "Failed to unassign memory (continuing): %d\n", ret);
	}

	dev_dbg(prime->dev,
		"MODEL sub-region released: offset=0x%llx, phys=0x%llx, dma=0x%llx, size=0x%zx, dir=%d\n",
		offset, (unsigned long long)phys_addr,
		(unsigned long long)dma_addr, size, direction);

	return 0;
}

static struct prime_pil_mapping *prime_pil_find_by_key_locked(struct qcom_prime *prime, u64 map_key)
{
	struct prime_pil_mapping *mapping;

	list_for_each_entry(mapping, &prime->pil_mappings, node) {
		if (mapping->map_key == map_key)
			return mapping;
	}

	return NULL;
}

static int prime_pil_mapping_release(struct qcom_prime *prime, struct prime_pil_mapping *mapping)
{
	int ret;

	ret = prime_model_mem_release(prime, mapping->offset, mapping->size,
				      mapping->direction, mapping->device_dma_base_addr);
	kfree(mapping);
	return ret;
}

static int prime_pil_prepare(struct qcom_prime *prime, struct prime_mem_pil_map *pil_data)
{
	struct prime_pil_mapping *mapping;
	dma_addr_t dma_addr;
	u64 key;
	int ret;

	mapping = kzalloc(sizeof(*mapping), GFP_KERNEL);
	if (!mapping)
		return -ENOMEM;

	ret = prime_model_mem_prepare(prime, pil_data->offset, pil_data->size,
				      (enum dma_data_direction)pil_data->direction,
				      &dma_addr);
	if (ret) {
		kfree(mapping);
		return ret;
	}

	mapping->owner_tgid = task_tgid_nr(current);
	mapping->offset = pil_data->offset;
	mapping->size = pil_data->size;
	mapping->direction = (enum dma_data_direction)pil_data->direction;
	mapping->device_dma_base_addr = dma_addr;

	key = (u64)atomic64_inc_return(&prime->pil_map_key_next);
	if (key == 0)
		key = (u64)atomic64_inc_return(&prime->pil_map_key_next);
	mapping->map_key = key;

	mutex_lock(&prime->pil_map_lock);
	list_add_tail(&mapping->node, &prime->pil_mappings);
	mutex_unlock(&prime->pil_map_lock);

	pil_data->map_key = mapping->map_key;
	pil_data->device_dma_base_addr = mapping->device_dma_base_addr;
	return 0;
}

static int prime_pil_release(struct qcom_prime *prime, struct prime_mem_pil_map *pil_data)
{
	struct prime_pil_mapping *mapping;
	pid_t tgid = task_tgid_nr(current);
	int ret;

	if (!pil_data->map_key)
		return -EINVAL;

	mutex_lock(&prime->pil_map_lock);
	mapping = prime_pil_find_by_key_locked(prime, pil_data->map_key);
	if (!mapping) {
		mutex_unlock(&prime->pil_map_lock);
		return -ENOENT;
	}

	if (mapping->owner_tgid != tgid) {
		mutex_unlock(&prime->pil_map_lock);
		dev_warn(prime->dev,
			 "pil map_key 0x%llx owner mismatch (owner=%d current=%d)\n",
			 (unsigned long long)mapping->map_key,
			 mapping->owner_tgid, tgid);
		return -EPERM;
	}

	list_del(&mapping->node);
	mutex_unlock(&prime->pil_map_lock);

	pil_data->offset = mapping->offset;
	pil_data->size = mapping->size;
	pil_data->direction = (enum prime_dma_direction)mapping->direction;
	pil_data->device_dma_base_addr = mapping->device_dma_base_addr;

	ret = prime_pil_mapping_release(prime, mapping);
	pil_data->map_key = 0;
	return ret;
}

static int prime_pil_cleanup_owner(struct qcom_prime *prime, pid_t owner_tgid)
{
	struct prime_pil_mapping *mapping, *tmp;
	int ret = 0;

	mutex_lock(&prime->pil_map_lock);
	list_for_each_entry_safe(mapping, tmp, &prime->pil_mappings, node) {
		if (owner_tgid >= 0 && mapping->owner_tgid != owner_tgid)
			continue;

		list_del(&mapping->node);
		mutex_unlock(&prime->pil_map_lock);
		ret = prime_pil_mapping_release(prime, mapping);
		mutex_lock(&prime->pil_map_lock);
	}
	mutex_unlock(&prime->pil_map_lock);

	return ret;
}

static int prime_mem_open(struct inode *inode, struct file *file)
{
	struct qcom_prime *prime;

	prime = container_of(inode->i_cdev, struct qcom_prime, cdev);
	file->private_data = prime;
	return 0;
}

static int prime_mem_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct qcom_prime *prime = file->private_data;
	int ret = 0;
	size_t size = vma->vm_end - vma->vm_start;
	phys_addr_t base_addr;
	size_t offset;
	size_t vsize;
	unsigned long pfn;

	switch (MINOR(file->f_inode->i_rdev)) {
	case 0: /* MODEL Memory */
		base_addr = prime->mem_model_base;
		offset = PFN_UP(vma->vm_pgoff);
		dev_dbg(prime->dev, "MMAP Model Mem 0x%08X\n", (u32)(base_addr + offset));
		pfn = PFN_DOWN(base_addr + offset);
		vsize = prime->mem_model_size - offset;

		/* base address and total buffer size must be page aligned */
		if (!PAGE_ALIGNED(base_addr) || !PAGE_ALIGNED(size))
			return -ENODEV;

		if (size > vsize)
			return -EINVAL;

		/* Map as write-combining memory (DDR-backed reserved region)
		 */
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
		ret = remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot);
		break;
	case 1: /* IUSS Interface exposed to kernel*/
		/* Check if PRIME has been started */
		if (!prime->prime_started) {
			dev_err(prime->dev,
				"Cannot map SWIF memory: PRIME not started.\n");
			return -EAGAIN;
		}

		// We can't map just the swif since mappings must be page boundary aligned.
		base_addr = prime->intf_table_phys;
		dev_dbg(prime->dev, "MMAP SWIF Mem 0x%08X (prime_started=%d, active_maps=%d)\n",
			(u32)base_addr, prime->prime_started,
			atomic_read(&prime->swif_mmap_count));
		pfn = PFN_DOWN(base_addr);
		vsize = (phys_addr_t)prime->pdmem_base + prime->pdmem_size - base_addr;

		/* base address and total buffer size must be page aligned */
		if (!PAGE_ALIGNED(base_addr) || !PAGE_ALIGNED(size))
			return -ENODEV;

		if (size > vsize)
			return -EINVAL;

		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
		ret = io_remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot);

		/* Set up VMA operations for lifecycle tracking */
		if (ret == 0) {
			vma->vm_ops = &prime_swif_vm_ops;
			vma->vm_private_data = prime;
			/* Increment count for initial mapping */
			atomic_inc(&prime->swif_mmap_count);
			dev_dbg(prime->dev,
				"SWIF mapped successfully: addr=0x%08llx, size=0x%zx, active_maps=%d\n",
				(unsigned long long)base_addr, size,
				atomic_read(&prime->swif_mmap_count));
		} else {
			dev_err(prime->dev, "SWIF mapping failed: %d\n", ret);
		}
		break;
	case 2: /* Tensor Memory from kmalloc?  */
		dev_dbg(prime->dev, "MMAP Tensor Mem\n");
		return -EINVAL;
	}

	return ret;
}

static int prime_dma_mapping_release(struct qcom_prime *prime,
				     struct prime_dma_mapping *mapping)
{
	int sg_idx;
	struct scatterlist *sg;
	int ret = 0;

	if (prime->config_mmu) {
		for_each_sgtable_dma_sg(mapping->sg_table, sg, sg_idx) {
			dev_dbg(prime->dev,
				"Unmapping DMA SG[%d]: 0x%08llx (%d bytes)\n",
				sg_idx, (u64)sg_dma_address(sg), sg_dma_len(sg));
			ret = prime_mem_mmu_remove_prime(prime,
							 (phys_addr_t)sg_dma_address(sg),
							 sg_dma_len(sg));
		}
	}

	dma_buf_unmap_attachment(mapping->dma_attachment, mapping->sg_table,
				 mapping->direction);
	dma_buf_detach(mapping->dma_buf, mapping->dma_attachment);
	dma_buf_put(mapping->dma_buf);
	kfree(mapping);

	return ret;
}

static struct prime_dma_mapping *prime_dma_find_by_key_locked(struct qcom_prime *prime, u64 map_key)
{
	struct prime_dma_mapping *mapping;

	list_for_each_entry(mapping, &prime->dma_mappings, node) {
		if (mapping->map_key == map_key)
			return mapping;
	}

	return NULL;
}

static int prime_dma_cleanup_owner(struct qcom_prime *prime, pid_t owner_tgid)
{
	struct prime_dma_mapping *mapping, *tmp;
	int ret = 0;

	mutex_lock(&prime->dma_map_lock);
	list_for_each_entry_safe(mapping, tmp, &prime->dma_mappings, node) {
		if (owner_tgid >= 0 && mapping->owner_tgid != owner_tgid)
			continue;
		list_del(&mapping->node);
		mutex_unlock(&prime->dma_map_lock);
		ret = prime_dma_mapping_release(prime, mapping);
		mutex_lock(&prime->dma_map_lock);
	}
	mutex_unlock(&prime->dma_map_lock);

	return ret;
}

static int prime_mem_dma_alloc(struct qcom_prime *prime, struct prime_mem_dma_map *mem_data)
{
	struct prime_dma_mapping *mapping;
	int sg_idx;
	struct scatterlist *sg;
	int ret;
	u64 key;

	mapping = kzalloc(sizeof(*mapping), GFP_KERNEL);
	if (!mapping)
		return -ENOMEM;

	mapping->owner_tgid = task_tgid_nr(current);
	mapping->direction = (enum dma_data_direction)mem_data->direction;
	mapping->dma_buf = dma_buf_get(mem_data->dma_fd);
	if (IS_ERR(mapping->dma_buf)) {
		ret = -EINVAL;
		goto err_free;
	}

	mapping->dma_attachment = dma_buf_attach(mapping->dma_buf, prime->dev);
	if (IS_ERR(mapping->dma_attachment)) {
		ret = -EINVAL;
		goto err_put;
	}

	mapping->sg_table = dma_buf_map_attachment(mapping->dma_attachment, mapping->direction);
	if (IS_ERR(mapping->sg_table)) {
		ret = -EINVAL;
		goto err_detach;
	}

	if (mapping->sg_table->nents != 1) {
		dev_dbg(prime->dev, "dma_buf_map_attachment nents %d > 1\n",
			mapping->sg_table->nents);
		ret = -EINVAL;
		goto err_unmap;
	}

	for_each_sgtable_dma_sg(mapping->sg_table, sg, sg_idx) {
		dev_dbg(prime->dev, "Mapped DMA SG[%d]: 0x%08llx (%d bytes)\n",
			sg_idx, (u64)sg_dma_address(sg), sg_dma_len(sg));
		mapping->device_dma_base_addr = (u64)sg_dma_address(sg);

		if (prime->config_mmu) {
			ret = prime_mem_mmu_add_prime(prime,
						      (phys_addr_t)sg_dma_address(sg),
						      sg_dma_len(sg));
			if (ret)
				goto err_mmu_remove;
		}
	}

	key = (u64)atomic64_inc_return(&prime->dma_map_key_next);
	if (key == 0)
		key = (u64)atomic64_inc_return(&prime->dma_map_key_next);
	mapping->map_key = key;

	mutex_lock(&prime->dma_map_lock);
	list_add_tail(&mapping->node, &prime->dma_mappings);
	mutex_unlock(&prime->dma_map_lock);

	mem_data->map_key = mapping->map_key;
	mem_data->device_dma_base_addr = mapping->device_dma_base_addr;
	return 0;

err_mmu_remove:
	if (prime->config_mmu) {
		for_each_sgtable_dma_sg(mapping->sg_table, sg, sg_idx)
			prime_mem_mmu_remove_prime(prime,
						   (phys_addr_t)sg_dma_address(sg),
						   sg_dma_len(sg));
	}
err_unmap:
	dma_buf_unmap_attachment(mapping->dma_attachment, mapping->sg_table, mapping->direction);
err_detach:
	dma_buf_detach(mapping->dma_buf, mapping->dma_attachment);
err_put:
	dma_buf_put(mapping->dma_buf);
err_free:
	kfree(mapping);
	return ret;
}

static int prime_mem_dma_free(struct qcom_prime *prime, struct prime_mem_dma_map *mem_data)
{
	struct prime_dma_mapping *mapping;
	pid_t tgid = task_tgid_nr(current);
	int ret;

	if (!mem_data->map_key)
		return -EINVAL;

	mutex_lock(&prime->dma_map_lock);
	mapping = prime_dma_find_by_key_locked(prime, mem_data->map_key);
	if (!mapping) {
		mutex_unlock(&prime->dma_map_lock);
		return -ENOENT;
	}

	if (mapping->owner_tgid != tgid) {
		mutex_unlock(&prime->dma_map_lock);
		dev_warn(prime->dev,
			 "dma map_key 0x%llx owner mismatch (owner=%d current=%d)\n",
			 mapping->map_key, mapping->owner_tgid, tgid);
		return -EPERM;
	}

	mem_data->device_dma_base_addr = mapping->device_dma_base_addr;
	list_del(&mapping->node);
	mutex_unlock(&prime->dma_map_lock);

	ret = prime_dma_mapping_release(prime, mapping);
	mem_data->map_key = 0;
	return ret;
}

static long prime_mem_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct qcom_prime *prime = file->private_data;
	int ret;

	switch (cmd) {
	case PRIME_MEM_GET_REGION: {
		struct prime_mem_region mem_data;

		if (copy_from_user(&mem_data, (void __user *)arg,
				   sizeof(struct prime_mem_region)) != 0)
			return -EINVAL;
		dev_dbg(prime->dev, "PRIME PRIME_MEM_GET_REGION %d\n", mem_data.region);
		switch (mem_data.region) {
		case PRIME_MEM_REGION_MODEL:
			// For TZ-PIL mapped regions, VA = PA once PIL is invoked
			mem_data.base = prime->mem_model_base;
			mem_data.sz = prime->mem_model_size;
			break;
		case PRIME_MEM_REGION_TENSOR:
			return -EINVAL;
		case PRIME_MEM_REGION_SWIF:
			// For TZ-PIL mapped regions, VA = PA once PIL is invoked
			mem_data.base = prime->intf_table_phys;
			mem_data.sz = (phys_addr_t)prime->pdmem_base_phys +
				      prime->pdmem_size - prime->intf_table_phys;
			break;
		case PRIME_MEM_REGION_PDMEM:
			// For TZ-PIL mapped regions, VA = PA once PIL is invoked
			mem_data.base = (phys_addr_t)prime->pdmem_base_phys;
			mem_data.sz = prime->pdmem_size;
			break;
		case PRIME_MEM_REGION_MLM:
			mem_data.base = prime->mem_mlm_base_phys;
			mem_data.sz = prime->mem_mlm_size;
			break;
		default:
			return -EINVAL;
		}

		if (copy_to_user((void __user *)arg, &mem_data,
				 sizeof(struct prime_mem_region)) != 0)
			return -EINVAL;
		return 0;
	}
	case PRIME_MEM_PIL: {
		struct prime_mem_pil_map pil_data;

		if (copy_from_user(&pil_data, (void __user *)arg,
				   sizeof(struct prime_mem_pil_map)) != 0)
			return -EINVAL;

		if (pil_data.region != PRIME_MEM_REGION_MODEL) {
			dev_err(prime->dev, "PRIME_MEM_PIL only supports MODEL region\n");
			return -EINVAL;
		}

		/* Validate direction */
		BUILD_BUG_ON((int)PRIME_DMA_BIDIRECTIONAL != (int)DMA_BIDIRECTIONAL);
		BUILD_BUG_ON((int)PRIME_DMA_TO_DEVICE != (int)DMA_TO_DEVICE);
		BUILD_BUG_ON((int)PRIME_DMA_FROM_DEVICE != (int)DMA_FROM_DEVICE);

		if (pil_data.direction > PRIME_DMA_FROM_DEVICE) {
			dev_err(prime->dev, "Invalid DMA direction: %d\n", pil_data.direction);
			return -EINVAL;
		}

		switch (pil_data.action) {
		case PRIME_MEM_PIL_PREPARE:
			ret = prime_pil_prepare(prime, &pil_data);
			if (ret == 0) {
				if (copy_to_user((void __user *)arg, &pil_data,
							sizeof(struct prime_mem_pil_map)) != 0) {
					prime_pil_release(prime, &pil_data);
					return -EFAULT;
				}
			}
			break;

		case PRIME_MEM_PIL_RELEASE:
			ret = prime_pil_release(prime, &pil_data);
			break;

		case PRIME_MEM_PIL_SYNC:
			phys_addr_t phys_addr;

			/* Validate direction for sync operation */
			if (pil_data.direction != PRIME_DMA_TO_DEVICE &&
			    pil_data.direction != PRIME_DMA_FROM_DEVICE) {
				dev_err(prime->dev,
					"SYNC requires TO_DEVICE or FROM_DEVICE direction\n");
				return -EINVAL;
			}

			phys_addr = prime->mem_model_base + pil_data.offset;

			if (pil_data.direction == PRIME_DMA_TO_DEVICE) {
				/* Flush write-combine buffers: CPU -> Device */
				wmb();
				dev_dbg(prime->dev,
					"WMB sync: phys=0x%llx, size=0x%zx (CPU->Device)\n",
					(unsigned long long)phys_addr,
					pil_data.size);
			} else {
				/* Read barrier: Device -> CPU */
				rmb();
				dev_dbg(prime->dev,
					"RMB sync: phys=0x%llx, size=0x%zx (Device->CPU)\n",
					(unsigned long long)phys_addr,
					pil_data.size);
			}
			ret = 0;
			break;

		default:
			return -EINVAL;
		}

		return ret;
	}
	case PRIME_MEM_DMA: {
		struct prime_mem_dma_map mem_data;

		if (copy_from_user(&mem_data, (void __user *)arg,
				   sizeof(struct prime_mem_dma_map)) != 0)
			return -EINVAL;
		dev_dbg(prime->dev, "PRIME PRIME_MEM_DMA %d\n", mem_data.region);
		BUILD_BUG_ON((int)PRIME_DMA_BIDIRECTIONAL != (int)DMA_BIDIRECTIONAL);
		BUILD_BUG_ON((int)PRIME_DMA_TO_DEVICE != (int)DMA_TO_DEVICE);
		BUILD_BUG_ON((int)PRIME_DMA_FROM_DEVICE != (int)DMA_FROM_DEVICE);
		switch (mem_data.region) {
		case PRIME_MEM_REGION_TENSOR:
			switch (mem_data.action) {
			case PRIME_MEM_DMA_ALLOC:
				ret = prime_mem_dma_alloc(prime, &mem_data);
				if (ret)
					return ret;
				break;
			case PRIME_MEM_DMA_FREE:
				ret = prime_mem_dma_free(prime, &mem_data);
				if (ret)
					return ret;
				break;
			default:
				return -EINVAL;
			}
			break;
		default:
			return -EINVAL;
		}

		if (copy_to_user((void __user *)arg, &mem_data,
				 sizeof(struct prime_mem_dma_map)) != 0)
			return -EINVAL;
		return 0;
	}
	case PRIME_MEM_EVENT_SUBSCRIBE: {
		struct prime_event_subscribe cfg;
		pid_t tgid = task_tgid_nr(current);
		bool wake = false;

		if (MINOR(file->f_inode->i_rdev) != 1)
			return -EINVAL;

		if (copy_from_user(&cfg, (void __user *)arg,
				   sizeof(struct prime_event_subscribe)) != 0)
			return -EINVAL;

		if (cfg.flags != 0)
			return -EINVAL;
		if (cfg.enable != 0 && cfg.enable != 1)
			return -EINVAL;

		mutex_lock(&prime->irq_sub_lock);
		if (cfg.enable) {
			if (prime->irq_sub_file && prime->irq_sub_file != file) {
				ret = -EBUSY;
			} else {
				prime->irq_sub_file = file;
				prime->irq_sub_owner_tgid = tgid;
				ret = 0;
			}
		} else {
			if (prime->irq_sub_file && prime->irq_sub_file != file) {
				ret = -EPERM;
			} else {
				if (prime->irq_sub_file == file) {
					prime->irq_sub_file = NULL;
					prime->irq_sub_owner_tgid = 0;
					wake = true;
				}
				ret = 0;
			}
		}
		mutex_unlock(&prime->irq_sub_lock);

		if (wake) {
			prime_irq_queue_flush(prime);
			wake_up_interruptible(&prime->irq_event_wq);
		}
		return ret;
	}
	case PRIME_MEM_SET_CLK_BW: {
#ifndef BUILDROOT
		struct prime_clk_bw_config clk_bw_cfg;

		if (copy_from_user(&clk_bw_cfg, (void __user *)arg,
				   sizeof(struct prime_clk_bw_config)) != 0)
			return -EINVAL;
		dev_dbg(prime->dev, "PRIME_MEM_SET_CLK_BW: clk_level=%d, ddr_bw=%u, cpu_bw=%u\n",
			clk_bw_cfg.clk_level, clk_bw_cfg.ddr_bw, clk_bw_cfg.cpu_bw);
		return prime_set_clk_bw(prime, clk_bw_cfg.clk_level,
					clk_bw_cfg.ddr_bw, clk_bw_cfg.cpu_bw);
#else
		dev_dbg(prime->dev, "PRIME_MEM_SET_CLK_BW not supported in BUILDROOT\n");
		return -EOPNOTSUPP;
#endif
	}
	default:
		return -EINVAL;
	}
	return -EINVAL;
}

static int prime_mem_release(struct inode *inode, struct file *file)
{
	struct qcom_prime *prime = file->private_data;
	pid_t tgid = task_tgid_nr(current);
	bool wake = false;

	dev_dbg(prime->dev, "Release Cur PID[%d]\n", task_pid_nr(current));

	if (MINOR(inode->i_rdev) == 1) {
		mutex_lock(&prime->irq_sub_lock);
		if (prime->irq_sub_file == file) {
			prime->irq_sub_file = NULL;
			prime->irq_sub_owner_tgid = 0;
			wake = true;
		}
		mutex_unlock(&prime->irq_sub_lock);
	}

	if (wake) {
		prime_irq_queue_flush(prime);
		wake_up_interruptible(&prime->irq_event_wq);
	}

	if (current->flags & PF_EXITING)
		prime_dma_cleanup_owner(prime, tgid);
	if (current->flags & PF_EXITING)
		prime_pil_cleanup_owner(prime, tgid);

	return 0;
}

static ssize_t prime_mem_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct qcom_prime *prime = file->private_data;
	struct prime_irq_event evt = {0};
	int ret;

	if (MINOR(file->f_inode->i_rdev) != 1)
		return -EINVAL;
	if (count < sizeof(evt))
		return -EINVAL;
	if (!prime_irq_reader_is_subscribed(prime, file))
		return -EPERM;

	for (;;) {
		ret = prime_irq_event_pop(prime, &evt);
		if (!ret)
			break;
		if (ret != -EAGAIN)
			return ret;
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(prime->irq_event_wq,
					       prime_irq_event_available(prime) ||
					       !prime_irq_reader_is_subscribed(prime, file));
		if (ret)
			return ret;
		if (!prime_irq_reader_is_subscribed(prime, file))
			return -EPERM;
	}

	if (copy_to_user(buf, &evt, sizeof(evt)) != 0)
		return -EFAULT;

	return sizeof(evt);
}

/** Used for construcing a memory allocation/mapping device */
static const struct file_operations prime_mem_fops = {
	.owner = THIS_MODULE,
	.open = prime_mem_open,
	.read = prime_mem_read,
	.mmap = prime_mem_mmap,
	.unlocked_ioctl = prime_mem_ioctl,
	.release = prime_mem_release
};

/** Remoteproc Callback Implementation */
static const struct rproc_ops prime_rproc_ops = {
	.start = prime_start,
	.stop = prime_stop,
	.da_to_va = prime_rproc_da_to_va,
#ifndef BUILDROOT
	.load = prime_load, //TODO: loading .mdt file - depends on drivers/soc/qcom/mdt_loader.c
	//.parse_fw = qcom_register_dump_segments, //TODO: Depends on remoteproc/qcom_common.c
#endif
};

static int prime_alloc_memory_region(struct qcom_prime *prime)
{
	struct device_node *node;
	struct resource r;
	int ret = 0;

	node = of_parse_phandle(prime->dev->of_node, "memory-region", 0);
	if (!node) {
		dev_dbg(prime->dev, "no memory-region specified\n");
		return -EINVAL;
	}

	ret = of_address_to_resource(node, 0, &r);
	if (ret)
		return ret;

	u32 model_offset = (64 + 256) * 1024;

	prime->mem_phys = r.start;
	prime->mem_reloc = r.start;
	prime->mem_model_base = prime->mem_phys + model_offset;
	prime->mem_size = resource_size(&r);
	prime->mem_model_size = prime->mem_size - model_offset;
	prime->mem_region = devm_ioremap_wc(prime->dev, prime->mem_phys, prime->mem_size);
	if (!prime->mem_region) {
		dev_err(prime->dev, "unable to map memory region: %pa+%zx\n",
			&r.start, prime->mem_size);
		return -EBUSY;
	}

	return 0;
}

static void prime_cleanup_memory_region(struct qcom_prime *prime)
{

}

static int prime_create_cdev(struct qcom_prime *prime)
{
	int num_devs = PRIME_NUM_DEVS;
	dev_t dev;
	int ret = 0;

	prime->mem_count = 0;
	ret = alloc_chrdev_region(&prime->cdev_dev, 0, num_devs, "prime");
	if (ret < 0) {
		dev_dbg(prime->dev, "Cannot allocate major number\n");
		return ret;
	}

	dev = prime->cdev_dev;
	dev_dbg(prime->dev, "Major = %d Minor = %d\n", MAJOR(dev), MINOR(dev));

	/*Creating cdev structure*/
	cdev_init(&prime->cdev, &prime_mem_fops);

	/*Adding character device to the system*/
	ret = cdev_add(&prime->cdev, dev, num_devs);
	if (ret < 0) {
		dev_dbg(prime->dev, "Cannot add the device to the system\n");
		goto r_class;
	}

	/*Creating struct class*/
	prime->cdev_class = class_create("prime");
	if (IS_ERR(prime->cdev_class)) {
		dev_dbg(prime->dev, "Failed to create struct Class\n");
		goto r_cdev;
	}

	/*Creating device*/
	for (int i = 0; i < num_devs; i++) {
		if (IS_ERR(device_create(prime->cdev_class, NULL,
					 MKDEV(MAJOR(dev), MINOR(dev) + i),
					 NULL, "prime%d", i))) {
			dev_dbg(prime->dev, "Failed to create device node %d\n", i);
			while (i--)
				device_destroy(prime->cdev_class,
					MKDEV(MAJOR(dev), MINOR(dev) + i));
			goto r_device;
		}
	}

	dev_dbg(prime->dev, "PRIME CDEV created!\n");

	return 0;

r_device:
	class_destroy(prime->cdev_class);
r_cdev:
	cdev_del(&prime->cdev);
r_class:
	unregister_chrdev_region(dev, num_devs);
	return -EINVAL;
}

static void prime_cleanup_dev(struct qcom_prime *prime)
{
	dev_t dev = prime->cdev_dev;
	int num_devs = PRIME_NUM_DEVS;

	for (int i = 0; i < num_devs; i++)
		device_destroy(prime->cdev_class, MKDEV(MAJOR(dev), MINOR(dev) + i));
	class_destroy(prime->cdev_class);
	cdev_del(&prime->cdev);
	unregister_chrdev_region(dev, num_devs);
}

static int prime_init_mmio(struct platform_device *pdev, struct qcom_prime *prime)
{
	struct resource *pss, *res;
	struct device *dev = &pdev->dev;
	int ret;

	/* get memory resource (just one MMAP for the entire PRIME aperture */
	pss = platform_get_resource_byname(pdev, IORESOURCE_MEM, "primess");
	prime->reg_ss_base = devm_ioremap_resource(dev, pss);
	if (IS_ERR(prime->reg_ss_base)) {
		dev_dbg(dev, "Get resource primess\n");
		return PTR_ERR(prime->reg_ss_base);
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "pdmem");
	if (!res) {
		dev_dbg(dev, "Get resource pdmem\n");
		ret = -ENODEV;
		goto r_unmap;
	}
	prime->pdmem_base = FW_PRIMESS_REG_ADDR(prime->reg_ss_base, res->start);
	prime->pdmem_base_phys = (phys_addr_t)FW_PRIMESS_REG_ADDR(pss->start, res->start);
	prime->pdmem_size = resource_size(res);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mlmem");
	if (!res) {
		dev_dbg(dev, "Get resource mlmem\n");
		ret = -ENODEV;
		goto r_unmap;
	}
	prime->mem_mlm_base_phys = (phys_addr_t)FW_PRIMESS_REG_ADDR(pss->start, res->start);
	prime->mem_mlm_size = resource_size(res);

#define IU_CORE_INTF_BASE_XPU (prime->pdmem_size - (16 * 1024))
	prime->intf_table = FW_PRIMESS_REG_ADDR(prime->pdmem_base, IU_CORE_INTF_BASE_XPU);
	prime->intf_table_phys =
		(phys_addr_t)FW_PRIMESS_REG_ADDR(prime->pdmem_base_phys, IU_CORE_INTF_BASE_XPU);

#ifdef BUILDROOT
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "iuss-cmn");
	if (!res) {
		dev_dbg(dev, "Get resource iuss-cmn\n");
		ret = -ENODEV;
		goto r_unmap;
	}
	prime->iuss_dev.reg_cmn_base = FW_PRIMESS_REG_ADDR(prime->reg_ss_base, res->start);
	prime->iuss_dev.reg_ss_base = prime->reg_ss_base;
	prime->iuss_dev.dev = prime->dev;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "iu0-csr");
	if (!res) {
		dev_dbg(dev, "Get resource iuss-csr\n");
		ret = -ENODEV;
		goto r_unmap;
	}
	prime->iuss_dev.reg_core_csr[0] = FW_PRIMESS_REG_ADDR(prime->reg_ss_base, res->start);
	prime->iuss_dev.pdmem_base = prime->pdmem_base;
	prime->iuss_dev.intf_table = prime->intf_table;
#endif /* BUILDROOT */

	dev_dbg(dev, "Remapped PRIME memory to 0x%p from 0x%pax\n",
		&prime->reg_ss_base, &pss->start);

	/* get the interrupt exposed by PRIME */
	int irq = platform_get_irq_byname(pdev, "boot-done");

	if (irq < 0) {
		dev_dbg(dev, "Unable to find IRQ to map (%d).\n", irq);
		ret = -ENODEV;
		goto r_unmap;
	}
	prime->prime_boot_done_irq = irq;

	/* setup interrupt handling */
	ret = devm_request_threaded_irq(dev, prime->prime_boot_done_irq, NULL,
					prime_irq_boot_done,
					IRQF_TRIGGER_RISING | IRQF_ONESHOT,
					"prime boot-done", prime);
	if (ret) {
		dev_dbg(dev, "Unable to register IRQ %d (error: %d)\n", irq, ret);
		goto r_unmap;
	}

	/* get the interrupt exposed by PRIME */
	irq = platform_get_irq_byname(pdev, "task-done");
	if (irq < 0) {
		dev_dbg(dev, "Unable to find IRQ to map (%d).\n", irq);
		ret = -ENODEV;
		goto r_boot_irq;
	}
	prime->prime_task_done_irq = irq;

	/* setup interrupt handling */
	ret = devm_request_threaded_irq(dev, prime->prime_task_done_irq, NULL,
					prime_irq_task_done,
					IRQF_TRIGGER_RISING | IRQF_ONESHOT,
					"prime task-done", prime);
	if (ret) {
		dev_dbg(dev, "Unable to register IRQ %d (error: %d)\n", irq, ret);
		goto r_boot_irq;
	}

	/* get the error interrupt exposed by PRIME */
	irq = platform_get_irq_byname(pdev, "error");
	if (irq < 0) {
		dev_dbg(dev, "Unable to find error IRQ to map (%d).\n", irq);
		ret = -ENODEV;
		goto r_task_irq;
	}
	prime->prime_error_irq = irq;

	/* setup error interrupt handling */
	ret = devm_request_threaded_irq(dev, prime->prime_error_irq, NULL,
					prime_irq_error,
					IRQF_TRIGGER_RISING | IRQF_ONESHOT,
					"prime error", prime);
	if (ret) {
		dev_dbg(dev, "Unable to register error IRQ %d (error: %d)\n", irq, ret);
		goto r_task_irq;
	}

	/* get the wdog interrupt exposed by PRIME */
	irq = platform_get_irq_byname(pdev, "wdog");
	if (irq < 0) {
		dev_dbg(dev, "Unable to find wdog IRQ to map (%d).\n", irq);
		ret = -ENODEV;
		goto r_error_irq;
	}
	prime->prime_wdog_irq = irq;

	/* setup wdog interrupt handling */
	ret = devm_request_threaded_irq(dev, prime->prime_wdog_irq, NULL,
					prime_irq_wdog,
					IRQF_TRIGGER_RISING | IRQF_ONESHOT,
					"prime wdog", prime);
	if (ret) {
		dev_dbg(dev, "Unable to register wdog IRQ %d (error: %d)\n", irq, ret);
		goto r_error_irq;
	}

	/* get the misc interrupt exposed by PRIME */
	irq = platform_get_irq_byname(pdev, "misc");
	if (irq < 0) {
		dev_dbg(dev, "Unable to find misc IRQ to map (%d).\n", irq);
		ret = -ENODEV;
		goto r_wdog_irq;
	}
	prime->prime_misc_irq = irq;

	/* setup misc interrupt handling */
	ret = devm_request_threaded_irq(dev, prime->prime_misc_irq, NULL,
					prime_irq_misc,
					IRQF_TRIGGER_RISING | IRQF_ONESHOT,
					"prime misc", prime);
	if (ret) {
		dev_dbg(dev, "Unable to register misc IRQ %d (error: %d)\n", irq, ret);
		goto r_wdog_irq;
	}

	return 0;

r_wdog_irq:
r_error_irq:
r_task_irq:
r_boot_irq:
r_unmap:
	return ret;
}

static void prime_cleanup_mmio(struct qcom_prime *prime)
{
}

/**
 *
 *  PRIME device probe.
 *
 *  Create an "PROC" device and register with the kernel.
 *
 */
static int qcom_prime_probe(struct platform_device *pdev)
{
	const struct prime_data *desc;
	struct device *dev = &pdev->dev;
	struct qcom_prime *prime;
	struct rproc *rproc;
	const char *fw_name;
	int ret;

	desc = of_device_get_match_data(&pdev->dev);
	if (!desc)
		return -EINVAL;

	fw_name = desc->firmware_name;
	/* Override fw_name if the firmware-name property exists in DT */
	ret = of_property_read_string(pdev->dev.of_node, "firmware-name", &fw_name);
	if (ret < 0 && ret != -EINVAL && ret != -ENODATA) {
		dev_err(dev, "Error reading firmware-name: %d\n", ret);
		return ret;
	}

	/* create an RPROC device */
	rproc = rproc_alloc(&pdev->dev, desc->sysmon_name, &prime_rproc_ops,
			    fw_name, sizeof(*prime));

	if (!rproc) {
		dev_dbg(dev, "Unable to allocate a remote processor\n");
		return -ENOMEM;
	}

	/* initialize pointers */
	prime = rproc->priv;
	prime->dev = dev;
	prime->rproc = rproc;
	prime->pas_id = desc->pas_id;
	prime->config_mmu = desc->config_mmu;
	prime->tmelcom_support = desc->tmelcom_support;
	init_completion(&prime->start_done);

	/* Initialize PRIME lifecycle and SWIF mapping protection */
	prime->prime_started = false;
	atomic_set(&prime->swif_mmap_count, 0);
	mutex_init(&prime->dma_map_lock);
	INIT_LIST_HEAD(&prime->dma_mappings);
	atomic64_set(&prime->dma_map_key_next, 0);
	mutex_init(&prime->pil_map_lock);
	INIT_LIST_HEAD(&prime->pil_mappings);
	atomic64_set(&prime->pil_map_key_next, 0);
	spin_lock_init(&prime->irq_event_lock);
	prime->irq_event_head = 0;
	prime->irq_event_tail = 0;
	prime->irq_event_count = 0;
	prime->irq_event_overflow = false;
	prime->irq_event_seqno = 0;
	init_waitqueue_head(&prime->irq_event_wq);
	mutex_init(&prime->irq_sub_lock);
	prime->irq_sub_file = NULL;
	prime->irq_sub_owner_tgid = 0;

	prime->primess_rif_last_val = 0;

	/* Initialize clock state tracking */
	prime->clk_enabled = false;
	prime->clk_level = PRIME_CLK_LEVEL_OFF;
	prime->clk_ddr_bw = 0;
	prime->clk_cpu_bw = 0;

	rproc->auto_boot = desc->auto_boot;

	rproc->recovery_disabled = true;
	rproc_coredump_set_elf_info(rproc, ELFCLASS32, EM_NONE);

	ret = device_init_wakeup(prime->dev, true);
	if (ret < 0)
		goto r_rproc;

	/*
	 * IMPORTANT:
	 * PRIME FW currently does not include a resource table.
	 * Override parse_fw so remoteproc does not fail during ELF parsing.
	 * Remove this once firmware provides the resource table.
	 */
	rproc->ops->parse_fw = NULL;

#ifndef BUILDROOT
	ret = prime_init_clock_power(prime);
	if (ret < 0)
		goto r_wakeup;
#endif

	ret = prime_init_mmio(pdev, prime);
	if (ret < 0)
		goto r_wakeup;

	ret = prime_create_cdev(prime);
	if (ret < 0)
		goto r_mmio;

	ret = prime_alloc_memory_region(prime);
	if (ret < 0)
		goto r_cdev;

	/* create device attributes in sysfs */
	ret = sysfs_create_group(&dev->kobj, &prime_attr_group);
	if (ret < 0) {
		dev_dbg(dev, "Unable to create sysfs group (error: %d)\n", ret);
		goto r_mem_region;
	}

	/* register this RPROC device */
	ret = rproc_add(rproc);
	if (ret < 0) {
		dev_dbg(dev, "Failed to register remote processor (error: %d)!\n", ret);
		goto r_sysfs;
	}

	/* initialize driver data */
	platform_set_drvdata(pdev, prime);

	return 0;

r_sysfs:
	sysfs_remove_group(&dev->kobj, &prime_attr_group);
r_mem_region:
	prime_cleanup_memory_region(prime);
r_cdev:
	prime_cleanup_dev(prime);
r_mmio:
	prime_cleanup_mmio(prime);
r_wakeup:
	device_init_wakeup(prime->dev, false);
r_rproc:
	rproc_free(rproc);

	return ret;
}

static void qcom_prime_remove(struct platform_device *pdev)
{
	struct qcom_prime *prime = platform_get_drvdata(pdev);
	int ret;
	int mmap_count;

	/* Safety check: if PRIME is still running during remove */
	if (prime->prime_started) {
		mmap_count = atomic_read(&prime->swif_mmap_count);

		if (mmap_count != 0) {
			dev_err(prime->dev, "============================================\n");
			dev_err(prime->dev,
				"WARNING: FORCED MODULE REMOVAL WITH %d ACTIVE SWIF MAPPINGS!\n",
				mmap_count);
			dev_err(prime->dev, "============================================\n");

			/* Force clear the mmap count to allow prime_stop to proceed */
			prime->prime_started = false;
			atomic_set(&prime->swif_mmap_count, 0);
		}

		/* Now stop PRIME subsystem */
		ret = prime_stop(prime->rproc);
		if (ret < 0)
			dev_err(prime->dev, "Failed to stop PRIME during removal: %d\n", ret);
	}

	ret = rproc_del(prime->rproc);
	if (ret < 0)
		dev_err(prime->dev, "Failed to unregister remote processor (error: %d)!\n", ret);

	sysfs_remove_group(&pdev->dev.kobj, &prime_attr_group);

	prime_cleanup_memory_region(prime);
	prime_dma_cleanup_owner(prime, -1);
	prime_pil_cleanup_owner(prime, -1);
	prime_irq_queue_flush(prime);
	prime_cleanup_dev(prime);
	prime_cleanup_mmio(prime);

	ret = device_init_wakeup(prime->dev, false);
	if (ret < 0)
		dev_err(prime->dev, "Unable to de-init wakeup (error: %d)\n", ret);
	rproc_free(prime->rproc);

}

static const struct prime_data prime_resource_init = {
	.firmware_name = "qcom_prime_a.elf",
	.pas_id = 81,
	.ssr_name = "prime",
	.sysmon_name = "primess",
	.auto_boot = false,
	.qmp_name = "prime",
	.config_mmu = true,
};

static const struct prime_data prime_resource_init_ipq = {
	.firmware_name = "qcom_prime_ipq.elf",
	.pas_id = 0xc3,
	.ssr_name = "prime",
	.sysmon_name = "primess",
	.auto_boot = false,
	.qmp_name = "prime",
	.config_mmu = false,
	.tmelcom_support = true,
};

static const struct prime_data prime_resource_generic = {
	.firmware_name = "qcom_prime.elf",
	.pas_id = 81,
	.ssr_name = "prime",
	.sysmon_name = "primess",
	.auto_boot = false,
	.qmp_name = "prime",
	.config_mmu = false,
};

static const struct of_device_id prime_of_match[] = {
	{ .compatible = "qcom,generic-primess-pas", .data = &prime_resource_generic },
	{ .compatible = "qcom,echo-primess-pas", .data = &prime_resource_init },
	{ .compatible = "qcom,ipq9650-primess-pas", .data = &prime_resource_init_ipq },
	{},
};
MODULE_DEVICE_TABLE(of, prime_of_match);

static struct platform_driver prime_driver = {
	.probe = qcom_prime_probe,
#if LINUX_VERSION_CODE > KERNEL_VERSION(6, 11, 0)
	.remove = qcom_prime_remove,
#else
	.remove_new = qcom_prime_remove,
#endif
	.driver = {
		.name = "qcom_prime",
		.of_match_table = prime_of_match,
	},
};
module_platform_driver(prime_driver);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
MODULE_IMPORT_NS("DMA_BUF");
#else
MODULE_IMPORT_NS(DMA_BUF);
#endif

MODULE_DESCRIPTION("QTI Peripheral Image Loader for PRIME Secure Subsystem");
MODULE_LICENSE("GPL");
