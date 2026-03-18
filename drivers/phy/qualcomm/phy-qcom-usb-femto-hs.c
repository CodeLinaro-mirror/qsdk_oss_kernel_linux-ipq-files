// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>

/* Register offsets */
#define USB2_PHY_USB_PHY_UTMI_CTRL0		0x3c
#define USB2_PHY_USB_PHY_UTMI_CTRL5		0x50
#define USB2_PHY_USB_PHY_HS_PHY_CTRL_COMMON0	0x54
#define USB2_PHY_USB_PHY_HS_PHY_CTRL_COMMON1	0x58
#define USB2_PHY_USB_PHY_HS_PHY_CTRL_COMMON2	0x5c
#define USB2_PHY_USB_PHY_HS_PHY_CTRL1		0x60
#define USB2_PHY_USB_PHY_HS_PHY_CTRL2		0x64
#define USB2_PHY_USB_PHY_HS_PHY_OVERRIDE_X1	0x70
#define USB2_PHY_USB_PHY_HS_PHY_TEST0		0x80
#define USB2_PHY_USB_PHY_HS_PHY_TEST1		0x84
#define USB2_PHY_USB_PHY_CFG0			0x94
#define USB2_PHY_USB_PHY_REFCLK_CTRL		0xa0
#define USB2_PHY_USB_PHY_FSEL_SEL		0xb8

/* USB2_PHY_USB_PHY_UTMI_CTRL0 bits */
#define SLEEPM					BIT(0)

/* USB2_PHY_USB_PHY_UTMI_CTRL5 bits */
#define POR					BIT(1)
#define ATERESET				BIT(0)

/* USB2_PHY_USB_PHY_HS_PHY_CTRL_COMMON0 bits */
#define FSEL_MASK				GENMASK(6, 4)
#define FSEL_24MHZ				(0x2 << 4)
#define VATESTENB_MASK				GENMASK(1, 0)
#define COMMONONN				BIT(7)

/* USB2_PHY_USB_PHY_HS_PHY_CTRL_COMMON1 bits */
#define VBUSVLDEXTSEL0				BIT(4)
#define PLLBTUNE				BIT(5)

/* USB2_PHY_USB_PHY_HS_PHY_CTRL_COMMON2 bits */
#define VREGBYPASS				BIT(0)

/* USB2_PHY_USB_PHY_HS_PHY_CTRL1 bits */
#define VBUSVLDEXT0				BIT(0)

/* USB2_PHY_USB_PHY_HS_PHY_CTRL2 bits */
#define USB2_SUSPEND_N				BIT(2)
#define USB2_SUSPEND_N_SEL			BIT(3)

/* USB2_PHY_USB_PHY_HS_PHY_OVERRIDE_X1 bits */
#define TXPREEMPAMPTUNE0_MASK			GENMASK(7, 6)

/* USB2_PHY_USB_PHY_HS_PHY_TEST0 bits */
#define TESTDATAIN_MASK				GENMASK(7, 0)

/* USB2_PHY_USB_PHY_TEST1 bits */
#define TESTDATAOUTSEL				BIT(4)
#define TESTCLK					BIT(6)

/* USB2_PHY_USB_PHY_CFG0 bits */
#define UTMI_PHY_CMN_CTRL_OVERRIDE_EN		BIT(1)

/* USB2_PHY_USB_PHY_REFCLK_CTRL bits */
#define REFCLK_SEL_MASK				GENMASK(1, 0)
#define REFCLK_SEL_DEFAULT			(0x2 << 0)

/* USB2_PHY_USB_PHY_FSEL_SEL bits */
#define FSEL_SEL				BIT(0)

/* Special delay values */
#define DELAY_MIN_US				10
#define DELAY_MAX_US				20

static const char * const femto_vreg_names[] = {
	"vdda-pll",
	"vdda33",
	"vdda18",
};

#define FEMTO_NUM_VREGS		ARRAY_SIZE(femto_vreg_names)

/**
 * struct phy_reg_cfg - PHY register configuration entry
 * @offset: Register offset
 * @mask: Bit mask for the field
 * @value: Value to write
 * @delay_us: Delay in microseconds after write (0 = no delay)
 */
