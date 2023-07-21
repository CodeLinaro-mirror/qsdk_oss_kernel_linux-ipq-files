// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2016-2018 Linaro Ltd.
 * Copyright (C) 2014 Sony Mobile Communications AB
 * Copyright (c) 2012-2018, 2021 The Linux Foundation. All rights reserved.
 * Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/soc/qcom/mdt_loader.h>
#include <linux/soc/qcom/smem.h>
#include <linux/soc/qcom/smem_state.h>
#include <linux/qcom_scm.h>
#include <linux/interrupt.h>
#include "qcom_common.h"
#include "qcom_q6v5.h"

#include "remoteproc_internal.h"

#define WCSS_CRASH_REASON		421
#define WCSS_SMEM_HOST			1

#define WCNSS_PAS_ID			6
#define MPD_WCNSS_PAS_ID        0xD

#define RPD_SWID		MPD_WCNSS_PAS_ID
#define UPD_SWID		0x12

#define BUF_SIZE			35

#define REMOTE_PID			1
#define Q6_BOOT_ARGS_SMEM_SIZE		4096
#define UPD_BOOTARGS_HEADER_TYPE	0x2

/**
 * enum state - state of a wcss (private)
 * @WCSS_NORMAL: subsystem is operating normally
 * @WCSS_CRASHED: subsystem has crashed and hasn't been shutdown
 * @WCSS_RESTARTING: subsystem has been shutdown and is now restarting
 * @WCSS_SHUTDOWN: subsystem has been shutdown
 *
 */
enum q6_wcss_state {
	WCSS_NORMAL,
	WCSS_CRASHED,
	WCSS_RESTARTING,
	WCSS_SHUTDOWN,
};

enum {
	Q6_IPQ,
	WCSS_AHB_IPQ,
	WCSS_PCIE_IPQ,
};

enum q6_bootargs_version {
	VERSION1 = 1,
	VERSION2,
};

struct q6_wcss {
	struct device *dev;
	struct qcom_rproc_glink glink_subdev;
	struct qcom_rproc_ssr ssr_subdev;
	struct qcom_q6v5 q6;
	phys_addr_t mem_phys;
	phys_addr_t mem_reloc;
	void *mem_region;
	size_t mem_size;
	int crash_reason_smem;
	u32 version;
	s8 pd_asid;
	enum q6_wcss_state state;
	bool is_fw_shared;
};

struct wcss_data {
	int (*init_irq)(struct qcom_q6v5 *q6, struct platform_device *pdev,
			struct rproc *rproc, int remote_id,
			int crash_reason, const char *load_state,
			void (*handover)(struct qcom_q6v5 *q6));
	const char *q6_firmware_name;
	int crash_reason_smem;
	int remote_id;
	u32 version;
	const char *ssr_name;
	const struct rproc_ops *ops;
	bool need_auto_boot;
	bool glink_subdev_required;
	s8 pd_asid;
	bool reset_seq;
	u32 pasid;
	int (*mdt_load_sec)(struct device *dev, const struct firmware *fw,
			    const char *fw_name, int pas_id, void *mem_region,
			    phys_addr_t mem_phys, size_t mem_size,
			    phys_addr_t *reloc_base);
	bool is_fw_shared;
	int (*powerup_scm)(u32 peripheral);
	int (*powerdown_scm)(u32 peripheral);
	u8 bootargs_version;
};

struct bootargs_smem_info {
	void *smem_base_ptr;
	void *smem_elem_cnt_ptr;
	void *smem_bootargs_ptr;
};

struct bootargs_header {
	u8 type;
	u8 length;
};

struct q6_userpd_bootargs {
	struct bootargs_header header;
	u8 pid;
	u32 bootaddr;
	u32 data_size;
} __packed;

