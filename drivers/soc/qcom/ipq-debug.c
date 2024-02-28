/* Copyright (c) 2015-2016, 2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2023-2024, Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/notifier.h>
#include <linux/panic_notifier.h>

#define NON_SECURE_WATCHDOG             0x1
#define AHB_TIMEOUT                     0x3
#define NOC_ERROR                       0x6
#define SYSTEM_RESET_OR_REBOOT          0x10
#define POWER_ON_RESET                  0x20
#define SECURE_WATCHDOG                 0x23
#define HLOS_PANIC                      0x47
#define VFSM_RESET                      0x68
#define TME_L_FATAL_ERROR               0x49
#define TME_L_WDT_BITE_FATAL_ERROR      0x69

#define RESET_REASON_MSG_MAX_LEN        100

struct restart_reason {
	void __iomem *wr_addr;
	struct notifier_block panic_blk;
};

static int debug_panic_handler(struct notifier_block *nb, unsigned long action,
			       void *data)
{
	struct restart_reason *reason;
	int val = 0x6;

	reason = container_of(nb, struct restart_reason, panic_blk);

	memcpy_toio(reason->wr_addr, &val, sizeof(int));
	iounmap(reason->wr_addr);

	return NOTIFY_DONE;
}

static int restart_reason_logging(unsigned int reason)
{
	char reset_reason_msg[RESET_REASON_MSG_MAX_LEN] = {};

	switch(reason) {
		case NON_SECURE_WATCHDOG:
			scnprintf(reset_reason_msg, RESET_REASON_MSG_MAX_LEN,
					"%s", "Non-Secure Watchdog ");
			break;
		case AHB_TIMEOUT:
			scnprintf(reset_reason_msg, RESET_REASON_MSG_MAX_LEN,
					"%s", "AHB Timeout ");
			break;
		case NOC_ERROR:
			scnprintf(reset_reason_msg, RESET_REASON_MSG_MAX_LEN,
					"%s", "NOC Error ");
			break;
		case SYSTEM_RESET_OR_REBOOT:
			scnprintf(reset_reason_msg, RESET_REASON_MSG_MAX_LEN,
					"%s", "System reset or reboot ");
			break;
		case POWER_ON_RESET:
			scnprintf(reset_reason_msg, RESET_REASON_MSG_MAX_LEN,
					"%s", "Power on Reset ");
			break;
		case SECURE_WATCHDOG:
			scnprintf(reset_reason_msg, RESET_REASON_MSG_MAX_LEN,
					"%s", "Secure Watchdog ");
			break;
		case HLOS_PANIC:
			scnprintf(reset_reason_msg, RESET_REASON_MSG_MAX_LEN,
					"%s", "HLOS Panic ");
			break;
		case VFSM_RESET:
			scnprintf(reset_reason_msg, RESET_REASON_MSG_MAX_LEN,
					"%s", "VFSM Reset ");
			break;
		case TME_L_FATAL_ERROR:
			scnprintf(reset_reason_msg, RESET_REASON_MSG_MAX_LEN,
					"%s", "TME-L Fatal Error ");
			break;
		case TME_L_WDT_BITE_FATAL_ERROR:
			scnprintf(reset_reason_msg, RESET_REASON_MSG_MAX_LEN,
					"%s", "TME-L WDT Bite occurred ");
			break;
	}

	pr_info("reset_reason : %s[0x%X]\n", reset_reason_msg, reason);
	return 0;
}

static const struct of_device_id ipq_debug_match_table[] = {
	{ .compatible = "qcom,ipq-debug",
	},
	{ .compatible = "qcom,ipq-debug-devsoc",
	},
	{}
};
MODULE_DEVICE_TABLE(of, ipq_debug_match_table);

void __iomem *ipq_debug_parse_address(struct device *dev,
				      const char *compatible)
{
	struct device_node *np;
	void __iomem *addr;

	np = of_find_compatible_node(NULL, NULL, compatible);
	if (!np) {
		dev_err(dev, "node %s doesn't exist\n", compatible);
		return ERR_PTR(-ENODEV);
	}

	addr = of_iomap(np, 0);
	of_node_put(np);
	if (!addr) {
		dev_err(dev, "iomap failed for compatible %s\n", compatible);
		return addr;
	}

	return addr;
}

static int ipq_debug_probe(struct platform_device *pdev)
{
	struct restart_reason *reason;
	unsigned int reset_reason;
	void __iomem *imem_base;
	struct device_node *np;
	int ret;

	np = of_node_get(pdev->dev.of_node);
	if (!np)
		return 0;

	imem_base = ipq_debug_parse_address(&pdev->dev,
				"qcom,msm-imem-restart-reason-buf-addr");
	if (IS_ERR_OR_NULL(imem_base))
		return PTR_ERR(imem_base);

	memcpy_fromio(&reset_reason, imem_base, 4);
	iounmap(imem_base);

	restart_reason_logging(reset_reason);

	/*
	 * For devsoc, kernel needs to write the restart reason in IMEM
	 * during the kernel panic.
	 */

	if (!of_device_is_compatible(np, "qcom,ipq-debug-devsoc"))
		return 0;

	reason = devm_kzalloc(&pdev->dev, sizeof(*reason), GFP_KERNEL);
	if (!reason)
		return -ENOMEM;

	reason->wr_addr = ipq_debug_parse_address(&pdev->dev,
				"qcom,imem-restart-reason-buf-wr-addr");
	if (IS_ERR_OR_NULL(reason->wr_addr))
		return PTR_ERR(reason->wr_addr);

	reason->panic_blk.notifier_call = debug_panic_handler;
	ret = atomic_notifier_chain_register(&panic_notifier_list,
					     &reason->panic_blk);
	if (ret) {
		dev_err(&pdev->dev, "failed to register the panic notifier, ret is %d\n",
			ret);
		return ret;
	}

	platform_set_drvdata(pdev, reason);

	return 0;
}

static int ipq_debug_remove(struct platform_device *pdev)
{
	struct restart_reason *reason = platform_get_drvdata(pdev);

	if (reason)
		atomic_notifier_chain_unregister(&panic_notifier_list,
						 &reason->panic_blk);
	return 0;
}

static struct platform_driver ipq_debug_driver = {
	.probe	= ipq_debug_probe,
	.remove	= ipq_debug_remove,
	.driver	= {
		.name = "qcom,ipq-debug",
		.of_match_table = ipq_debug_match_table,
	},
};

module_platform_driver(ipq_debug_driver);

MODULE_DESCRIPTION("QCOM IPQ DEBUG Driver");