struct phy_reg_cfg {
	u32 offset;
	u32 mask;
	u32 value;
	u32 delay_us;
};

/**
 * struct phy_init_seq - PHY initialization sequence
 * @seq: Array of register configurations
 * @num_regs: Number of register configurations
 */
struct phy_init_seq {
	const struct phy_reg_cfg *seq;
	unsigned int num_regs;
};

/**
 * struct femto_phy_cfg - SoC-specific PHY configuration
 * @por_seq: Power-on-Reset sequence
 * @suspend_seq: Suspend sequence
 * @resume_seq: Resume sequence
 */
struct femto_phy_cfg {
	struct phy_init_seq por_seq;
	struct phy_init_seq suspend_seq;
	struct phy_init_seq resume_seq;
};

/**
 * struct femto_usb_hs_phy - Femto USB HS PHY attributes
 * @phy: generic PHY
 * @base: iomapped memory space for PHY registers
 * @cfg_ahb_clk: AHB configuration clock
 * @ref_clk: PHY reference clock
 * @phy_reset: PHY reset control
 * @vregs: regulator supplies
 * @cfg: SoC-specific configuration
 * @phy_initialized: PHY initialization status
 */
struct femto_usb_hs_phy {
	struct phy *phy;
	void __iomem *base;

	struct clk *cfg_ahb_clk;
	struct clk *ref_clk;
	struct reset_control *phy_reset;
	struct regulator_bulk_data vregs[FEMTO_NUM_VREGS];

	const struct femto_phy_cfg *cfg;
	bool phy_initialized;
};

/* IPQ9650 Power-on-Reset sequence (23 steps) */
static const struct phy_reg_cfg ipq9650_por_seq[] = {
	/* Step 1: Enable software override */
	{ USB2_PHY_USB_PHY_CFG0, UTMI_PHY_CMN_CTRL_OVERRIDE_EN,
	  UTMI_PHY_CMN_CTRL_OVERRIDE_EN, 0 },

	/* Step 2: Assert POR for at least 10us */
	{ USB2_PHY_USB_PHY_UTMI_CTRL5, POR, POR, DELAY_MIN_US },

	/* Step 3: Enable FSEL software override */
	{ USB2_PHY_USB_PHY_FSEL_SEL, FSEL_SEL, FSEL_SEL, 0 },

	/* Step 4: Set FSEL for 24 MHz reference clock */
	{ USB2_PHY_USB_PHY_HS_PHY_CTRL_COMMON0, FSEL_MASK, FSEL_24MHZ, 0 },

	/* Step 5: Set PLL bandwidth */
	{ USB2_PHY_USB_PHY_HS_PHY_CTRL_COMMON1, PLLBTUNE, PLLBTUNE, 0 },

	/* Step 6: Select CLKCORE as reference clock source */
	{ USB2_PHY_USB_PHY_REFCLK_CTRL, REFCLK_SEL_MASK, REFCLK_SEL_DEFAULT, 0 },

	/* Step 7: Wordinterface - hardware controlled (no register write) */

	/* Step 8: Enable external VBUS valid select */
	{ USB2_PHY_USB_PHY_HS_PHY_CTRL_COMMON1, VBUSVLDEXTSEL0, VBUSVLDEXTSEL0, 0 },

	/* Step 9: Set external VBUS valid */
	{ USB2_PHY_USB_PHY_HS_PHY_CTRL1, VBUSVLDEXT0, VBUSVLDEXT0, 0 },

	/* Step 10: Set TX preemphasis to 1X */
	{ USB2_PHY_USB_PHY_HS_PHY_OVERRIDE_X1, TXPREEMPAMPTUNE0_MASK, (0x1 << 6), 0 },

	/* Step 11: Bypass internal voltage regulator */
	{ USB2_PHY_USB_PHY_HS_PHY_CTRL_COMMON2, VREGBYPASS, VREGBYPASS, 0 },

	/* Step 12: Deassert ATE reset */
	{ USB2_PHY_USB_PHY_UTMI_CTRL5, ATERESET, 0, 0 },

	/* Step 13: Clear test data output select */
	{ USB2_PHY_USB_PHY_HS_PHY_TEST1, TESTDATAOUTSEL, 0, 0 },

	/* Step 14: Clear test clock */
	{ USB2_PHY_USB_PHY_HS_PHY_TEST1, TESTCLK, 0, 0 },

	/* Step 15: Clear VATE test enable */
	{ USB2_PHY_USB_PHY_HS_PHY_CTRL_COMMON0, VATESTENB_MASK, 0, 0 },

	/* Step 16: Clear test data input */
	{ USB2_PHY_USB_PHY_HS_PHY_TEST0, TESTDATAIN_MASK, 0, 0 },

	/* Step 17: Note - outputs indeterminate during reset (no register write) */

	/* Step 18: Enable suspend override select */
	{ USB2_PHY_USB_PHY_HS_PHY_CTRL2, USB2_SUSPEND_N_SEL, USB2_SUSPEND_N_SEL, 0 },

	/* Step 19: Set suspend signal active */
	{ USB2_PHY_USB_PHY_HS_PHY_CTRL2, USB2_SUSPEND_N, USB2_SUSPEND_N, 0 },

	/* Step 20: Set sleep mode inactive */
	{ USB2_PHY_USB_PHY_UTMI_CTRL0, SLEEPM, SLEEPM, 0 },

	/* Step 21: Release POR */
	{ USB2_PHY_USB_PHY_UTMI_CTRL5, POR, 0, 0 },

	/* Step 22: Disable suspend override */
	{ USB2_PHY_USB_PHY_HS_PHY_CTRL2, USB2_SUSPEND_N_SEL, 0, 0 },

	/* Step 23: Disable software override */
	{ USB2_PHY_USB_PHY_CFG0, UTMI_PHY_CMN_CTRL_OVERRIDE_EN, 0, 0 },
};