static int q6_wcss_start(struct rproc *rproc)
{
	struct q6_wcss *wcss = rproc->priv;
	int ret;
	struct device_node *upd_np, *temp;
	struct platform_device *upd_pdev;
	struct rproc *upd_rproc;
	struct q6_wcss *upd_wcss;
	const struct wcss_data *desc;

	desc = of_device_get_match_data(wcss->dev);
	if (!desc)
		return -EINVAL;

	qcom_q6v5_prepare(&wcss->q6);

	ret = qcom_scm_pas_auth_and_reset(desc->pasid);
	if (ret) {
		dev_err(wcss->dev, "wcss_reset failed\n");
		return ret;
	}

	ret = qcom_q6v5_wait_for_start(&wcss->q6, 5 * HZ);
	if (ret == -ETIMEDOUT)
		dev_err(wcss->dev, "start timed out\n");

	/* Bring userpd wcss state to default value */
	for_each_available_child_of_node(wcss->dev->of_node, upd_np) {
		if (!strstr(upd_np->name, "pd"))
			continue;
		upd_pdev = of_find_device_by_node(upd_np);
		upd_rproc = platform_get_drvdata(upd_pdev);
		upd_wcss = upd_rproc->priv;
		upd_wcss->state = WCSS_NORMAL;

		for_each_available_child_of_node(upd_np, temp) {
			upd_pdev = of_find_device_by_node(temp);
			upd_rproc = platform_get_drvdata(upd_pdev);
			upd_wcss = upd_rproc->priv;
			upd_wcss->state = WCSS_NORMAL;
		}
	}
	return ret;
}

static int q6_wcss_spawn_pd(struct rproc *rproc)
{
	int ret;
	struct q6_wcss *wcss = rproc->priv;

	ret = qcom_q6v5_request_spawn(&wcss->q6);
	if (ret == -ETIMEDOUT) {
		pr_err("%s spawn timedout\n", rproc->name);
		return ret;
	}

	ret = qcom_q6v5_wait_for_start(&wcss->q6, msecs_to_jiffies(10000));
	if (ret == -ETIMEDOUT) {
		pr_err("%s start timedout\n", rproc->name);
		wcss->q6.running = false;
		return ret;
	}
	wcss->q6.running = true;
	return ret;
}

static int wcss_ahb_pcie_pd_start(struct rproc *rproc)
{
	struct q6_wcss *wcss = rproc->priv;
	const struct wcss_data *desc = of_device_get_match_data(wcss->dev);
	u8 pd_asid = qcom_get_pd_asid(wcss->dev->of_node);
	u32 pasid = desc->pasid;
	int ret;

	if (desc->reset_seq) {
		if (!pasid)
			pasid = (pd_asid << 8) | UPD_SWID;
		ret = desc->powerup_scm(pasid);
		if (ret) {
			dev_err(wcss->dev, "failed to power up ahb pd\n");
			return ret;
		}
	}

	if (wcss->q6.spawn_bit) {
		ret = q6_wcss_spawn_pd(rproc);
		if (ret)
			return ret;
	}

	wcss->state = WCSS_NORMAL;
	return 0;
}

static int q6_wcss_stop(struct rproc *rproc)
{
	struct q6_wcss *wcss = rproc->priv;
	int ret;
	const struct wcss_data *desc =
			of_device_get_match_data(wcss->dev);

	if (!desc)
		return -EINVAL;

	ret = qcom_scm_pas_shutdown(desc->pasid);
	if (ret) {
		dev_err(wcss->dev, "not able to shutdown\n");
		return ret;
	}
	qcom_q6v5_unprepare(&wcss->q6);

	return 0;
}

static int wcss_ahb_pcie_pd_stop(struct rproc *rproc)
{
	struct q6_wcss *wcss = rproc->priv;
	struct rproc *rpd_rproc = dev_get_drvdata(wcss->dev->parent);
	const struct wcss_data *desc = of_device_get_match_data(wcss->dev);
	u8 pd_asid = qcom_get_pd_asid(wcss->dev->of_node);
	u32 pasid = desc->pasid;
	int ret;

	if (rproc->state != RPROC_CRASHED && wcss->q6.stop_bit) {
		ret = qcom_q6v5_request_stop(&wcss->q6, NULL);
		if (ret) {
			dev_err(&rproc->dev, "pd not stopped\n");
			return ret;
		}
	}

	if (desc->reset_seq) {
		if (!pasid)
			pasid = (pd_asid << 8) | UPD_SWID;
		ret = desc->powerdown_scm(pasid);
		if (ret) {
			dev_err(wcss->dev, "failed to power down pd\n");
			return ret;
		}
	}

	if (rproc->state != RPROC_CRASHED)
		rproc_shutdown(rpd_rproc);

	wcss->state = WCSS_SHUTDOWN;
	return 0;
}

