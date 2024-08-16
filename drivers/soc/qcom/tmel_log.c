// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/key.h>
#include <linux/kobject.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sysfs.h>
#include <linux/tmelcom_ipc.h>

#define MAX_COMPONENT		30
#define MAX_ARG_SIZE		MAX_COMPONENT * 2
#define MAX_LOG_SIZE		4096

static int log_level[MAX_ARG_SIZE] = {-1};
static int argc = 0;

static ssize_t tmel_log_read(struct file *fp, char __user *user_buffer,
				size_t count, loff_t *position)
{
	char *log;
	uint32_t size;
	int ret = 0;

	log = kzalloc(MAX_LOG_SIZE, GFP_KERNEL);
	if (!log)
		return -ENOMEM;

	ret = tmelcom_get_tmel_log(log, MAX_LOG_SIZE, &size);
        if (ret) {
		pr_err("%s : Get TMEL LOG is failed\n", __func__);
		return ret;
	}
	pr_info("TMEL Log buffer size : %x\n", size);

	return simple_read_from_buffer(user_buffer, count, position,
					log, size);
}

static const struct file_operations tmel_log_fops = {
	.read = tmel_log_read,
};

static int tmel_log_probe(struct platform_device *pdev)
{
	struct tmel_log_config *log_config;
	struct dentry *file;
	uint32_t count = 0;
	int i, ret = 0;

	file  = debugfs_create_file("tmel_log", 0444, NULL,
					NULL, &tmel_log_fops);
	if (IS_ERR_OR_NULL(file)) {
		dev_err(&pdev->dev, "unable to create tmel_log debugfs\n");
		return -EIO;
	}
	if (!argc)
		return ret;

	/* argc will have component id and loglevel, 2 for each entry, so
	   checks argc % 2 != 0
	 */
	if (argc % 2 != 0 || argc > MAX_ARG_SIZE) {
		dev_err(&pdev->dev,
			"Invalid arguments to parse component and log level\n");
		return ret;
	}

	log_config = kzalloc((argc / 2) * sizeof(*log_config), GFP_KERNEL);
	if (!log_config)
		return -ENOMEM;

	for (i = 0; i < argc; i = i + 2) {
		dev_info(&pdev->dev,
			"component ID : Log Level = %d : %d\n",
			log_level[i], log_level[i+1]);
		log_config[count].component_id = log_level[i];
		log_config[count].log_level = log_level[i+1];
		count++;
	}

	ret = tmelcom_set_tmel_log_config(log_config,
			(argc / 2) * sizeof(log_config));
	if (ret) {
		dev_err(&pdev->dev,
			"failed to set the config, ret = %d\n", ret);
	}

	return ret;
}

static const struct of_device_id tmel_log_match_tbl[] = {
	{.compatible = "qcom,tmel-log"},
	{},
};
MODULE_DEVICE_TABLE(of, tmel_log_match_tbl);

static struct platform_driver tmel_log_driver = {
	.probe	= tmel_log_probe,
	.driver	= {
		.name = "tmel-log",
		.of_match_table = tmel_log_match_tbl,
	},
};
module_platform_driver(tmel_log_driver);

module_param_array(log_level, int, &argc, 0000);
MODULE_PARM_DESC(log_level, "An array of components and log level");

MODULE_DESCRIPTION("Collect TMEL LOG using component and log level id's");
MODULE_LICENSE("GPL");