/* IPQ9650 Suspend sequence (3 steps) */
static const struct phy_reg_cfg ipq9650_suspend_seq[] = {
	/* Step 1: Enable suspend override select */
	{ USB2_PHY_USB_PHY_HS_PHY_CTRL2, USB2_SUSPEND_N_SEL, USB2_SUSPEND_N_SEL, 0 },

	/* Step 2: Enter suspend */
	{ USB2_PHY_USB_PHY_HS_PHY_CTRL2, USB2_SUSPEND_N, 0, 0 },

	/* Step 3: Set COMMONONN */
	{ USB2_PHY_USB_PHY_HS_PHY_CTRL_COMMON0, COMMONONN, COMMONONN, 0 },
};

/* IPQ9650 Resume sequence (3 steps) */
static const struct phy_reg_cfg ipq9650_resume_seq[] = {
	/* Step 1: Clear COMMONONN */
	{ USB2_PHY_USB_PHY_HS_PHY_CTRL_COMMON0, COMMONONN, 0, 0 },

	/* Step 2: Exit suspend */
	{ USB2_PHY_USB_PHY_HS_PHY_CTRL2, USB2_SUSPEND_N, USB2_SUSPEND_N, 0 },

	/* Step 3: Disable suspend override */
	{ USB2_PHY_USB_PHY_HS_PHY_CTRL2, USB2_SUSPEND_N_SEL, 0, 0 },
};

static const struct femto_phy_cfg ipq9650_phy_cfg = {
	.por_seq = {
		.seq = ipq9650_por_seq,
		.num_regs = ARRAY_SIZE(ipq9650_por_seq),
	},
	.suspend_seq = {
		.seq = ipq9650_suspend_seq,
		.num_regs = ARRAY_SIZE(ipq9650_suspend_seq),
	},
	.resume_seq = {
		.seq = ipq9650_resume_seq,
		.num_regs = ARRAY_SIZE(ipq9650_resume_seq),
	},
};