static void *q6_wcss_da_to_va(struct rproc *rproc, u64 da, size_t len,
			      bool *is_iomem)
{
	struct q6_wcss *wcss = rproc->priv;
	int offset;

	offset = da - wcss->mem_reloc;
	if (offset < 0 || offset + len > wcss->mem_size)
		return NULL;

	return wcss->mem_region + offset;
}

static int copy_userpd_bootargs(struct bootargs_smem_info *boot_args,
				struct rproc *upd_rproc)
{
	struct q6_wcss *upd_wcss = upd_rproc->priv;
	int ret;
	const struct firmware *fw;
	struct q6_userpd_bootargs upd_bootargs = {0};
	struct device *dev = upd_wcss->dev;

	/* TYPE */
	upd_bootargs.header.type = UPD_BOOTARGS_HEADER_TYPE;

	/* LENGTH */
	upd_bootargs.header.length =
		sizeof(struct q6_userpd_bootargs) - sizeof(upd_bootargs.header);

	/* PID */
	upd_bootargs.pid = qcom_get_pd_asid(upd_wcss->dev->of_node) + 1;

	ret = request_firmware(&fw, upd_rproc->firmware, upd_wcss->dev);
	if (ret < 0) {
		dev_err(upd_wcss->dev, "request_firmware failed: %d\n",	ret);
		return ret;
	}

	/* Load address */
	upd_bootargs.bootaddr = rproc_get_boot_addr(upd_rproc, fw);

	/* PIL data size */
	upd_bootargs.data_size = qcom_mdt_get_file_size(fw);

	release_firmware(fw);

	/* copy into smem bootargs array*/
	memcpy_toio(boot_args->smem_bootargs_ptr,
		    &upd_bootargs, sizeof(struct q6_userpd_bootargs));
	boot_args->smem_bootargs_ptr += sizeof(struct q6_userpd_bootargs);
	return ret;
}

static int load_userpd_params_to_bootargs(struct device *dev,
					  struct bootargs_smem_info *boot_args)
{
	int ret = 0;
	struct device_node *upd_np, *temp;
	struct platform_device *upd_pdev;
	struct rproc *upd_rproc;
	u16 cnt;
	u8 upd_cnt = 0;

	if (!of_property_read_bool(dev->of_node, "qcom,userpd-bootargs"))
		return -EINVAL;

	for_each_available_child_of_node(dev->of_node, upd_np) {
		if (!strstr(upd_np->name, "pd"))
			continue;
		upd_cnt++;
		for_each_available_child_of_node(upd_np, temp)
			upd_cnt++;
	}

	/* No of elements */
	cnt = *((u16 *)boot_args->smem_elem_cnt_ptr);
	cnt += (sizeof(struct q6_userpd_bootargs) * upd_cnt);
	memcpy_toio(boot_args->smem_elem_cnt_ptr, &cnt, sizeof(u16));

	for_each_available_child_of_node(dev->of_node, upd_np) {
		if (!strstr(upd_np->name, "pd"))
			continue;
		upd_pdev = of_find_device_by_node(upd_np);
		upd_rproc = platform_get_drvdata(upd_pdev);
		ret = copy_userpd_bootargs(boot_args, upd_rproc);
		if (ret)
			return ret;

		for_each_available_child_of_node(upd_np, temp) {
			upd_pdev = of_find_device_by_node(temp);
			upd_rproc = platform_get_drvdata(upd_pdev);
			ret = copy_userpd_bootargs(boot_args, upd_rproc);
			if (ret)
				return ret;
		}
	}
	return ret;
}

