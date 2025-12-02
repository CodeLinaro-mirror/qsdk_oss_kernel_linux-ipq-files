/*
 * Copyright (c) 2015-2017, 2020 The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: ISC
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/sizes.h>

#define DIAG_MAGIC 0x47414944  /* "DIAG" in hex */

/**
 * struct diag - Main diagnostic region structure
 * @magic:       Magic identifier to validate the region
 * @ring_off:    Offset to the ring buffer from the start of the region
 * @ring_len:    Length of the ring buffer
 * @wrap:        Number of times the buffer has wrapped around
 * @offset:      Current offset in the buffer
 * @log_buf:     Flexible array for the actual log data
 **/
struct diag {
	u32 magic;
	u32 ring_off;
	u32 ring_len;
	u32 wrap;
	u32 offset;
	u8 log_buf[];
};

/**
 * struct tfa_optee_log_struct - TFA/OPTEE log info structure
 * @debugfs_dir: debugfs directory
 * @tfa_buf: TFA log buffer
 * @optee_buf: OPTEE log buffer
 * @tfa_copy_buf: TFA copy buffer for user space
 * @optee_copy_buf: OPTEE copy buffer for user space
 * @tfa_copy_len: length of TFA log copied
 * @optee_copy_len: length of OPTEE log copied
 * @tfa_phys_addr: physical address of TFA log
 * @optee_phys_addr: physical address of OPTEE log
 * @tfa_buf_size: TFA buffer size
 * @optee_buf_size: OPTEE buffer size
 * @tfa_lock: mutex lock for TFA log
 * @optee_lock: mutex lock for OPTEE log
 */
struct tfa_optee_log_struct {
	struct dentry *debugfs_dir;
	void __iomem *tfa_buf;
	void __iomem *optee_buf;
	char *tfa_copy_buf;
	char *optee_copy_buf;
	int tfa_copy_len;
	int optee_copy_len;
	phys_addr_t tfa_phys_addr;
	phys_addr_t optee_phys_addr;
	u32 tfa_buf_size;
	u32 optee_buf_size;
	struct mutex tfa_lock;
	struct mutex optee_lock;
};

static int parse_diag_log(void __iomem *buf, char *copy_buf, u32 buf_size)
{
	struct diag diag_hdr;
	u32 log_buf_off;
	u32 copy_size;
	u32 ring_len;
	u32 offset;
	u32 magic;
	u32 wrap;

	log_buf_off = offsetof(struct diag, log_buf);

	memcpy_fromio(&diag_hdr, buf, log_buf_off);

	ring_len = diag_hdr.ring_len;
	offset = diag_hdr.offset;
	wrap = diag_hdr.wrap;
	magic = diag_hdr.magic;

	if (magic != DIAG_MAGIC) {
		pr_err("Invalid magic number: 0x%x, expected 0x%x\n", magic, DIAG_MAGIC);
		return -EINVAL;
	}

	if (ring_len > buf_size - log_buf_off) {
		pr_err("Invalid ring_len: %u, max allowed: %u\n", ring_len, buf_size - log_buf_off);
		return -EINVAL;
	}

	if (offset > ring_len) {
		pr_err("Invalid offset: %u, ring_len: %u\n", offset, ring_len);
		return -EINVAL;
	}

	/* Skip the header */
	buf += log_buf_off;

	if (wrap != 0) {
		memcpy_fromio(copy_buf, (buf + offset), (ring_len - offset));
		memcpy_fromio(copy_buf + (ring_len - offset), buf, offset);
		copy_size = ring_len;
	} else {
		memcpy_fromio(copy_buf, buf, offset);
		copy_size = offset;
	}

	return copy_size;
}

static int tfa_log_open(struct inode *inode, struct file *file)
{
	struct tfa_optee_log_struct *log_data;
	int ret;

	file->private_data = inode->i_private;
	log_data = file->private_data;

	mutex_lock(&log_data->tfa_lock);

	ret = parse_diag_log(log_data->tfa_buf, log_data->tfa_copy_buf, log_data->tfa_buf_size);
	if (ret < 0) {
		pr_err("Failed to parse TFA log: %d\n", ret);
		mutex_unlock(&log_data->tfa_lock);
		return ret;
	}

	log_data->tfa_copy_len = ret;
	return 0;
}

static ssize_t tfa_log_read(struct file *fp, char __user *user_buffer,
			    size_t count, loff_t *position)
{
	struct tfa_optee_log_struct *log_data;

	log_data = fp->private_data;

	return simple_read_from_buffer(user_buffer, count, position,
				       log_data->tfa_copy_buf,
				       log_data->tfa_copy_len);
}

static int tfa_log_release(struct inode *inode, struct file *file)
{
	struct tfa_optee_log_struct *log_data;

	log_data = file->private_data;
	mutex_unlock(&log_data->tfa_lock);
	return 0;
}

static const struct file_operations fops_tfa_log = {
	.owner = THIS_MODULE,
	.open = tfa_log_open,
	.read = tfa_log_read,
	.release = tfa_log_release,
};

static int optee_log_open(struct inode *inode, struct file *file)
{
	struct tfa_optee_log_struct *log_data;
	int ret;

	file->private_data = inode->i_private;
	log_data = file->private_data;

	mutex_lock(&log_data->optee_lock);

	ret = parse_diag_log(log_data->optee_buf, log_data->optee_copy_buf, log_data->optee_buf_size);
	if (ret < 0) {
		pr_err("Failed to parse OPTEE log: %d\n", ret);
		mutex_unlock(&log_data->optee_lock);
		return ret;
	}

	log_data->optee_copy_len = ret;
	return 0;
}