static inline void femto_phy_write_mask(void __iomem *base, u32 offset,
					 u32 mask, u32 val)
{
	u32 reg;

	reg = readl_relaxed(base + offset);
	reg &= ~mask;
	reg |= val & mask;
	writel_relaxed(reg, base + offset);

	/* Ensure write is completed */
	readl_relaxed(base + offset);
}

/**
 * femto_phy_apply_seq() - Apply a register configuration sequence
 * @hsphy: PHY instance
 * @seq: Initialization sequence to apply
 *
 * Return: 0 on success
 */
static int femto_phy_apply_seq(struct femto_usb_hs_phy *hsphy,
				const struct phy_init_seq *seq)
{
	unsigned int i;

	for (i = 0; i < seq->num_regs; i++) {
		const struct phy_reg_cfg *cfg = &seq->seq[i];

		femto_phy_write_mask(hsphy->base, cfg->offset,
				     cfg->mask, cfg->value);

		if (cfg->delay_us)
			usleep_range(cfg->delay_us, cfg->delay_us + 10);
	}

	return 0;
}

/**
 * femto_usb_hs_phy_init() - Initialize Femto USB HS PHY
 * @phy: generic PHY
 *
 * Implements the complete Power-on-Reset (POR) sequence using
 * table-based configuration.
 *
 * Return: 0 on success, negative error code on failure
 */
static int femto_usb_hs_phy_init(struct phy *phy)
{
	struct femto_usb_hs_phy *hsphy = phy_get_drvdata(phy);
	int ret;

	ret = regulator_bulk_enable(FEMTO_NUM_VREGS, hsphy->vregs);
	if (ret)
		return ret;

	ret = clk_prepare_enable(hsphy->cfg_ahb_clk);
	if (ret)
		goto disable_regulators;

	ret = clk_prepare_enable(hsphy->ref_clk);
	if (ret)
		goto disable_cfg_ahb_clk;

	ret = reset_control_assert(hsphy->phy_reset);
	if (ret)
		goto disable_ref_clk;

	usleep_range(100, 150);

	ret = reset_control_deassert(hsphy->phy_reset);
	if (ret)
		goto disable_ref_clk;

	/* Apply POR sequence from configuration table */
	ret = femto_phy_apply_seq(hsphy, &hsphy->cfg->por_seq);
	if (ret)
		goto disable_ref_clk;

	hsphy->phy_initialized = true;

	return 0;

disable_ref_clk:
	clk_disable_unprepare(hsphy->ref_clk);
disable_cfg_ahb_clk:
	clk_disable_unprepare(hsphy->cfg_ahb_clk);
disable_regulators:
	regulator_bulk_disable(FEMTO_NUM_VREGS, hsphy->vregs);

	return ret;
}

/**
 * femto_usb_hs_phy_exit() - Power down Femto USB HS PHY
 * @phy: generic PHY
 *
 * Return: 0 on success
 */
static int femto_usb_hs_phy_exit(struct phy *phy)
{
	struct femto_usb_hs_phy *hsphy = phy_get_drvdata(phy);

	reset_control_assert(hsphy->phy_reset);
	clk_disable_unprepare(hsphy->ref_clk);
	clk_disable_unprepare(hsphy->cfg_ahb_clk);
	regulator_bulk_disable(FEMTO_NUM_VREGS, hsphy->vregs);
	hsphy->phy_initialized = false;

	return 0;
}

static const struct phy_ops femto_usb_hs_phy_ops = {
	.init		= femto_usb_hs_phy_init,
	.exit		= femto_usb_hs_phy_exit,
	.owner		= THIS_MODULE,
};

/**
 * femto_usb_hs_phy_suspend() - Suspend Femto USB HS PHY
 * @hsphy: PHY instance
 *
 * Applies suspend sequence from configuration table.
 *
 * Return: 0 on success
 */
static int femto_usb_hs_phy_suspend(struct femto_usb_hs_phy *hsphy)
{
	if (!hsphy->phy_initialized)
		return 0;

	return femto_phy_apply_seq(hsphy, &hsphy->cfg->suspend_seq);
}

/**
 * femto_usb_hs_phy_resume() - Resume Femto USB HS PHY
 * @hsphy: PHY instance
 *
 * Applies resume sequence from configuration table.
 *
 * Return: 0 on success
 */