static int share_bootargs_to_q6(struct device *dev)
{
	int ret;
	u32 smem_id, rd_val;
	const char *key = "qcom,bootargs_smem";
	size_t size;
	u16 cnt, tmp, version;
	void *ptr;
	u8 *bootargs_arr;
	struct device_node *np = dev->of_node;
	struct bootargs_smem_info boot_args;
	const struct wcss_data *desc =
				of_device_get_match_data(dev);

	if (!desc)
		return -EINVAL;

	ret = of_property_read_u32(np, key, &smem_id);
	if (ret) {
		pr_err("failed to get smem id\n");
		return ret;
	}

	ret = qcom_smem_alloc(REMOTE_PID, smem_id, Q6_BOOT_ARGS_SMEM_SIZE);
	if (ret && ret != -EEXIST) {
		pr_err("failed to allocate q6 bootargs smem segment\n");
		return ret;
	}

	boot_args.smem_base_ptr = qcom_smem_get(REMOTE_PID, smem_id, &size);
	if (IS_ERR(boot_args.smem_base_ptr)) {
		pr_err("Unable to acquire smp2p item(%d) ret:%ld\n",
		       smem_id, PTR_ERR(boot_args.smem_base_ptr));
		return PTR_ERR(boot_args.smem_base_ptr);
	}
	ptr = boot_args.smem_base_ptr;

	/*get physical address*/
	pr_info("smem phyiscal address:0x%lX\n",
		(uintptr_t)qcom_smem_virt_to_phys(ptr));

	/*Version*/
	version = desc->bootargs_version;
	memcpy_toio(ptr, &version, sizeof(version));
	ptr += sizeof(version);
	boot_args.smem_elem_cnt_ptr = ptr;

	ret = of_property_count_u32_elems(np, "boot-args");
	cnt = ret;
	if (ret < 0) {
		if (ret == -ENODATA) {
			pr_err("failed to read boot args ret:%d\n", ret);
			return ret;
		}
		cnt = 0;
	}

	/* No of elements */
	memcpy_toio(ptr, &cnt, sizeof(u16));
	ptr += sizeof(u16);

	bootargs_arr = kzalloc(cnt, GFP_KERNEL);
	if (!bootargs_arr) {
		pr_err("failed to allocate memory\n");
		return PTR_ERR(bootargs_arr);
	}

	for (tmp = 0; tmp < cnt; tmp++) {
		ret = of_property_read_u32_index(np, "boot-args", tmp, &rd_val);
		if (ret) {
			pr_err("failed to read boot args\n");
			kfree(bootargs_arr);
			return ret;
		}
		bootargs_arr[tmp] = (u8)rd_val;
	}

	/* Copy bootargs */
	memcpy_toio(ptr, bootargs_arr, cnt);
	ptr += (cnt);
	boot_args.smem_bootargs_ptr = ptr;

	of_node_put(np);
	kfree(bootargs_arr);

	ret = load_userpd_params_to_bootargs(dev, &boot_args);
	if (ret < 0) {
		pr_err("failed to read userpd boot args ret:%d\n", ret);
		return ret;
	}

	return 0;
}

static int load_m3_firmware(struct device_node *np, struct q6_wcss *wcss)
{
	int ret;
	const struct firmware *m3_fw;
	const char *m3_fw_name;

	ret = of_property_read_string(np, "m3_firmware", &m3_fw_name);
	if (ret)
		return 0;

	ret = request_firmware(&m3_fw, m3_fw_name, wcss->dev);
	if (ret)
		return 0;

	ret = qcom_mdt_load_no_init(wcss->dev, m3_fw,
				    m3_fw_name, 0,
				    wcss->mem_region, wcss->mem_phys,
				    wcss->mem_size, &wcss->mem_reloc);
	release_firmware(m3_fw);

	if (ret) {
		dev_err(wcss->dev, "can't load %s ret:%d\n", m3_fw_name, ret);
		return ret;
	}

	dev_info(wcss->dev, "m3 firmware %s loaded to DDR\n", m3_fw_name);
	return ret;
}

static int q6_wcss_load(struct rproc *rproc, const struct firmware *fw)
{
	struct q6_wcss *wcss = rproc->priv;
	const struct firmware *m3_fw;
	int ret;
	const char *m3_fw_name;
	struct device_node *upd_np, *temp;
	struct platform_device *upd_pdev;
	const struct wcss_data *desc =
				of_device_get_match_data(wcss->dev);

	if (!desc)
		return -EINVAL;

	/* Share boot args to Q6 remote processor */
	ret = share_bootargs_to_q6(wcss->dev);
	if (ret && ret != -EINVAL) {
		dev_err(wcss->dev,
			"boot args sharing with q6 failed %d\n",
			ret);
		return ret;
	}

	/* load m3 firmware */
	for_each_available_child_of_node(wcss->dev->of_node, upd_np) {
		if (!strstr(upd_np->name, "pd"))
			continue;
		upd_pdev = of_find_device_by_node(upd_np);
		ret = load_m3_firmware(upd_np, wcss);
		if (ret)
			return ret;

		for_each_available_child_of_node(upd_np, temp) {
			upd_pdev = of_find_device_by_node(temp);
			ret = load_m3_firmware(temp, wcss);
			if (ret)
				return ret;
		}
	}

	return qcom_mdt_load(wcss->dev, fw, rproc->firmware,
				desc->pasid, wcss->mem_region,
				wcss->mem_phys, wcss->mem_size,
				&wcss->mem_reloc);
}

