/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */


#include <linux/dma-direction.h>
#include <linux/dma-buf.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioctl.h>
#include <linux/kernel.h>
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



#ifndef BUILDROOT
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/interconnect.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/soc/qcom/mdt_loader.h>
#include <linux/tmelcom_ipc.h>
#endif

#include "prime_dev.h"

#define IPQ9650_GCC_BASE 0x01800000
#define IPQ9676_GCC_SIZE 0x40000

/* GCC register offsets (base: ipq9650_GCC_BASE) */
#define GCC_SNOC_PRIMESS_AXIM_CBCR		0x2E0E0
#define GCC_CNOC_PRIMESS_AHBS_CBCR		0x310BC


struct iuss_dev {
	struct device * dev;
	void __iomem * reg_ss_base;      /*!< HWIO base address for IUSS */
	void __iomem * reg_cmn_base;     /*!< HWIO base address for IUSS common CSR */
	void __iomem * reg_core_csr[1];  /*!< HWIO base address for CoreX IUSS CSRs */
	void __iomem * pdmem_base;       /*!< virtual PDMEM base address */
	u32 __iomem * intf_table;        /*!< virtual address of interface ptr table in PDMEM */
};

struct qcom_prime {
  void __iomem * reg_ss_base;   /*!< HWIO base address for IUSS */

  void __iomem * pdmem_base;       /*!< virtual PDMEM base address */
  phys_addr_t pdmem_base_phys;    /*!< Physical address of PDMEM base */
  size_t pdmem_size;                 /*!< PDMEM size, in bytes */
  //fw_mem_heap_t pdmem_heap;     /*!< Heap for PDMEM reservation */
  u32 __iomem * intf_table;       /*!< virtual address of interface ptr table in PDMEM */
  phys_addr_t intf_table_phys;    /*!< Physical address of interface */

  phys_addr_t mem_mlm_base_phys;    /*!< Physical address of ML TOP memory */
  size_t mem_mlm_size;              /*!< Size of MLM */

  char * name;            /*!< Name of IUSS, for debug */

  struct rproc * rproc;
  struct device * dev;   /* Device pointer */

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
  bool prime_started;              /* True after successful prime_start(), false after prime_stop() */
  atomic_t swif_mmap_count;        /* Count of active SWIF mappings */

  phys_addr_t mem_phys;  /* Base address of PIL carveout */
  phys_addr_t mem_reloc; /* Copy of mem_phys */
  phys_addr_t mem_model_base; /* Base address of model storage. Must be within mem_phys < mem_phys+mem_size */
  void *mem_region; /* Kernel driver vm remapped region*/
  size_t mem_size; /* Carveout size*/
  size_t mem_model_size; /*Size of model memory within carveout */

  struct cdev cdev;
  dev_t cdev_dev;
  struct class *cdev_class;

  struct iuss_dev iuss_dev;

  //IRQ forwarding
  struct task_struct *sig_task[PRIME_IRQ2L_MAX+1];
  int signo[PRIME_IRQ2L_MAX+1];

  struct clk * core_clk;
  struct icc_path *icc_ddr;
  struct icc_path *icc_cpu;

  /* Clock state tracking */
  bool clk_enabled;
  enum prime_clk_level clk_level;
  u32 clk_ddr_bw;
  u32 clk_cpu_bw;

  //struct memory_descriptor mem_descs[10];
  int mem_count;

  bool config_mmu;

  /* For PRIMESS_RIF */
  u32 primess_rif_last_val;
};


//TODO: Move these definitions to device tree?


/// PRIME SS Condition Status Registers (Relative Byte Offset)
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

#define FW_PRIMESS_REG_ADDR(_base, _offset) ((u32 *)((uintptr_t)_base + (_offset)))

/*
 * QCOM PRIME Device Driver.
 */

struct prime_data {
	const char *firmware_name;
	int pas_id;
	const char *ssr_name;
	bool auto_boot;
	const char *qmp_name;
	bool config_mmu;
	bool tmelcom_support;
};

static void signal_userspace(struct qcom_prime *prime, int irq_id)
{
	//Signal Userspace
	if (prime->sig_task[irq_id]) {
		struct kernel_siginfo info;
		memset(&info, 0, sizeof(struct kernel_siginfo));
		info.si_signo = prime->signo[irq_id];
		info.si_code = SI_QUEUE;
		info.si_int = 1;
		dev_dbg(prime->dev, "Signaling Userspace %d\n", prime->signo[irq_id]);

		if (send_sig_info(prime->signo[irq_id], &info, prime->sig_task[irq_id]) < 0) {
			dev_err(prime->dev, "Userspace PID[%d] registered, but unable to send signal\n",
				prime->sig_task[irq_id]->pid);
		}
	}
}

static irqreturn_t prime_irq_boot_done(int irq, void *data)
{
	struct qcom_prime *prime = data;
	int irq_id = PRIME_IRQ2L_BOOT_DONE;
	//pr_info("IUSS IRQ handler called!");
	dev_dbg(prime->dev, "PRIME CMD/RSP IRQ handler (%d) triggered!", irq);
	writel_relaxed(1 << (irq_id), prime->reg_ss_base + PRIMESS_IRQ2L_PEND_CLR);
	#ifdef BUILDROOT
	complete(&prime->start_done);
	#endif
	signal_userspace(prime, irq_id);
	return IRQ_HANDLED;
}

static irqreturn_t prime_irq_task_done(int irq, void *data)
{
	struct qcom_prime *prime = data;
	int irq_id = PRIME_IRQ2L_TASK_DONE;
	dev_dbg(prime->dev, "PRIME Task Complete IRQ handler (%d) triggered!", irq);
	writel_relaxed(1 << (irq_id), prime->reg_ss_base + PRIMESS_IRQ2L_PEND_CLR);

	signal_userspace(prime, irq_id);

	return IRQ_HANDLED;
}

static irqreturn_t prime_irq_error(int irq, void *data)
{
	struct qcom_prime *prime = data;
	int irq_id = PRIME_IRQ2L_ERROR;
	dev_err(prime->dev, "PRIME Error IRQ handler (%d) triggered!", irq);
	writel_relaxed(1 << (irq_id), prime->reg_ss_base + PRIMESS_IRQ2L_PEND_CLR);
	#ifndef BUILDROOT
	complete(&prime->start_done);
	#endif
	signal_userspace(prime, irq_id);

	return IRQ_HANDLED;
}

static irqreturn_t prime_irq_wdog(int irq, void *data)
{
	struct qcom_prime *prime = data;
	int irq_id = PRIME_IRQ2L_ERROR; //TODO: fix this
	dev_err(prime->dev, "PRIME Watchdog IRQ handler (%d) triggered!", irq);
	/* Note: Watchdog interrupt cannot be cleared without additional action */

	signal_userspace(prime, irq_id);

	return IRQ_HANDLED;
}