static int femto_usb_hs_phy_resume(struct femto_usb_hs_phy *hsphy)
{
	if (!hsphy->phy_initialized)
		return 0;

	return femto_phy_apply_seq(hsphy, &hsphy->cfg->resume_seq);
}

static int __maybe_unused femto_usb_hs_phy_runtime_suspend(struct device *dev)
{
	struct femto_usb_hs_phy *hsphy = dev_get_drvdata(dev);

	return femto_usb_hs_phy_suspend(hsphy);
}

static int __maybe_unused femto_usb_hs_phy_runtime_resume(struct device *dev)
{
	struct femto_usb_hs_phy *hsphy = dev_get_drvdata(dev);

	return femto_usb_hs_phy_resume(hsphy);
}

static const struct dev_pm_ops femto_usb_hs_phy_pm_ops = {
	SET_RUNTIME_PM_OPS(femto_usb_hs_phy_runtime_suspend,
			   femto_usb_hs_phy_runtime_resume, NULL)
};

static int femto_usb_hs_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct femto_usb_hs_phy *hsphy;
	struct phy_provider *phy_provider;
	int ret, i;

	hsphy = devm_kzalloc(dev, sizeof(*hsphy), GFP_KERNEL);
	if (!hsphy)
		return -ENOMEM;

	hsphy->cfg = of_device_get_match_data(dev);
	if (!hsphy->cfg)
		return -EINVAL;

	hsphy->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(hsphy->base))
		return PTR_ERR(hsphy->base);

	hsphy->cfg_ahb_clk = devm_clk_get(dev, "cfg_ahb");
	if (IS_ERR(hsphy->cfg_ahb_clk))
		return dev_err_probe(dev, PTR_ERR(hsphy->cfg_ahb_clk),
				     "failed to get cfg_ahb clock\n");

	hsphy->ref_clk = devm_clk_get(dev, "ref");
	if (IS_ERR(hsphy->ref_clk))
		return dev_err_probe(dev, PTR_ERR(hsphy->ref_clk),
				     "failed to get ref clock\n");

	hsphy->phy_reset = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(hsphy->phy_reset))
		return dev_err_probe(dev, PTR_ERR(hsphy->phy_reset),
				     "failed to get PHY reset\n");

	for (i = 0; i < FEMTO_NUM_VREGS; i++)
		hsphy->vregs[i].supply = femto_vreg_names[i];

	ret = devm_regulator_bulk_get(dev, FEMTO_NUM_VREGS, hsphy->vregs);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to get regulator supplies\n");

	hsphy->phy = devm_phy_create(dev, NULL, &femto_usb_hs_phy_ops);
	if (IS_ERR(hsphy->phy))
		return dev_err_probe(dev, PTR_ERR(hsphy->phy),
				     "failed to create PHY\n");

	phy_set_drvdata(hsphy->phy, hsphy);
	platform_set_drvdata(pdev, hsphy);

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	/*
	 * Prevent runtime PM from being ON by default. Users can enable
	 * it using power/control in sysfs.
	 */
	pm_runtime_forbid(dev);

	phy_provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	if (IS_ERR(phy_provider)) {
		pm_runtime_disable(dev);
		return PTR_ERR(phy_provider);
	}

	return 0;
}

static const struct of_device_id femto_usb_hs_phy_of_match[] = {
	{
		.compatible = "qcom,ipq9650-usb-hs-phy",
		.data = &ipq9650_phy_cfg,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, femto_usb_hs_phy_of_match);

static struct platform_driver femto_usb_hs_phy_driver = {
	.probe		= femto_usb_hs_phy_probe,
	.driver = {
		.name	= "qcom-femto-usb-hs-phy",
		.pm	= &femto_usb_hs_phy_pm_ops,
		.of_match_table = femto_usb_hs_phy_of_match,
	},
};

module_platform_driver(femto_usb_hs_phy_driver);

MODULE_DESCRIPTION("Qualcomm Femto USB HS PHY driver");
MODULE_LICENSE("GPL");