static int wcss_ahb_pcie_pd_load(struct rproc *rproc, const struct firmware *fw)
{
	struct q6_wcss *wcss = rproc->priv;
	struct rproc *rpd_rproc = dev_get_drvdata(wcss->dev->parent);
	int ret;
	u8 pd_asid;
	u32 pasid;
	const struct wcss_data *desc =
				of_device_get_match_data(wcss->dev);

	if (!desc)
		return -EINVAL;

	/* Boot rootpd rproc */
	ret = rproc_boot(rpd_rproc);
	if ((ret || wcss->state == WCSS_NORMAL) && wcss->is_fw_shared)
		return ret;

	pasid = desc->pasid;
	if (!pasid) {
		pd_asid = qcom_get_pd_asid(wcss->dev->of_node);
		pasid = (pd_asid << 8) | UPD_SWID;
	}

	return desc->mdt_load_sec(wcss->dev, fw, rproc->firmware,
				pasid, wcss->mem_region,
				wcss->mem_phys, wcss->mem_size,
				&wcss->mem_reloc);
}

static unsigned long q6_wcss_panic(struct rproc *rproc)
{
	struct q6_wcss *wcss = rproc->priv;

	return qcom_q6v5_panic(&wcss->q6);
}

static const struct rproc_ops wcss_ahb_pcie_ipq5018_ops = {
	.start = wcss_ahb_pcie_pd_start,
	.stop = wcss_ahb_pcie_pd_stop,
	.load = wcss_ahb_pcie_pd_load,
	.get_boot_addr = rproc_elf_get_boot_addr,
};

static const struct rproc_ops q6_wcss_ipq5018_ops = {
	.start = q6_wcss_start,
	.stop = q6_wcss_stop,
	.da_to_va = q6_wcss_da_to_va,
	.load = q6_wcss_load,
	.get_boot_addr = rproc_elf_get_boot_addr,
	.panic = q6_wcss_panic,
};

static int q6_alloc_memory_region(struct q6_wcss *wcss)
{
	struct reserved_mem *rmem = NULL;
	struct device_node *node;
	struct device *dev = wcss->dev;

	if (wcss->version == Q6_IPQ) {
		node = of_parse_phandle(dev->of_node, "memory-region", 0);
		if (node)
			rmem = of_reserved_mem_lookup(node);

		of_node_put(node);

		if (!rmem) {
			dev_err(dev, "unable to acquire memory-region\n");
			return -EINVAL;
		}
	} else {
		struct rproc *rpd_rproc = dev_get_drvdata(dev->parent);
		struct q6_wcss *rpd_wcss = rpd_rproc->priv;

		wcss->mem_phys = rpd_wcss->mem_phys;
		wcss->mem_reloc = rpd_wcss->mem_reloc;
		wcss->mem_size = rpd_wcss->mem_size;
		wcss->mem_region = rpd_wcss->mem_region;
		return 0;
	}

	wcss->mem_phys = rmem->base;
	wcss->mem_reloc = rmem->base;
	wcss->mem_size = rmem->size;
	wcss->mem_region = devm_ioremap_wc(dev, wcss->mem_phys, wcss->mem_size);
	if (!wcss->mem_region) {
		dev_err(dev, "unable to map memory region: %pa+%pa\n",
			&rmem->base, &rmem->size);
		return -EBUSY;
	}
	return 0;
}