static irqreturn_t prime_irq_misc(int irq, void *data)
{
	struct qcom_prime *prime = data;
	int irq_id = PRIME_IRQ2L_MISC;
	dev_dbg(prime->dev, "PRIME Misc IRQ handler (%d) triggered!", irq);
	writel_relaxed(1 << (irq_id), prime->reg_ss_base + PRIMESS_IRQ2L_PEND_CLR);

	signal_userspace(prime, irq_id);

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
#define PRIME_ATTR_PERM_RW  0600  /* Read-Write (root only) */
#define PRIME_ATTR_PERM_RO  0400  /* Read-Only (root only) */
#define PRIME_ATTR_PERM_WO  0200  /* Write-Only (root only) */

/**
 *   Declare a R/W SysFs attribute corresponding to a register.
 *
 *   @_reg: Register name from prime_hwio_reg.h
 *   @_perm: Permission mode (PRIME_ATTR_PERM_RW, PRIME_ATTR_PERM_RO, or PRIME_ATTR_PERM_WO)
 */
#define DECL_PRIME_RW_ATTR(_reg, _perm)                                                                                            \
	static ssize_t _reg##_show_cmd(struct device *dev, struct device_attribute *attr, char *buf)                               \
	{                                                                                                                          \
		struct qcom_prime *vp = dev_get_drvdata(dev);                                                                      \
		u32 val = readl_relaxed(FW_PRIMESS_REG_ADDR(vp->reg_ss_base, (_reg)));                                                   \
		return scnprintf(buf, PAGE_SIZE, "0x%x\n", val);                                                                    \
	}                                                                                                                          \
	static ssize_t _reg##_store_cmd(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)            \
	{                                                                                                                          \
		struct qcom_prime *vp = dev_get_drvdata(dev);                                                                      \
		u32 val;                                                                                                           \
		if (kstrtou32(buf, 0, &val) != 0) {                                                                                 \
			if (len == sizeof(u32)) {                                                                                  \
				val = *(u32 *)buf;                                                                                 \
			} else if (len == sizeof(u8)) {                                                                            \
				val = *(u8 *)buf;                                                                                  \
			} else {                                                                                                   \
				return -EINVAL;                                                                                    \
			}                                                                                                          \
		}                                                                                                                  \
		writel(val, FW_PRIMESS_REG_ADDR(vp->reg_ss_base, (_reg)));                                                               \
		return len;                                                                                                        \
	}                                                                                                                          \
	static DEVICE_ATTR(_reg, (_perm), _reg##_show_cmd, _reg##_store_cmd);

/** Declare R/W SysFs attributes */
DECL_PRIME_RW_ATTR(PRIMESS_IRQ2L_PEND_CLR, PRIME_ATTR_PERM_WO);
DECL_PRIME_RW_ATTR(PRIMESS_IRQ2L_PEND_SET, PRIME_ATTR_PERM_WO);
DECL_PRIME_RW_ATTR(PRIMESS_IRQ2L_PEND_STAT, PRIME_ATTR_PERM_RO);
DECL_PRIME_RW_ATTR(PRIMESS_IUSS_IRQ_PEND_WDATA_WORD0, PRIME_ATTR_PERM_WO);


static ssize_t PRIMESS_RIF_show_cmd(struct device *dev, struct device_attribute *attr, char *buf)
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
static ssize_t PRIMESS_RIF_store_cmd(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
	struct qcom_prime *vp = dev_get_drvdata(dev);
	u32 addr, val;
	char *colon_pos;
	bool is_write;

	/* Check if this is a write (contains ':') or read operation */
	colon_pos = strchr(buf, ':');
	is_write = (colon_pos != NULL);

	if (is_write) {
		/* Parse "ADDR:VAL" format */
		char addr_str[32], val_str[32];
		int addr_len = colon_pos - buf;

		if (addr_len >= sizeof(addr_str))
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

static DEVICE_ATTR(PRIMESS_RIF, PRIME_ATTR_PERM_RW, PRIMESS_RIF_show_cmd, PRIMESS_RIF_store_cmd);

#ifndef BUILDROOT

/* Clock level to frequency mapping - available for both BUILDROOT and non-BUILDROOT */
static u32 prime_get_clk(enum prime_clk_level clk_rate)
{
	switch(clk_rate){
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

/* Bandwidth table - available for both BUILDROOT and non-BUILDROOT */
static struct prime_bw_table_val {
	u32 mem_bw;
	u32 cfg_bw;
} prime_bw_table[PRIME_CLK_LEVEL_TURBO + 1] = {
	[PRIME_CLK_LEVEL_OFF]      = { 0,       0 }, /* Bandwidth values in KB/s */
	[PRIME_CLK_LEVEL_LSVS]     = { 922,     1000 },
	[PRIME_CLK_LEVEL_SVS]      = { 1844,    1000 },
	[PRIME_CLK_LEVEL_SVS_L1]   = { 3688,    1000 },
	[PRIME_CLK_LEVEL_NOM]      = { 7376,    102400 },
	[PRIME_CLK_LEVEL_TURBO]    = { 14752,   102400 },
};

/* Forward declaration for prime_set_clk_bw (defined in non-BUILDROOT section) */
static int prime_set_clk_bw(struct qcom_prime* prime, enum prime_clk_level clk_level, u32 ddr_bw, u32 cpu_bw);
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
static ssize_t PRIMESS_CLK_BW_show_cmd(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct qcom_prime *vp = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%d %u %u\n",
			 vp->clk_level, vp->clk_ddr_bw, vp->clk_cpu_bw);
}

static ssize_t PRIMESS_CLK_BW_store_cmd(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
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

static DEVICE_ATTR(PRIMESS_CLK_BW, 0600, PRIMESS_CLK_BW_show_cmd, PRIMESS_CLK_BW_store_cmd);

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
	if((ret = fw_iu_rproc_stop_ss(&prime->iuss_dev)) != 0) {
		dev_dbg(prime->dev, "Error Stopping Core");
		return ret;
	}
	if((ret = fw_iu_rproc_enable_pdmem(&prime->iuss_dev, 0)) != 0) {
		dev_dbg(prime->dev, "Error UnPreparing Core\n");
	}
	return ret;
}

static int qcom_scm_pas_auth_and_reset(struct rproc *rproc)
{
	int ret;
	struct qcom_prime *prime = (struct qcom_prime *)rproc->priv;
	void *pdmem_base = prime->pdmem_base;
	dev_dbg(prime->dev, "Memcpy PRIME 0x%08llx -> 0x%08llx (%zu)byte", (uint64_t)prime->mem_region, (uint64_t)pdmem_base,
		prime->pdmem_size);

	//Enable PDMEM
	if((ret = fw_iu_rproc_enable_pdmem(&prime->iuss_dev, 1)) != 0) {
		dev_dbg(prime->dev, "Error Preparing Core\n");
		return ret;
	}

	// Old model thows an exception if we write the last word of PDMEM
	// Check to make sure model is good
	writel(0xcafebabe, FW_PRIMESS_REG_ADDR(prime->pdmem_base, prime->pdmem_size-4));
	memcpy_toio(pdmem_base, prime->mem_region, prime->pdmem_size);
	wmb();

	if ((ret = fw_iu_rproc_boot_ss(&prime->iuss_dev)) < 0) {
		dev_dbg(prime->dev, "Error Starting Core\n");
	}

	return ret;
}
#endif

#ifndef BUILDROOT
#define PRIME_KM_VOTE_BIT (1)

static int prime_set_clk_bw(struct qcom_prime* prime, enum prime_clk_level clk_level, u32 ddr_bw, u32 cpu_bw)
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

		if (prime->icc_ddr) {
			icc_set_bw(prime->icc_ddr, 0, ddr_bw);
		}
		if (prime->icc_cpu) {
			icc_set_bw(prime->icc_cpu, 0, cpu_bw);
		}

		/* Update state */
		prime->clk_level = PRIME_CLK_LEVEL_OFF;
		prime->clk_ddr_bw = ddr_bw;
		prime->clk_cpu_bw = cpu_bw;

		dev_dbg(dev, "Clock disabled, ddr_bw=%u, cpu_bw=%u\n", ddr_bw, cpu_bw);
		return 0;
	}

	/* Enable clock if not already enabled */
	if (!prime->clk_enabled) {
		if (!prime->core_clk) {
			dev_err(dev, "core_clk is NULL\n");
			return -EINVAL;
		}
		dev_dbg(dev, "Enabling clock\n");
		if ((ret = clk_prepare_enable(prime->core_clk)) != 0) {
			dev_err(dev, "clk_prepare_enable failed. ret: %d\n", ret);
			return ret;
		}
		prime->clk_enabled = true;
	}

	/* Convert clock level to actual rate */
	clk_rate = prime_get_clk(clk_level);

	if((ret = clk_set_rate(prime->core_clk, clk_rate)) != 0) {
		dev_err(dev, "clk_set_rate failed. ret: %d\n", ret);
		return ret;
	}

	if (prime->icc_ddr && prime->icc_cpu) {
		if ((ret = icc_set_bw(prime->icc_ddr, 0, ddr_bw)) != 0) {
			dev_err(dev, "failed to set ddr bandwidth request: %d\n", ret);
			return ret;
		}

		if ((ret = icc_set_bw(prime->icc_cpu, 0, cpu_bw)) != 0) {
			dev_err(dev, "failed to set cpu bandwidth request: %d\n", ret);
			return ret;
		}
	}

	/* Update state */
	prime->clk_level = clk_level;
	prime->clk_ddr_bw = ddr_bw;
	prime->clk_cpu_bw = cpu_bw;

	dev_dbg(dev, "Set clk_level=%d, clk_rate=%lu, ddr_bw=%u, cpu_bw=%u\n", clk_level, clk_rate, ddr_bw, cpu_bw);

	#if 0
	{
		dev_dbg(dev, "Delay for 0.25ms before reading GCC registers\n");
		usleep_range(250, 250);

		void __iomem *gcc_base = ioremap(0x110000, 0x100);
		if (gcc_base) {
			dev_dbg(dev, "GCC_PRIME_BCR (0x110000): 0x%08x\n", readl_relaxed(gcc_base + 0x0));
			dev_dbg(dev, "GCC_PRIME_AHB_M_CBCR (0x110004): 0x%08x\n", readl_relaxed(gcc_base + 0x4));
			dev_dbg(dev, "GCC_PRIME_AHB_S_CBCR (0x110008): 0x%08x\n", readl_relaxed(gcc_base + 0x8));
			dev_dbg(dev, "GCC_PRIME_AXI_CBCR (0x11000C): 0x%08x\n", readl_relaxed(gcc_base + 0xc));
			dev_dbg(dev, "GCC_PRIME_XO_CBCR (0x110010): 0x%08x\n", readl_relaxed(gcc_base + 0x10));
			dev_dbg(dev, "GCC_PRIME_CORE_CBCR (0x110014): 0x%08x\n", readl_relaxed(gcc_base + 0x14));
			dev_dbg(dev, "GCC_PRIME_AT_CBCR (0x110018): 0x%08x\n", readl_relaxed(gcc_base + 0x18));
			dev_dbg(dev, "GCC_PRIME_CORE_CMD_RCGR (0x11001C): 0x%08x\n", readl_relaxed(gcc_base + 0x1c));
			dev_dbg(dev, "GCC_PRIME_CORE_CFG_RCGR (0x110020): 0x%08x\n", readl_relaxed(gcc_base + 0x20));
			dev_dbg(dev, "GCC_PRIME_CORE_DCD_CDIV_DCDR (0x110034): 0x%08x\n", readl_relaxed(gcc_base + 0x34));
			dev_dbg(dev, "GCC_SYS_NOC_PRIME_AXI_CBCR (0x110038): 0x%08x\n", readl_relaxed(gcc_base + 0x38));
			dev_dbg(dev, "GCC_SYS_NOC_PRIME_AHB_CBCR (0x11003C): 0x%08x\n", readl_relaxed(gcc_base + 0x3c));
			dev_dbg(dev, "GCC_SNOC_CNOC_PRIME_AHB_CBCR (0x110040): 0x%08x\n", readl_relaxed(gcc_base + 0x40));

			iounmap(gcc_base);
		}
	}
	#endif
	return 0;
}

static int prime_init_clock_power(struct qcom_prime* prime)
{
	struct device *dev = prime->dev;
	int ret;

	prime->core_clk = devm_clk_get(dev, "core-clk");
	if (IS_ERR_OR_NULL(prime->core_clk)) {
		dev_dbg(dev, "devm_clk_get failed for core-clk\n");
		return PTR_ERR(prime->core_clk);
	}

	prime->icc_ddr = devm_of_icc_get(dev, "prime-ddr");
	if (IS_ERR(prime->icc_ddr)){
		dev_dbg(dev, "No interconnects in DT\n");
		prime->icc_ddr = NULL;
		prime->icc_cpu = NULL;
	}
	else{
		prime->icc_cpu = devm_of_icc_get(dev, "cpu-prime");
		if (IS_ERR(prime->icc_cpu))
			return dev_err_probe(dev, PTR_ERR(prime->icc_cpu),
						"failed to acquire interconnect cpu\n");

		/* Include these votes as part of probing since interconnect driver holds a proxy vote
			setting clks to turbo until we vote for ourselves.
		*/
		if ((ret = icc_set_bw(prime->icc_ddr, 0, 0)) != 0) {
			dev_dbg(dev, "failed to set ddr bandwidth request: %d\n", ret);
			goto r_bw;
		}

		if ((ret = icc_set_bw(prime->icc_cpu, 0, 0)) != 0) {
			dev_dbg(dev, "failed to set cpu bandwidth request: %d\n", ret);
			goto r_bw;
		}
	}

	return 0;

r_bw:

	return ret;
}

static int prime_start_clock(struct qcom_prime* prime, enum prime_clk_level clk_level)
{
	struct device *dev = prime->dev;
	int ret;
	void __iomem *gcc_base;
	u32 val;

	struct prime_bw_table_val mem_cfg_bw = prime_bw_table[clk_level];

	dev_dbg(dev, "Setting BW\n");
	if ((ret = prime_set_clk_bw(prime, clk_level, mem_cfg_bw.mem_bw, mem_cfg_bw.cfg_bw)) != 0) {
		dev_err(dev, "prime_set_clk_bw failed. ret: %d\n", ret);
		goto r_clk;
	}

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
	mdelay(1);

	/* Configure GCC_CNOC_PRIMESS_AHBS_CBCR */
	val = readl(gcc_base + GCC_CNOC_PRIMESS_AHBS_CBCR);
	val |= 0x1;
	writel(val, gcc_base + GCC_CNOC_PRIMESS_AHBS_CBCR);
	mdelay(1);

	iounmap(gcc_base);

	dev_dbg(dev, "Enable Voter\n");
	//TODO:  Any delay needed between clk calls and starting the subsystem?
	// Enable bit 1 in the voter
	writel(PRIME_KM_VOTE_BIT, prime->reg_ss_base + PRIMESS_VOTER_FW_EN_CFG);
	// readback to allow time for cfg to flush.
	readl(prime->reg_ss_base + PRIMESS_VOTER_FW_EN_CFG);
	dev_dbg(dev, "Powerup PRIME SS\n");
	// Vote to start power up sequence
	writel(PRIME_KM_VOTE_BIT, prime->reg_ss_base + PRIMESS_VOTER_FW_VOTE_SET);


	if ((ret = readl_poll_timeout_atomic(prime->reg_ss_base + PRIMESS_VOTER_FW_VOTE_DONE, val, val & PRIME_KM_VOTE_BIT, 1, 100)) != 0) {
		dev_err(dev, "Voter Powerup Timed out!\n");
		goto r_bw;
	}
	else {
		dev_dbg(dev, "Voter Done Set.\n");
	}

	return 0;

r_bw:
	prime_set_clk_bw(prime, PRIME_CLK_LEVEL_LSVS, 0, 0);
r_clk:
	clk_disable_unprepare(prime->core_clk);

	return ret;
}

static int prime_cleanup_clock_power(struct qcom_prime* prime)
{
	writel(0, prime->reg_ss_base + PRIMESS_VOTER_FW_EN_CFG);
	writel(PRIME_KM_VOTE_BIT, prime->reg_ss_base + PRIMESS_VOTER_FW_VOTE_CLEAR);
	readl(prime->reg_ss_base + PRIMESS_VOTER_FW_EN_CFG);

	// Do we need to guarantee SS is powered down before stopping clocks?  How?
	clk_disable_unprepare(prime->core_clk);
	prime->clk_enabled = false;

	if (prime->icc_ddr) {
		icc_set_bw(prime->icc_ddr, 0, 0);
	}
	if (prime->icc_cpu) {
		icc_set_bw(prime->icc_cpu, 0, 0);
	}

	/* Reset state */
	prime->clk_level = PRIME_CLK_LEVEL_OFF;
	prime->clk_ddr_bw = 0;
	prime->clk_cpu_bw = 0;

	return 0;
}
#endif /* BUILDROOT */

#ifndef BUILDROOT
static int prime_load(struct rproc *rproc, const struct firmware *fw)
{
	int ret;
	struct qcom_prime *prime = (struct qcom_prime *)rproc->priv;

	if(	prime->mem_region == NULL) {
		prime->mem_region = devm_ioremap_wc(prime->dev, prime->mem_phys, prime->mem_size);
		if (!prime->mem_region) {
			dev_err(prime->dev, "unable to map memory region: %pa+%zx\n", &prime->mem_phys, prime->mem_size);
			return -EBUSY;
		}
	}

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

		ret = qcom_mdt_load_no_init(prime->dev, fw, rproc->firmware, prime->pas_id,
			prime->mem_region, prime->mem_phys, prime->mem_size, &prime->mem_reloc);
	} else {
		dev_dbg(prime->dev, "Loading firmware via standard PIL/PAS (qcom_mdt_load)\n");
		ret = qcom_mdt_load(prime->dev, fw, rproc->firmware, prime->pas_id,
			prime->mem_region, prime->mem_phys, prime->mem_size, &prime->mem_reloc);
	}

	devm_iounmap(prime->dev, prime->mem_region);
	prime->mem_region = NULL;
	dev_dbg(prime->dev, "Umapped PIL memory region\n");

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
			"Cannot stop PRIME: %d active SWIF mapping(s) exist. "
			"Userspace must munmap() all SWIF mappings before stopping.\n",
			mmap_count);
		return -EBUSY;
	}

	/* Mark PRIME as stopped - prevent new SWIF mappings */
	prime->prime_started = false;

	/* Decrement module reference to allow rmmod */
	module_put(THIS_MODULE);

#if 0
	if((ret = fw_iu_rproc_disable_components(&prime->iuss_dev)) < 0) {
		dev_dbg(prime->dev, "Error Disabling Components\n");
		goto done;
	}
#endif

#ifdef BUILDROOT
	if((ret = qcom_scm_pas_shutdown(rproc)) < 0) {
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
	if((ret = prime_cleanup_clock_power(prime)) < 0) {
		dev_dbg(prime->dev, "Error Cleaning Clocks");
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
   prime_rproc_da_to_va() - ELF "Device Addr" -> "Virtual Addr" Conversion.

   The RPROC framework requires us to remap the Phys Addr in the ELF Segments to
   the virtual address on to which they were remapped by the device driver probe
   function. Since, PRIME ELFs are addressed zero-relative to PDMEM start, we
   simply add the PDMEM's HWIO offset to the overall virtual address base to get
   the absolute virtual address of PDMEM corresponding to the provided device
   address.
 */
static void *prime_rproc_da_to_va(struct rproc *rproc, u64 da, /**< Device Address (in ELF) */
				  size_t len, /**< Length of the mapping required. */
				  bool *is_iomem /**< Set to true if this is a IO memory */
)
{
	struct qcom_prime *prime = rproc->priv;
	*is_iomem = false;
	int offset;

	//If less than 1MB, assume addresses are relative
	#define PRIME_ELF_ABS_BASE (0x8C180000)
	offset = (da < 0x100000) ? da : da - PRIME_ELF_ABS_BASE;

	//Alow overflow of PDMEM into MLM region if using prime->mem_size (Carvout size vs pdmem_size)
	if (offset < 0 || offset + len > prime->pdmem_size) {
		dev_err(prime->dev, "da[0x%llX] + len[0x%zX] > 0x%zX\n", da, len, prime->pdmem_size);
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
#else
	if((ret = prime_start_clock(prime, PRIME_CLK_LEVEL_LSVS)) < 0) {
		dev_dbg(prime->dev, "Error setting clocks");
		return ret;
	}

	//Initial boot clear pending
	unmask_irqs(prime, 0xff);

	/* Use tmelcom path for IPQ platforms, standard PIL/PAS path for others */
	if (prime->tmelcom_support) {
		dev_dbg(prime->dev, "Starting firmware via tmelcom (tmelcom_secboot_sec_auth)\n");
		ret = tmelcom_secboot_sec_auth_v2(prime->pas_id, prime->metadata, prime->metadata_len);
	} else {
		dev_dbg(prime->dev, "Starting firmware via standard PIL/PAS (qcom_scm_pas_auth_and_reset)\n");
		ret = qcom_scm_pas_auth_and_reset(prime->pas_id);
	}
#endif
	if (ret) {
		dev_err(prime->dev, "Auth and reset failed for remoteproc %s: %d\n", rproc->name, ret);
		if (prime->tmelcom_support)
			(void)tmelcom_secboot_teardown(prime->pas_id, 0);

		return ret;
	}

	// This isn't a guaranteed sync point as there will potentially be 3 IRQs from PRIME... Treated more as a proof of life since auth_and_reset will be blocking until booted.
	if (!wait_for_completion_timeout(&prime->start_done, msecs_to_jiffies(10000))) {
		dev_err(prime->dev, "Boot completion timeout after 5 seconds\n");
		return -ETIMEDOUT;
	}

#if 0
	// App transition in TZ PIL since IRQ isn't exposed to ARM.
	//  Add a springboard in IU for future control by HLOS?
	ret = fw_iu_rproc_enable_components(&prime->iuss_dev);
	if (ret) {
		dev_err(prime->dev, "Error Eanbling components\n");
		return ret;
	}
#endif

#if 0
     ret = wait_for_completion_timeout(&prime->start_done, msecs_to_jiffies(prime_TIMEOUT));
     read_sp2cl_debug_registers(prime);
     if (rproc->recovery_disabled && !ret) {
         panic("Panicking, %s start timed out\n", rproc->name);
     }
     else if (!ret) {
         dev_err(prime->dev, "start timed out\n");
     }
     ret = ret ? 0 : -ETIMEDOUT;
#endif

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

static int prime_mem_open(struct inode *inode, struct file *file)
{
	struct qcom_prime *prime;
	prime = container_of(inode->i_cdev, struct qcom_prime, cdev);
	file->private_data = prime; // Set private data
	return 0;
}

// static int prime_mem_release(struct inode *inode, struct file *file) {
//     return 0;
// }

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
		dev_dbg(prime->dev, "MMAP Model Mem 0x%08X", (u32)(base_addr + offset));
		pfn = PFN_DOWN(base_addr + offset);
		vsize = prime->mem_model_size - offset;

		/* base address and total buffer size must be page aligned */
		if (!PAGE_ALIGNED(base_addr) || !PAGE_ALIGNED(size)) {
			return -ENODEV;
		}
		if (size > vsize) {
			return -EINVAL;
		}

		ret = io_remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot);
		break;
	case 1: /* IUSS Interface exposed to kernel*/
		/* Check if PRIME has been started */
		if (!prime->prime_started) {
			dev_err(prime->dev,
				"Cannot map SWIF memory: PRIME not started. "
				"Start PRIME subsystem first.\n");
			return -EAGAIN;
		}

		// We can't map just the swif since mappings must be page boundary aligned.
		base_addr = prime->intf_table_phys;
		dev_dbg(prime->dev, "MMAP SWIF Mem 0x%08X (prime_started=%d, active_maps=%d)",
			 (u32)base_addr, prime->prime_started,
			 atomic_read(&prime->swif_mmap_count));
		pfn = PFN_DOWN(base_addr);
		vsize = (phys_addr_t)prime->pdmem_base + prime->pdmem_size - base_addr;

		/* base address and total buffer size must be page aligned */
		if (!PAGE_ALIGNED(base_addr) || !PAGE_ALIGNED(size)) {
			return -ENODEV;
		}

		if (size > vsize) {
			return -EINVAL;
		}

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
		dev_dbg(prime->dev, "MMAP Tensor Mem");
		// unsigned long pfn = virt_to_phys(kmalloc_ptr) >> PAGE_SHIFT;
		// ret = remap_pfn_range(vma, vma->vm_start, pfn, vma->vm_end - vma->vm_start, vma->vm_page_prot);
		return -EINVAL;
		break;
	}

	return ret;
}

static long prime_mem_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct qcom_prime *prime = file->private_data;

	switch (cmd) {
	case PRIME_MEM_GET_REGION: {
		struct prime_mem_region mem_data;
		if (copy_from_user(&mem_data, (void __user *)arg, sizeof(struct prime_mem_region)) != 0) {
			return -EINVAL;
		}
		dev_dbg(prime->dev, "PRIME PRIME_MEM_GET_REGION %d\n", mem_data.region);
		switch (mem_data.region) {
		case PRIME_MEM_REGION_MODEL:
			// For all TZ-PIL mapped regions, we have VA = PA, so access should be OK once PIL is invoked
			mem_data.base = prime->mem_model_base;
			mem_data.sz = prime->mem_model_size;
			break;
		case PRIME_MEM_REGION_TENSOR:
			return -EINVAL;
		case PRIME_MEM_REGION_SWIF:
			// For all TZ-PIL mapped regions, we have VA = PA, so access should be OK once PIL is invoked
			mem_data.base = prime->intf_table_phys;
			mem_data.sz = (phys_addr_t)prime->pdmem_base_phys + prime->pdmem_size - prime->intf_table_phys;
			break;
		case PRIME_MEM_REGION_PDMEM:
			// For all TZ-PIL mapped regions, we have VA = PA, so access should be OK once PIL is invoked
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

		if (copy_to_user((void __user *)arg, &mem_data, sizeof(struct prime_mem_region)) != 0) {
			return -EINVAL;
		}
		return 0;
	}
	case PRIME_MEM_SIG_REGISTER: {
		struct prime_sig_reg sig_reg;

		/* Only allow signal registration on SWIF FD (minor 1) */
		if (MINOR(file->f_inode->i_rdev) != 1) {
			dev_err(prime->dev, "Signal registration only allowed on SWIF FD (minor 1)\n");
			return -EINVAL;
		}

		if (copy_from_user(&sig_reg, (void __user *)arg, sizeof(struct prime_sig_reg)) != 0) {
			return -EINVAL;
		}
		int irq = sig_reg.irq;
		if (irq > PRIME_IRQ2L_MAX) {
			dev_dbg(prime->dev, "Bad IRQ\n");
			return -EINVAL;
		}
		if (prime->sig_task[irq]) {
			dev_dbg(prime->dev, "SIG_TASK already registered PID[%d]\n", prime->sig_task[irq]->pid);
			return -EINVAL;
		}
		prime->sig_task[irq] = get_current();
		get_task_struct(prime->sig_task[irq]);
		prime->signo[irq] = sig_reg.signo;
		dev_dbg(prime->dev, "PRIME SIG %d: Userspace PID %d registered\n", prime->signo[irq], prime->sig_task[irq]->pid);
		return 0;
	}
	case PRIME_MEM_SIG_DEREGISTER:  {
		struct prime_sig_reg sig_reg;

		/* Only allow signal deregistration on SWIF FD (minor 1) */
		if (MINOR(file->f_inode->i_rdev) != 1) {
			dev_err(prime->dev, "Signal deregistration only allowed on SWIF FD (minor 1)\n");
			return -EINVAL;
		}

		if (copy_from_user(&sig_reg, (void __user *)arg, sizeof(struct prime_sig_reg)) != 0) {
			return -EINVAL;
		}
		int irq = sig_reg.irq;
		if (irq > PRIME_IRQ2L_MAX) {
			dev_dbg(prime->dev, "Bad IRQ\n");
			return -EINVAL;
		}
		struct task_struct *ref_task = get_current();
		if(prime->sig_task[irq] != ref_task) {
			dev_dbg(prime->dev, "SIG_DEREGISTER from non-owning process\n");
			return -EINVAL;
		}
		if(prime->signo[irq] != sig_reg.signo){
			dev_dbg(prime->dev, "SIG_DEREGISTER with invalid SIGNO\n");
			return -EINVAL;
		}
		put_task_struct(prime->sig_task[irq]);
		prime->sig_task[irq] = NULL;
		prime->signo[irq] = 0;
		dev_dbg(prime->dev, "PRIME SIG %d: Userspace PID %d de-registered\n", sig_reg.signo, ref_task->pid);
		return 0;
	}
	case PRIME_MEM_PIL: {
		struct prime_mem_region mem_data;
		if (copy_from_user(&mem_data, (void __user *)arg, sizeof(struct prime_mem_region)) != 0) {
			return -EINVAL;
		}

		switch (mem_data.region) {
		case PRIME_MEM_REGION_MODEL:
		case PRIME_MEM_REGION_TENSOR:

		case PRIME_MEM_REGION_SWIF:
		default:
			return -EINVAL;
		}
		return -EINVAL;
	}
	case PRIME_MEM_DMA: {
		struct prime_mem_dma_map mem_data;
		if (copy_from_user(&mem_data, (void __user *)arg, sizeof(struct prime_mem_dma_map)) != 0) {
			return -EINVAL;
		}
		dev_dbg(prime->dev, "PRIME PRIME_MEM_DMA %d\n", mem_data.region);
		BUILD_BUG_ON((int)PRIME_DMA_BIDIRECTIONAL != (int)DMA_BIDIRECTIONAL);
		BUILD_BUG_ON((int)PRIME_DMA_TO_DEVICE != (int)DMA_TO_DEVICE);
		BUILD_BUG_ON((int)PRIME_DMA_FROM_DEVICE != (int)DMA_FROM_DEVICE);
		switch (mem_data.region) {
		case PRIME_MEM_REGION_TENSOR:
			switch (mem_data.action) {
			case PRIME_MEM_DMA_ALLOC:
				{
					if (IS_ERR(mem_data.dma_buf = dma_buf_get(mem_data.dma_fd))) {
						dev_dbg(prime->dev, "Failed dma_buf_get\n");
						return -EINVAL;
					}
					if (IS_ERR(mem_data.dma_attachment = dma_buf_attach(mem_data.dma_buf, prime->dev))) {
						dma_buf_put(mem_data.dma_buf);
						dev_dbg(prime->dev, "Failed dma_buf_attach\n");
						return -EINVAL;
					}
					if (IS_ERR(mem_data.sg_table = dma_buf_map_attachment(
							mem_data.dma_attachment, (enum dma_data_direction)mem_data.direction))) {
						dma_buf_detach(mem_data.dma_buf, mem_data.dma_attachment);
						dma_buf_put(mem_data.dma_buf);
						dev_dbg(prime->dev, "Failed dma_buf_map_attachment\n");
						return -EINVAL;
					}
					if (mem_data.sg_table->nents != 1) {
						dma_buf_unmap_attachment(mem_data.dma_attachment, mem_data.sg_table,
									(enum dma_data_direction)mem_data.direction);
						dma_buf_detach(mem_data.dma_buf, mem_data.dma_attachment);
						dma_buf_put(mem_data.dma_buf);
						dev_dbg(prime->dev, "dma_buf_map_attachment nents %d > 1 \n", mem_data.sg_table->nents);
						return -EINVAL;
					}
					int sg_idx;
					struct scatterlist *sg;
					for_each_sgtable_dma_sg (mem_data.sg_table, sg, sg_idx) {
						dev_dbg(prime->dev, "Mapped DMA SG[%d]: 0x%08llx (%d bytes)\n", sg_idx,
							(uint64_t)sg_dma_address(sg), sg_dma_len(sg));
						mem_data.device_dma_base_addr = (uint64_t)sg_dma_address(sg);
#if IS_ENABLED(CONFIG_QCOM_SECURE_BUFFER)
						if(prime->config_mmu)
						{
							#define QCOM_SCM_VMID_PRIME (93)
							struct qcom_scm_mem_map_info *mem_region;
							struct qcom_scm_current_perm_info *dest_perms;
							u32 *src_vmids;
							void *buf;
							size_t buf_size;
							int ret;

							/* Allocate single buffer for all structures */
							buf_size = sizeof(*mem_region) + sizeof(u32) + 2 * sizeof(*dest_perms);
							buf = kmalloc(buf_size, GFP_KERNEL);
							if (!buf) {
								ret = -ENOMEM;
								dev_err(prime->dev, "Failed to allocate SCM buffers\n");
								dma_buf_unmap_attachment(mem_data.dma_attachment, mem_data.sg_table,
									(enum dma_data_direction)mem_data.direction);
								dma_buf_detach(mem_data.dma_buf, mem_data.dma_attachment);
								dma_buf_put(mem_data.dma_buf);
								return ret;
							}

							/* Set up pointers within the buffer */
							mem_region = (struct qcom_scm_mem_map_info *)buf;
							src_vmids = (u32 *)(mem_region + 1);
							dest_perms = (struct qcom_scm_current_perm_info *)(src_vmids + 1);

							/* Populate memory region */
							qcom_scm_populate_mem_map_info(mem_region,
								(uint64_t)sg_dma_address(sg), sg_dma_len(sg));

							/* Populate source VMIDs */
							src_vmids[0] = QCOM_SCM_VMID_HLOS;

							/* Populate destination permissions */
							qcom_scm_populate_vmperm_info(&dest_perms[0],
								QCOM_SCM_VMID_HLOS, QCOM_SCM_PERM_READ);
							qcom_scm_populate_vmperm_info(&dest_perms[1],
								QCOM_SCM_VMID_PRIME, QCOM_SCM_PERM_RW);

							dev_dbg(prime->dev, "qcom_scm_assign_mem_regions map");
							ret = qcom_scm_assign_mem_regions(mem_region, sizeof(*mem_region),
													src_vmids, sizeof(u32),
													dest_perms, 2 * sizeof(*dest_perms));

							kfree(buf);

							if (ret) {
								dev_err(prime->dev, "SCM Assign mem failed: %d\n", ret);
								dma_buf_unmap_attachment(mem_data.dma_attachment, mem_data.sg_table,
									(enum dma_data_direction)mem_data.direction);
								dma_buf_detach(mem_data.dma_buf, mem_data.dma_attachment);
								dma_buf_put(mem_data.dma_buf);
								return ret;
							}
						}
#endif
					}
				}
				break;
			case PRIME_MEM_DMA_FREE:
				{
#if IS_ENABLED(CONFIG_QCOM_SECURE_BUFFER)
					if(prime->config_mmu)
					{
						int sg_idx;
						int ret;
						struct scatterlist *sg;
						for_each_sgtable_dma_sg (mem_data.sg_table, sg, sg_idx) {
							dev_dbg(prime->dev, "Unmapping DMA SG[%d]: 0x%08llx (%d bytes)\n", sg_idx,
								(uint64_t)sg_dma_address(sg), sg_dma_len(sg));
							struct qcom_scm_mem_map_info *mem_region;
							struct qcom_scm_current_perm_info *dest_perms;
							u32 *src_vmids;
							void *buf;
							size_t buf_size;

							/* Allocate single buffer for all structures */
							buf_size = sizeof(*mem_region) + 2 * sizeof(u32) + sizeof(*dest_perms);
							buf = kmalloc(buf_size, GFP_KERNEL);
							if (!buf) {
								ret = -ENOMEM;
								dev_err(prime->dev, "Failed to allocate SCM buffers for unmap\n");
								continue;
							}

							/* Set up pointers within the buffer */
							mem_region = (struct qcom_scm_mem_map_info *)buf;
							src_vmids = (u32 *)(mem_region + 1);
							dest_perms = (struct qcom_scm_current_perm_info *)(src_vmids + 2);

							/* Populate memory region */
							qcom_scm_populate_mem_map_info(mem_region,
								(uint64_t)sg_dma_address(sg), sg_dma_len(sg));

							/* Populate source VMIDs */
							src_vmids[0] = QCOM_SCM_VMID_HLOS;
							src_vmids[1] = QCOM_SCM_VMID_PRIME;

							/* Populate destination permissions - return to HLOS only */
							qcom_scm_populate_vmperm_info(dest_perms,
								QCOM_SCM_VMID_HLOS, (QCOM_SCM_PERM_READ | QCOM_SCM_PERM_WRITE | QCOM_SCM_PERM_EXEC));

							dev_dbg(prime->dev, "qcom_scm_assign_mem_regions unmap");
							ret = qcom_scm_assign_mem_regions(mem_region, sizeof(*mem_region),
													src_vmids, 2 * sizeof(u32),
													dest_perms, sizeof(*dest_perms));

							kfree(buf);

							if (ret) {
								dev_err(prime->dev, "SCM Assign mem unmap failed: %d\n", ret);
							}
						}
					}
#endif
					dma_buf_unmap_attachment(mem_data.dma_attachment, mem_data.sg_table,
								(enum dma_data_direction)mem_data.direction);
					dma_buf_detach(mem_data.dma_buf, mem_data.dma_attachment);
					dma_buf_put(mem_data.dma_buf);
				}
				break;
			default:
				return -EINVAL;
			}
			break;
		default:
			return -EINVAL;
		}

		if (copy_to_user((void __user *)arg, &mem_data, sizeof(struct prime_mem_dma_map)) != 0) {
			return -EINVAL;
		}
		return 0;
	}
	case PRIME_MEM_SET_CLK_BW: {
#ifndef BUILDROOT
		struct prime_clk_bw_config clk_bw_cfg;
		if (copy_from_user(&clk_bw_cfg, (void __user *)arg, sizeof(struct prime_clk_bw_config)) != 0) {
			return -EINVAL;
		}
		dev_dbg(prime->dev, "PRIME_MEM_SET_CLK_BW: clk_level=%d, ddr_bw=%u, cpu_bw=%u\n",
			clk_bw_cfg.clk_level, clk_bw_cfg.ddr_bw, clk_bw_cfg.cpu_bw);
		return prime_set_clk_bw(prime, clk_bw_cfg.clk_level, clk_bw_cfg.ddr_bw, clk_bw_cfg.cpu_bw);
#else
		dev_dbg(prime->dev, "PRIME_MEM_SET_CLK_BW not supported in BUILDROOT\n");
		return -ENOTSUPP;
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
	struct task_struct *ref_task = get_current();
	dev_dbg(prime->dev, "Release Cur PID[%d]\n", ref_task->pid);

	/* Only check signals for SWIF FD (minor 1) */
	if (MINOR(inode->i_rdev) == 1) {
		/* If the process is exiting, allow cleanup of signals */
		if (ref_task->flags & PF_EXITING) {
			for (int i = 0; i <= PRIME_IRQ2L_MAX; ++i) {
				if (ref_task == prime->sig_task[i]) {
					dev_warn(prime->dev, "Process PID %d exiting while SIGNO %d registered, cleaning up\n",
						prime->sig_task[i]->pid, prime->signo[i]);
					put_task_struct(prime->sig_task[i]);
					prime->sig_task[i] = NULL;
					prime->signo[i] = 0;
				}
			}
		} else {
			/* Process not exiting - return error if any signals still registered */
			for (int i = 0; i <= PRIME_IRQ2L_MAX; ++i) {
				if (ref_task == prime->sig_task[i]) {
					dev_err(prime->dev, "Cannot close SWIF FD with signal %d still registered by PID %d\n",
						prime->signo[i], prime->sig_task[i]->pid);
					return -EBUSY;
				}
			}
		}
	}

	return 0;
}

/** Used for construcing a memory allocation/mapping device */
static const struct file_operations prime_mem_fops = { .owner = THIS_MODULE,
						       .open = prime_mem_open,
						       .mmap = prime_mem_mmap,
						       .unlocked_ioctl = prime_mem_ioctl,
						       .release = prime_mem_release };

/** Remoteproc Callback Implementation */
static const struct rproc_ops prime_rproc_ops = {
	.start = prime_start,
	.stop = prime_stop,
	.da_to_va = prime_rproc_da_to_va,
#ifndef BUILDROOT
	.load = prime_load, //TODO: This is for loading a .mdt file.... depends on drivers/soc/qcom/mdt_loader.c
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
	if (ret) {
		return ret;
	}

	u32 model_offset = (64 + 256) * 1024;
	prime->mem_phys = prime->mem_reloc = r.start;
	prime->mem_model_base = prime->mem_phys + model_offset;
	prime->mem_size = resource_size(&r);
	prime->mem_model_size = prime->mem_size - model_offset;
	prime->mem_region = devm_ioremap_wc(prime->dev, prime->mem_phys, prime->mem_size);
	if (!prime->mem_region) {
		dev_err(prime->dev, "unable to map memory region: %pa+%zx\n", &r.start, prime->mem_size);
		return -EBUSY;
	}

	return 0;
}
static void prime_cleanup_memory_region(struct qcom_prime *prime)
{
	if(prime->mem_region) {
		devm_iounmap(prime->dev, prime->mem_region);
		prime->mem_region = NULL;
	}
}

static int prime_create_cdev(struct qcom_prime *prime)
{
	int ret = 0;

	prime->mem_count = 0;
	int num_devs = 3;
	if ((ret = alloc_chrdev_region(&prime->cdev_dev, 0, num_devs, "prime")) < 0) {
		dev_dbg(prime->dev, "Cannot allocate major number\n");
		return ret;
	}

	dev_t dev = prime->cdev_dev;
	dev_dbg(prime->dev, "Major = %d Minor = %d \n", MAJOR(dev), MINOR(dev));

	/*Creating cdev structure*/
	cdev_init(&prime->cdev, &prime_mem_fops);

	/*Adding character device to the system*/
	if ((ret = cdev_add(&prime->cdev, dev, num_devs)) < 0) {
		dev_dbg(prime->dev, "Cannot add the device to the system\n");
		goto r_class;
	}

	/*Creating struct class*/
	if (IS_ERR(prime->cdev_class = class_create("prime"))) {
		dev_dbg(prime->dev, "Failed to create struct Class\n");
		goto r_cdev;
	}

	/*Creating device*/
	for (int i = 0; i < num_devs; i++) {
		if (IS_ERR(device_create(prime->cdev_class, NULL, MKDEV(MAJOR(dev), MINOR(dev) + i), NULL, "prime%d", i))) {
			dev_dbg(prime->dev, "Failed to create device node %d\n", i);
			while (i--) {
				device_destroy(prime->cdev_class, MKDEV(MAJOR(dev), MINOR(dev) + i));
			}
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
	int num_devs = 3;

	for (int i = 0; i < num_devs; i++) {
		device_destroy(prime->cdev_class, MKDEV(MAJOR(dev), MINOR(dev) + i));
	}
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
		dev_dbg(dev, "Get resource primess");
		return PTR_ERR(prime->reg_ss_base);
	}

	if(NULL == (res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "pdmem"))) {
		dev_dbg(dev, "Get resource pdmem");
		ret = -ENODEV;
		goto r_unmap;
	}
	prime->pdmem_base = FW_PRIMESS_REG_ADDR(prime->reg_ss_base, res->start);
	prime->pdmem_base_phys = (phys_addr_t)FW_PRIMESS_REG_ADDR(pss->start, res->start);
	prime->pdmem_size = resource_size(res);


	if(NULL == (res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mlmem"))) {
		dev_dbg(dev, "Get resource mlmem");
		ret = -ENODEV;
		goto r_unmap;
	}
	prime->mem_mlm_base_phys = (phys_addr_t)FW_PRIMESS_REG_ADDR(pss->start, res->start);
	prime->mem_mlm_size = resource_size(res);


#define IU_CORE_INTF_BASE_XPU (prime->pdmem_size - (16 * 1024))
	prime->intf_table = FW_PRIMESS_REG_ADDR(prime->pdmem_base, IU_CORE_INTF_BASE_XPU);
	prime->intf_table_phys = (phys_addr_t)FW_PRIMESS_REG_ADDR(prime->pdmem_base_phys, IU_CORE_INTF_BASE_XPU);

#ifdef BUILDROOT
	if(NULL == (res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "iuss-cmn"))) {
		dev_dbg(dev, "Get resource iuss-cmn");
		ret = -ENODEV;
		goto r_unmap;
	}
	prime->iuss_dev.reg_cmn_base = FW_PRIMESS_REG_ADDR(prime->reg_ss_base, res->start);
	prime->iuss_dev.reg_ss_base = prime->reg_ss_base;
	prime->iuss_dev.dev = prime->dev;


	if(NULL == (res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "iu0-csr"))) {
		dev_dbg(dev, "Get resource iuss-csr");
		ret = -ENODEV;
		goto r_unmap;
	}
	prime->iuss_dev.reg_core_csr[0] = FW_PRIMESS_REG_ADDR(prime->reg_ss_base, res->start);
	prime->iuss_dev.pdmem_base = prime->pdmem_base;
	prime->iuss_dev.intf_table = prime->intf_table;
#endif /* BUILDROOT */


	dev_dbg(dev, "Remapped PRIME memory to 0x%px from 0x%pax", &prime->reg_ss_base, &pss->start);

	/* get the interrupt exposed by PRIME */
	int irq = platform_get_irq_byname(pdev, "boot-done");
	if (irq < 0) {
		dev_dbg(dev, "Unable to find IRQ to map (%d).", irq);
		ret = -ENODEV;
		goto r_unmap;
	}
	prime->prime_boot_done_irq = irq;

	/* setup interrupt handling */
	ret = devm_request_threaded_irq(dev, prime->prime_boot_done_irq, NULL, prime_irq_boot_done, IRQF_TRIGGER_RISING | IRQF_ONESHOT, "prime boot-done", prime);
	if (ret) {
		dev_dbg(dev, "Unable to register IRQ %d (error: %d)", irq, ret);
		goto r_unmap;
	}

	/* get the interrupt exposed by PRIME */
	irq = platform_get_irq_byname(pdev, "task-done");
	if (irq < 0) {
		dev_dbg(dev, "Unable to find IRQ to map (%d).", irq);
		ret = -ENODEV;
		goto r_boot_irq;
	}
	prime->prime_task_done_irq = irq;

	/* setup interrupt handling */
	ret = devm_request_threaded_irq(dev, prime->prime_task_done_irq, NULL, prime_irq_task_done, IRQF_TRIGGER_RISING | IRQF_ONESHOT, "prime task-done", prime);
	if (ret) {
		dev_dbg(dev, "Unable to register IRQ %d (error: %d)", irq, ret);
		goto r_boot_irq;
	}

	/* get the error interrupt exposed by PRIME */
	irq = platform_get_irq_byname(pdev, "error");
	if (irq < 0) {
		dev_dbg(dev, "Unable to find error IRQ to map (%d).", irq);
		ret = -ENODEV;
		goto r_task_irq;
	}
	prime->prime_error_irq = irq;

	/* setup error interrupt handling */
	ret = devm_request_threaded_irq(dev, prime->prime_error_irq, NULL, prime_irq_error, IRQF_TRIGGER_RISING | IRQF_ONESHOT, "prime error", prime);
	if (ret) {
		dev_dbg(dev, "Unable to register error IRQ %d (error: %d)", irq, ret);
		goto r_task_irq;
	}

	/* get the wdog interrupt exposed by PRIME */
	irq = platform_get_irq_byname(pdev, "wdog");
	if (irq < 0) {
		dev_dbg(dev, "Unable to find wdog IRQ to map (%d).", irq);
		ret = -ENODEV;
		goto r_error_irq;
	}
	prime->prime_wdog_irq = irq;

	/* setup wdog interrupt handling */
	ret = devm_request_threaded_irq(dev, prime->prime_wdog_irq, NULL, prime_irq_wdog, IRQF_TRIGGER_RISING | IRQF_ONESHOT, "prime wdog", prime);
	if (ret) {
		dev_dbg(dev, "Unable to register wdog IRQ %d (error: %d)", irq, ret);
		goto r_error_irq;
	}

	/* get the misc interrupt exposed by PRIME */
	irq = platform_get_irq_byname(pdev, "misc");
	if (irq < 0) {
		dev_dbg(dev, "Unable to find misc IRQ to map (%d).", irq);
		ret = -ENODEV;
		goto r_wdog_irq;
	}
	prime->prime_misc_irq = irq;

	/* setup misc interrupt handling */
	ret = devm_request_threaded_irq(dev, prime->prime_misc_irq, NULL, prime_irq_misc, IRQF_TRIGGER_RISING | IRQF_ONESHOT, "prime misc", prime);
	if (ret) {
		dev_dbg(dev, "Unable to register misc IRQ %d (error: %d)", irq, ret);
		goto r_wdog_irq;
	}

	return 0;

r_wdog_irq:
	devm_free_irq(dev, prime->prime_wdog_irq, prime);
r_error_irq:
	devm_free_irq(dev, prime->prime_error_irq, prime);
r_task_irq:
	devm_free_irq(dev, prime->prime_task_done_irq, prime);
r_boot_irq:
	devm_free_irq(dev, prime->prime_boot_done_irq, prime);
r_unmap:
	devm_iounmap(dev, prime->reg_ss_base);
	return ret;
}

static void prime_cleanup_mmio(struct qcom_prime *prime)
{
	struct device *dev = prime->dev;

	devm_free_irq(dev, prime->prime_misc_irq, prime);
	devm_free_irq(dev, prime->prime_wdog_irq, prime);
	devm_free_irq(dev, prime->prime_error_irq, prime);
	devm_free_irq(dev, prime->prime_task_done_irq, prime);
	devm_free_irq(dev, prime->prime_boot_done_irq, prime);
	devm_iounmap(dev, prime->reg_ss_base);
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
	int i;
	const struct firmware *fw;
	static const char * const prime_fw_names[] = {
		"qcom_prime_a.elf"
	};

	desc = of_device_get_match_data(&pdev->dev);
	if (!desc) {
		return -EINVAL;
	}

#if 0 //TODO: qcom rproc driver?
     if (!qcom_scm_is_available()) {
         return -EPROBE_DEFER;
     }
#endif

	fw_name = desc->firmware_name;

	if (fw_name)
		goto skip_fw_search;

	ret = of_property_read_string(pdev->dev.of_node, "firmware-name", &fw_name);
	if (ret < 0 && ret != -EINVAL && ret != -ENODATA) {
		dev_err(dev, "Error reading firmware-name: %d\n", ret);
		return ret;
	}

	/* Try DT or default name first */
	if (request_firmware(&fw, fw_name, dev) == 0) {
		release_firmware(fw);
		dev_info(dev, "Found firmware: %s\n", fw_name);
	}
	else {
		for (i = 0; i < ARRAY_SIZE(prime_fw_names); i++) {
			if (request_firmware(&fw, prime_fw_names[i], dev) == 0) {
				release_firmware(fw);
				fw_name = prime_fw_names[i];
				dev_info(dev, "Found firmware via search: %s\n", fw_name);
				break;
			}
		}
	}

skip_fw_search:
	/* create an RPROC device */
	rproc = rproc_alloc(&pdev->dev, pdev->name, &prime_rproc_ops, fw_name, sizeof(*prime));

	if (!rproc) {
		dev_dbg(dev, "Unable to allocate a remote processor");
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

	prime->primess_rif_last_val = 0;

	for (int i = 0; i <= PRIME_IRQ2L_MAX; ++i) {
		prime->sig_task[i] = NULL;
	}

	/* Initialize clock state tracking */
	prime->clk_enabled = false;
	prime->clk_level = PRIME_CLK_LEVEL_OFF;
	prime->clk_ddr_bw = 0;
	prime->clk_cpu_bw = 0;

	//prime->qmp_name = desc->qmp_name;

	rproc->auto_boot = desc->auto_boot;

	rproc->recovery_disabled = true;
	rproc_coredump_set_elf_info(rproc, ELFCLASS32, EM_NONE);

	if ((ret = device_init_wakeup(prime->dev, true)) < 0) {
		goto r_rproc;
	}

	/* *IMPORTANT* Right now, we don't include a device resource table inside
         PRIME FW. This will cause the device boot to fail during the ELF
         parsing phase. We can override this behaviour by setting parse_fw =
         NULL. This is just a temporary measure, should be removed when the
         firmware is updated with the resource table */
	rproc->ops->parse_fw = NULL;

	/* initialize driver data */
	platform_set_drvdata(pdev, prime);

#ifndef BUILDROOT
	if ((ret = prime_init_clock_power(prime)) < 0) {
		goto r_wakeup;
	}
#endif

	if ((ret = prime_init_mmio(pdev, prime)) < 0) {
		goto r_wakeup;
	}

	if ((ret = prime_create_cdev(prime)) < 0) {
		goto r_mmio;
	}

	if ((ret = prime_alloc_memory_region(prime)) < 0) {
		goto r_cdev;
	}

	/* create device attributes in sysfs */
	if ((ret = sysfs_create_group(&dev->kobj, &prime_attr_group)) < 0) {
		dev_dbg(dev, "Unable to create sysfs group (error: %d)", ret);
		goto r_mem_region;
	}

	/* register this RPROC device */
	if ((ret = rproc_add(rproc)) < 0) {
		dev_dbg(dev, "Failed to register remote processor (error: %d)!", ret);
		goto r_sysfs;
	}

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
			dev_err(prime->dev, "WARNING: FORCED MODULE REMOVAL WITH %d ACTIVE SWIF MAPPINGS!\n", mmap_count);
			dev_err(prime->dev, "============================================\n");

			/* Force clear the mmap count to allow prime_stop to proceed */
			prime->prime_started = false;
			atomic_set(&prime->swif_mmap_count, 0);
		}

		/* Now stop PRIME subsystem */
		ret = prime_stop(prime->rproc);
		if (ret < 0) {
			dev_err(prime->dev, "Failed to stop PRIME during removal: %d\n", ret);
		}
	}

	if((ret = rproc_del(prime->rproc)) < 0) {
		dev_err(prime->dev, "Failed to unregister remote processor (error: %d)!", ret);
	}

	// qcom_remove_glink_prime_subdev(prime->rproc, &prime->glink_subdev);
	// qcom_remove_ssr_subdev(prime->rproc, &prime->ssr_subdev);
	// qcom_remove_sysmon_subdev(prime->sysmon_subdev);
	sysfs_remove_group(&pdev->dev.kobj, &prime_attr_group);

	prime_cleanup_memory_region(prime);
	prime_cleanup_dev(prime);
	prime_cleanup_mmio(prime);

	if((ret = device_init_wakeup(prime->dev, false)) < 0) {
		dev_err(prime->dev, "Unable to de-init wakeup (error: %d)", ret);
	}
	rproc_free(prime->rproc);

}


static const struct prime_data prime_resource_init = {
	.firmware_name = "qcom_prime.elf",
	.pas_id = 81,
	.ssr_name = "prime",
	.auto_boot = false,
	.qmp_name = "prime",
	.config_mmu = true,
};

static const struct prime_data prime_resource_init_ipq = {
	.firmware_name = "qcom_prime_ipq.elf",
	.pas_id = 0xc3,
	.ssr_name = "prime",
	.auto_boot = false,
	.qmp_name = "prime",
	.config_mmu = false,
	.tmelcom_support = true,
};

static const struct prime_data prime_resource_generic = {
	.firmware_name = "qcom_prime.elf",
	.pas_id = 81,
	.ssr_name = "prime",
	.auto_boot = false,
	.qmp_name = "prime",
	.config_mmu = false,
};

/**
 *    PRIME firmware matching table.
 */
static const struct of_device_id prime_of_match[] = {
	{ .compatible = "qcom,generic-primess-pas", .data = &prime_resource_generic },
	{ .compatible = "qcom,sdxecho-primess-pas", .data = &prime_resource_init },
	{ .compatible = "qcom,ipq9650-primess-pas", .data = &prime_resource_init_ipq },
	{},
};
MODULE_DEVICE_TABLE(of, prime_of_match);

/**
 *    PRIMEModule Ops.
 */
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
MODULE_LICENSE("GPL v2");