static ssize_t optee_log_read(struct file *fp, char __user *user_buffer,
			      size_t count, loff_t *position)
{
	struct tfa_optee_log_struct *log_data;

	log_data = fp->private_data;

	return simple_read_from_buffer(user_buffer, count, position,
				       log_data->optee_copy_buf,
				       log_data->optee_copy_len);
}

static int optee_log_release(struct inode *inode, struct file *file)
{
	struct tfa_optee_log_struct *log_data;

	log_data = file->private_data;
	mutex_unlock(&log_data->optee_lock);

	return 0;
}

static const struct file_operations fops_optee_log = {
	.owner = THIS_MODULE,
	.open = optee_log_open,
	.read = optee_log_read,
	.release = optee_log_release,
};

static int get_diag_info_from_imem(struct platform_device *pdev,
				   const char *phandle_name,
				   phys_addr_t *addr, u32 *size)
{
	struct device_node *np = pdev->dev.of_node;
	struct device_node *node;
	void __iomem *imem_buf;
	struct resource res;
	int ret;

	node = of_parse_phandle(np, phandle_name, 0);
	if (!node)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "unable to get %s phandle\n", phandle_name);

	ret = of_address_to_resource(node, 0, &res);
	of_node_put(node);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "unable to get %s resource\n", phandle_name);

	imem_buf = devm_ioremap(&pdev->dev, res.start, resource_size(&res));
	if (!imem_buf)
		return dev_err_probe(&pdev->dev, -ENOMEM,
				     "unable to map %s buffer\n", phandle_name);

	/* Read buffer address and size from IMEM */
	*addr = readl_relaxed(imem_buf);
	*size = readl_relaxed(imem_buf + 4);

	return 0;
}

static int tfa_optee_log_probe(struct platform_device *pdev)
{
	struct tfa_optee_log_struct *log_data;
	phys_addr_t tfa_addr, optee_addr;
	u32 tfa_size, optee_size;
	int ret;

	log_data = devm_kzalloc(&pdev->dev, sizeof(struct tfa_optee_log_struct), GFP_KERNEL);
	if (!log_data)
		return -ENOMEM;


	/* Get TFA diag buffer info from IMEM */
	ret = get_diag_info_from_imem(pdev, "tfa-diag-info", &tfa_addr, &tfa_size);
	if (ret)
		return ret;

	/* Get OPTEE diag buffer info from IMEM */
	ret = get_diag_info_from_imem(pdev, "optee-diag-info", &optee_addr, &optee_size);
	if (ret)
		return ret;

	/* Validate buffer addresses and sizes */
	if (!tfa_addr || !tfa_size) {
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "invalid TFA buffer: addr=%pa, size=0x%x\n",
				     &tfa_addr, tfa_size);
	}

	if (!optee_addr || !optee_size) {
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "invalid OPTEE buffer: addr=%pa, size=0x%x\n",
				     &optee_addr, optee_size);
	}

	log_data->tfa_phys_addr = tfa_addr;
	log_data->tfa_buf_size = tfa_size;
	log_data->optee_phys_addr = optee_addr;
	log_data->optee_buf_size = optee_size;

	log_data->tfa_buf = devm_ioremap(&pdev->dev, log_data->tfa_phys_addr, log_data->tfa_buf_size);
	if (!log_data->tfa_buf)
		return dev_err_probe(&pdev->dev, -ENOMEM, "unable to map TFA log buffer\n");

	log_data->optee_buf = devm_ioremap(&pdev->dev, log_data->optee_phys_addr, log_data->optee_buf_size);
	if (!log_data->optee_buf)
		return dev_err_probe(&pdev->dev, -ENOMEM, "unable to map OPTEE log buffer\n");

	log_data->tfa_copy_buf = devm_kzalloc(&pdev->dev, log_data->tfa_buf_size, GFP_KERNEL);
	if (!log_data->tfa_copy_buf)
		return -ENOMEM;

	log_data->optee_copy_buf = devm_kzalloc(&pdev->dev, log_data->optee_buf_size, GFP_KERNEL);
	if (!log_data->optee_copy_buf)
		return -ENOMEM;

	mutex_init(&log_data->tfa_lock);
	mutex_init(&log_data->optee_lock);

	log_data->debugfs_dir = debugfs_create_dir("qti_debug_logs", NULL);

	debugfs_create_file("tfa_log", 0444, log_data->debugfs_dir,
				      log_data, &fops_tfa_log);

	debugfs_create_file("optee_log", 0444, log_data->debugfs_dir,
				      log_data, &fops_optee_log);

	platform_set_drvdata(pdev, log_data);

	dev_dbg(&pdev->dev, "TFA log: addr=%pa, size=0x%x\tOPTEE log: addr=%pa, size=0x%x\n",
		&tfa_addr, tfa_size, &optee_addr, optee_size);

	return 0;
}

static int tfa_optee_log_remove(struct platform_device *pdev)
{
	struct tfa_optee_log_struct *log_data = platform_get_drvdata(pdev);

	debugfs_remove_recursive(log_data->debugfs_dir);

	mutex_destroy(&log_data->tfa_lock);
	mutex_destroy(&log_data->optee_lock);

	return 0;
}

static const struct of_device_id tfa_optee_log_of_match[] = {
	{ .compatible = "qti,tfa-optee-log" },
	{}
};
MODULE_DEVICE_TABLE(of, tfa_optee_log_of_match);

static struct platform_driver tfa_optee_log_driver = {
	.probe = tfa_optee_log_probe,
	.remove = tfa_optee_log_remove,
	.driver = {
		.name = "qti_tfa_optee_log",
		.of_match_table = tfa_optee_log_of_match,
	},
};

module_platform_driver(tfa_optee_log_driver);

MODULE_DESCRIPTION("Qualcomm TFA/OPTEE Log Driver");
MODULE_LICENSE("Dual BSD/GPL");