static int q6_get_inbound_irq(struct qcom_q6v5 *q6,
			      struct platform_device *pdev,
			      const char *int_name,
			      irqreturn_t (*handler)(int irq, void *data))
{
	int ret, irq;
	char *interrupt, *tmp = (char *)int_name;
	struct q6_wcss *wcss = q6->rproc->priv;

	irq = platform_get_irq_byname(pdev, int_name);
	if (irq < 0) {
		if (irq != -EPROBE_DEFER)
			dev_err(&pdev->dev,
				"failed to retrieve %s IRQ: %d\n",
					int_name, irq);
		return irq;
	}

	if (!strcmp(int_name, "fatal")) {
		q6->fatal_irq = irq;
	} else if (!strcmp(int_name, "stop-ack")) {
		q6->stop_irq = irq;
		tmp = "stop_ack";
	} else if (!strcmp(int_name, "ready")) {
		q6->ready_irq = irq;
	} else if (!strcmp(int_name, "handover")) {
		q6->handover_irq  = irq;
	} else if (!strcmp(int_name, "spawn-ack")) {
		q6->spawn_irq = irq;
		tmp = "spawn_ack";
	} else {
		dev_err(&pdev->dev, "unknown interrupt\n");
		return -EINVAL;
	}

	interrupt = devm_kzalloc(&pdev->dev, BUF_SIZE, GFP_KERNEL);
	if (!interrupt)
		return -ENOMEM;

	snprintf(interrupt, BUF_SIZE, "q6v5_wcss_userpd%d", wcss->pd_asid);
	strlcat(interrupt, "_", BUF_SIZE);
	strlcat(interrupt, tmp, BUF_SIZE);

	ret = devm_request_threaded_irq(&pdev->dev, irq,
					NULL, handler,
					IRQF_TRIGGER_RISING | IRQF_ONESHOT,
					interrupt, q6);
	if (ret) {
		dev_err(&pdev->dev, "failed to acquire %s irq\n", interrupt);
		return ret;
	}
	return 0;
}

static int q6_get_outbound_irq(struct qcom_q6v5 *q6,
			       struct platform_device *pdev,
			       const char *int_name)
{
	struct qcom_smem_state *tmp_state;
	unsigned  bit;

	tmp_state = qcom_smem_state_get(&pdev->dev, int_name, &bit);
	if (IS_ERR(tmp_state)) {
		dev_err(&pdev->dev, "failed to acquire %s state\n", int_name);
		return PTR_ERR(tmp_state);
	}

	if (!strcmp(int_name, "stop")) {
		q6->state = tmp_state;
		q6->stop_bit = bit;
	} else if (!strcmp(int_name, "spawn")) {
		q6->spawn_state = tmp_state;
		q6->spawn_bit = bit;
	}

	return 0;
}

static int init_irq(struct qcom_q6v5 *q6,
		    struct platform_device *pdev, struct rproc *rproc,
		    int remote_id, int crash_reason, const char *load_state,
		    void (*handover)(struct qcom_q6v5 *q6))
{
	int ret;

	q6->rproc = rproc;
	q6->dev = &pdev->dev;
	q6->crash_reason = crash_reason;
	q6->remote_id = remote_id;
	q6->handover = handover;

	init_completion(&q6->start_done);
	init_completion(&q6->stop_done);
	init_completion(&q6->spawn_done);

	ret = q6_get_inbound_irq(q6, pdev, "fatal",
				 q6v5_fatal_interrupt);
	if (ret)
		return ret;

	ret = q6_get_inbound_irq(q6, pdev, "ready",
				 q6v5_ready_interrupt);
	if (ret)
		return ret;

	ret = q6_get_inbound_irq(q6, pdev, "stop-ack",
				 q6v5_stop_interrupt);
	if (ret)
		return ret;

	ret = q6_get_inbound_irq(q6, pdev, "spawn-ack",
				 q6v5_spawn_interrupt);
	if (ret)
		return ret;

	ret = q6_get_outbound_irq(q6, pdev, "stop");
	if (ret)
		return ret;

	ret = q6_get_outbound_irq(q6, pdev, "spawn");
	if (ret)
		return ret;

	return 0;
}

static int q6_wcss_probe(struct platform_device *pdev)
{
	const struct wcss_data *desc;
	struct q6_wcss *wcss;
	struct rproc *rproc;
	int ret;
	char *subdev_name;
	const char *fw_name = NULL;

	desc = of_device_get_match_data(&pdev->dev);
	if (!desc)
		return -EINVAL;

	of_property_read_string(pdev->dev.of_node, "firmware", &fw_name);
	if (!fw_name)
		fw_name = desc->q6_firmware_name;

	rproc = rproc_alloc(&pdev->dev, pdev->name, desc->ops,
			    fw_name, sizeof(*wcss));
	if (!rproc) {
		dev_err(&pdev->dev, "failed to allocate rproc\n");
		return -ENOMEM;
	}
	wcss = rproc->priv;
	wcss->dev = &pdev->dev;
	wcss->version = desc->version;
	wcss->is_fw_shared = desc->is_fw_shared;

	ret = q6_alloc_memory_region(wcss);
	if (ret)
		goto free_rproc;

	wcss->pd_asid = qcom_get_pd_asid(wcss->dev->of_node);
	if (wcss->pd_asid < 0)
		goto free_rproc;

	if (desc->init_irq) {
		ret = desc->init_irq(&wcss->q6, pdev, rproc, desc->remote_id,
				     desc->crash_reason_smem, NULL, NULL);
		if (ret) {
			if (wcss->version == Q6_IPQ)
				goto free_rproc;
			else
				dev_info(wcss->dev,
					 "userpd irq registration failed\n");
		}
	}

	if (desc->glink_subdev_required)
		qcom_add_glink_subdev(rproc, &wcss->glink_subdev, desc->ssr_name);

	subdev_name = (char *)(desc->ssr_name ? desc->ssr_name : pdev->name);
	qcom_add_ssr_subdev(rproc, &wcss->ssr_subdev, subdev_name);

	rproc->auto_boot = desc->need_auto_boot;
	ret = rproc_add(rproc);
	if (ret)
		goto free_rproc;

	platform_set_drvdata(pdev, rproc);

	ret = of_platform_populate(wcss->dev->of_node, NULL,
				   NULL, wcss->dev);
	if (ret) {
		dev_err(&pdev->dev, "failed to populate wcss pd nodes\n");
		goto free_rproc;
	}
	return 0;

free_rproc:
	rproc_free(rproc);

	return ret;
}

static int q6_wcss_remove(struct platform_device *pdev)
{
	struct rproc *rproc = platform_get_drvdata(pdev);
	struct q6_wcss *wcss = rproc->priv;

	if (wcss->version == Q6_IPQ)
		qcom_q6v5_deinit(&wcss->q6);

	rproc_del(rproc);
	rproc_free(rproc);

	return 0;
}

static const struct wcss_data q6_ipq5018_res_init = {
	.init_irq = qcom_q6v5_init,
	.q6_firmware_name = "IPQ5018/q6_fw.mdt",
	.crash_reason_smem = WCSS_CRASH_REASON,
	.remote_id = WCSS_SMEM_HOST,
	.ssr_name = "q6wcss",
	.ops = &q6_wcss_ipq5018_ops,
	.version = Q6_IPQ,
	.glink_subdev_required = true,
	.pasid = MPD_WCNSS_PAS_ID,
	.bootargs_version = VERSION1,
};

static const struct wcss_data q6_ipq5332_res_init = {
	.init_irq = qcom_q6v5_init,
	.q6_firmware_name = "IPQ5332/q6_fw0.mdt",
	.crash_reason_smem = WCSS_CRASH_REASON,
	.remote_id = WCSS_SMEM_HOST,
	.ssr_name = "q6wcss",
	.ops = &q6_wcss_ipq5018_ops,
	.version = Q6_IPQ,
	.glink_subdev_required = true,
	.pasid = RPD_SWID,
	.bootargs_version = VERSION2,
};

static const struct wcss_data q6_ipq9574_res_init = {
	.init_irq = qcom_q6v5_init,
	.q6_firmware_name = "IPQ9574/q6_fw.mdt",
	.crash_reason_smem = WCSS_CRASH_REASON,
	.remote_id = WCSS_SMEM_HOST,
	.ssr_name = "q6wcss",
	.ops = &q6_wcss_ipq5018_ops,
	.version = Q6_IPQ,
	.glink_subdev_required = true,
	.pasid = WCNSS_PAS_ID,
};

static const struct wcss_data wcss_ahb_ipq5018_res_init = {
	.init_irq = init_irq,
	.q6_firmware_name = "IPQ5018/q6_fw.mdt",
	.crash_reason_smem = WCSS_CRASH_REASON,
	.remote_id = WCSS_SMEM_HOST,
	.ops = &wcss_ahb_pcie_ipq5018_ops,
	.version = WCSS_AHB_IPQ,
	.pasid = MPD_WCNSS_PAS_ID,
	.reset_seq = true,
	.mdt_load_sec = qcom_mdt_load_pd_seg,
	.is_fw_shared = true,
	.powerup_scm = qti_scm_int_radio_powerup,
	.powerdown_scm = qti_scm_int_radio_powerdown,
};

static const struct wcss_data wcss_ahb_ipq5332_res_init = {
	.init_irq = init_irq,
	.q6_firmware_name = "IPQ5332/q6_fw1.mdt",
	.crash_reason_smem = WCSS_CRASH_REASON,
	.remote_id = WCSS_SMEM_HOST,
	.ops = &wcss_ahb_pcie_ipq5018_ops,
	.version = WCSS_AHB_IPQ,
	.reset_seq = true,
	.mdt_load_sec = qcom_mdt_load,
	.powerup_scm = qcom_scm_pas_auth_and_reset,
	.powerdown_scm = qcom_scm_pas_shutdown,
};

static const struct wcss_data wcss_ahb_ipq9574_res_init = {
	.q6_firmware_name = "IPQ9574/q6_fw.mdt",
	.crash_reason_smem = WCSS_CRASH_REASON,
	.remote_id = WCSS_SMEM_HOST,
	.ops = &wcss_ahb_pcie_ipq5018_ops,
	.version = WCSS_AHB_IPQ,
	.pasid = WCNSS_PAS_ID,
	.mdt_load_sec = qcom_mdt_load,
	.is_fw_shared = true,
};

static const struct wcss_data wcss_pcie_ipq5018_res_init = {
	.init_irq = init_irq,
	.q6_firmware_name = "IPQ5018/q6_fw.mdt",
	.crash_reason_smem = WCSS_CRASH_REASON,
	.ops = &wcss_ahb_pcie_ipq5018_ops,
	.version = WCSS_PCIE_IPQ,
	.mdt_load_sec = qcom_mdt_load_pd_seg,
	.pasid = MPD_WCNSS_PAS_ID,
	.is_fw_shared = true,
};

static const struct wcss_data wcss_pcie_ipq5332_res_init = {
	.init_irq = init_irq,
	.q6_firmware_name = "IPQ5332/q6_fw2.mdt",
	.crash_reason_smem = WCSS_CRASH_REASON,
	.remote_id = WCSS_SMEM_HOST,
	.ops = &wcss_ahb_pcie_ipq5018_ops,
	.version = WCSS_PCIE_IPQ,
	.reset_seq = true,
	.mdt_load_sec = qcom_mdt_load,
	.powerup_scm = qcom_scm_pas_auth_and_reset,
	.powerdown_scm = qcom_scm_pas_shutdown,
};

static const struct wcss_data wcss_text_ipq5332_res_init = {
	.q6_firmware_name = "IPQ5332/q6_fw4.mdt",
	.ops = &wcss_ahb_pcie_ipq5018_ops,
	.version = WCSS_AHB_IPQ,
	.reset_seq = true,
	.mdt_load_sec = qcom_mdt_load,
	.powerup_scm = qcom_scm_pas_auth_and_reset,
	.powerdown_scm = qcom_scm_pas_shutdown,
};

static const struct of_device_id q6_wcss_of_match[] = {
	{ .compatible = "qcom,ipq5018-q6-mpd", .data = &q6_ipq5018_res_init },
	{ .compatible = "qcom,ipq5332-q6-mpd", .data = &q6_ipq5332_res_init },
	{ .compatible = "qcom,ipq9574-q6-mpd", .data = &q6_ipq9574_res_init },
	{ .compatible = "qcom,ipq5018-wcss-ahb-mpd",
		.data = &wcss_ahb_ipq5018_res_init },
	{ .compatible = "qcom,ipq5332-wcss-ahb-mpd",
		.data = &wcss_ahb_ipq5332_res_init },
	{ .compatible = "qcom,ipq9574-wcss-ahb-mpd",
		.data = &wcss_ahb_ipq9574_res_init },
	{ .compatible = "qcom,ipq5018-wcss-pcie-mpd",
		.data = &wcss_pcie_ipq5018_res_init },
	{ .compatible = "qcom,ipq5332-wcss-pcie-mpd",
		.data = &wcss_pcie_ipq5332_res_init },
	{ .compatible = "qcom,ipq5332-mpd-upd-text",
		.data = &wcss_text_ipq5332_res_init },
	{ },
};
MODULE_DEVICE_TABLE(of, q6_wcss_of_match);

static struct platform_driver q6_wcss_driver = {
	.probe = q6_wcss_probe,
	.remove = q6_wcss_remove,
	.driver = {
		.name = "qcom-q6-mpd",
		.of_match_table = q6_wcss_of_match,
	},
};
module_platform_driver(q6_wcss_driver);

MODULE_DESCRIPTION("Hexagon WCSS Multipd Peripheral Image Loader");
MODULE_LICENSE("GPL v2");
