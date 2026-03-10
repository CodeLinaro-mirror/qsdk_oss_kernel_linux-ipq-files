/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/mdio.h>
#include <linux/pcs/pcs-qce2204.h>
#include <linux/phylink.h>
#include <linux/property.h>
#include <linux/reset.h>

#include <dt-bindings/clock/qcom,qce2204-pcs.h>

/* MMD_PMAPMD registers */
#define CALIBRATION4				0x78
#define CALIBRATION_DONE			BIT(7)

#define CDR_CONTROL				0x20
#define SSC_FIXED_OFFSET			BIT(3)

#define UPHY_TXPI				0x166
#define UPHY_SSC_PU				BIT(7)

#define UPHY_MLDO				0x16d

#define UPHY_SLDO				0x16e

#define PLL_POWER_ON_AND_RESET                  0x1e0
#define PCS_ANA_SW_RESET                        BIT(6)

#define MODE_CONTROL				0x11b
#define MODE_CONTROL_SEL_MASK			GENMASK(12, 8)
#define MODE_CONTROL_XPCS			0x10
#define MODE_CONTROL_SGMII_PLUS			0x8
#define MODE_CONTROL_SGMII			0x4
#define MODE_CONTROL_SGMII_SEL_MASK		GENMASK(6, 4)
#define MODE_CONTROL_SGMII_PHY			1
#define MODE_CONTROL_SGMII_MAC			2

#define QP_USXG_OPTION3				0x182

#define PCS_CH0_CONFIG				0x120
#define PCS_CH0_ADPT_RESET			BIT(11)
#define PCS_CH0_AUTONEG_DIS			BIT(3)
#define PCS_CH0_SPEED_MASK			GENMASK(2, 1)
#define PCS_CH0_SPEED_1000			2
#define PCS_CH0_SPEED_100			1
#define PCS_CH0_SPEED_10			0

#define QP_USXG_RESET				0x18c
#define QP_USXG_SGMII_FUNC_RESET		BIT(4)
#define QP_USXG_P3_FUNC_RESET			BIT(3)
#define QP_USXG_P2_FUNC_RESET			BIT(2)
#define QP_USXG_P1_FUNC_RESET			BIT(1)
#define QP_USXG_P0_FUNC_RESET			BIT(0)

/* XPCS MMD3 registers */
#define XPCS_CONTROL2				0x7
#define XPCS_TYPE_MASK				GENMASK(3, 0)
#define XPCS_TYPE_BASER				0

#define XPCS_EEE_CONTROL			0x14
#define XPCS_EEE_CAPABILITY			BIT(6)

#define XPCS_KR_STS				0x20
#define XPCS_KR_LINK_STS			BIT(12)

#define XPCS_DIG_CTRL				0x8000
#define XPCS_SOFT_RESET				BIT(15)
#define XPCS_USXG_ADPT_RESET			BIT(10)
#define XPCS_USXG_EN				BIT(9)

#define XPCS_EEE_MODE_CONTROL			0x8006
#define XPCS_EEE_LCT_RES			GENMASK(11, 8)
#define XPCS_EEE_SIGN				BIT(6)
#define XPCS_EEE_LRX_EN				BIT(1)
#define XPCS_EEE_LTX_EN				BIT(0)

#define XPCS_EEE_TX_TIMER			0x8008
#define XPCS_EEE_TX_TIMER_TSL_RES		GENMASK(5, 0)

#define XPCS_EEE_RX_TIMER			0x8009
#define XPCS_EEE_RX_TIMER_RWR_RES		GENMASK(12, 8)
#define XPCS_EEE_RX_TIMER_100US_RES		GENMASK(7, 0)

#define XPCS_EEE_MODE_CONTROL1			0x800b
#define XPCS_EEE_TRANS_RX_LPI_EN		BIT(8)
#define XPCS_EEE_TRANS_LPI_EN			BIT(0)

/* XPCS MMD31 registers */
#define XPCS_MII_CTRL				0x0
#define XPCS_MII_AN_EN				BIT(12)
#define XPCS_DUPLEX_FULL			BIT(8)
#define XPCS_SPEED_MASK				(BIT(13) | BIT(6) | BIT(5))
#define XPCS_SPEED_10000			(BIT(13) | BIT(6))
#define XPCS_SPEED_5000				(BIT(13) | BIT(5))
#define XPCS_SPEED_2500				BIT(5)
#define XPCS_SPEED_1000				BIT(6)
#define XPCS_SPEED_100				BIT(13)
#define XPCS_SPEED_10				0

#define XPCS_MII_AN_CTRL			0x8001
#define XPCS_MII_BIT_CONTROL			BIT(8)
#define XPCS_TX_CONFIG				BIT(3)
#define XPCS_AN_INTR_EN				BIT(0)

#define XPCS_MII_AN_INTR_STS			0x8002
#define XPCS_USXG_AN_LINK_STS			BIT(14)
#define XPCS_USXG_AN_SPEED_MASK			GENMASK(12, 10)
#define XPCS_AN_INTR_CMPLT			BIT(0)
#define XPCS_USXG_AN_SPEED_10			0
#define XPCS_USXG_AN_SPEED_100			1
#define XPCS_USXG_AN_SPEED_1000			2
#define XPCS_USXG_AN_SPEED_2500			4
#define XPCS_USXG_AN_SPEED_5000			5
#define XPCS_USXG_AN_SPEED_10000		3

#define QCE2204_PCS_CLK_NUM			2

/* There are dedicated clock and reset for each PCS function
 * such as system, RX and TX.
 */
enum pcs_func_id {
	PCS_FUNC_SYS,
	PCS_FUNC_RX,
	PCS_FUNC_TX,
	XPCS_FUNC_XGMII_RX,
	XPCS_FUNC_XGMII_TX,
	PCS_FUNC_MAX
};

struct qce2204_raw_clk {
	struct clk_hw hw_clk;
	struct qce2204_pcs *qce2204;
};

struct qce2204_pcs {
	struct phylink_pcs pcs;
	struct mdio_device *mdiodev;
	struct clk *clks[PCS_FUNC_MAX];
	struct reset_control *rstcs[PCS_FUNC_MAX];
	struct reset_control *xpcs_rstc;
	struct qce2204_raw_clk raw_clk[QCE2204_PCS_TX_CLK + 1];
	phy_interface_t curr_mode;
};

#define phylink_pcs_to_qce2204(pl_pcs) \
	container_of((pl_pcs), struct qce2204_pcs, pcs)
#define qce2204_to_phylink_pcs(qce2204) (&(qce2204)->pcs)

const char *const pcs_func_name[PCS_FUNC_MAX] = {
	"sys", "rx", "tx", "xgmii_rx", "xgmii_tx"
};

static unsigned long qce2204_pcs_clk_recalc_rate(struct clk_hw *hw,
						 unsigned long parent_rate)
{
	struct qce2204_raw_clk *raw_clk = container_of(hw, struct qce2204_raw_clk,
						       hw_clk);
	struct qce2204_pcs *qce2204 = raw_clk->qce2204;

	switch (qce2204->curr_mode) {
	case PHY_INTERFACE_MODE_2500BASEX:
	case PHY_INTERFACE_MODE_10GBASER:
	case PHY_INTERFACE_MODE_USXGMII:
		return 312500000;
	default:
		return 125000000;
	}
}

static int qce2204_pcs_clk_determine_rate(struct clk_hw *hw,
					  struct clk_rate_request *req)
{
	switch (req->rate) {
	case 125000000:
	case 312500000:
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct clk_ops qce2204_pcs_clk_ops = {
	.recalc_rate = qce2204_pcs_clk_recalc_rate,
	.determine_rate = qce2204_pcs_clk_determine_rate,
};

static int qce2204_pcs_clocks_register(struct mdio_device *mdiodev)
{
	struct qce2204_pcs *qce2204 = mdiodev_get_drvdata(mdiodev);
	struct clk_hw_onecell_data *clk_data;
	struct device *dev = &mdiodev->dev;
	struct clk_init_data init = {};
	char name[64];
	int ret;

	clk_data = devm_kzalloc(dev,
				struct_size(clk_data, hws, QCE2204_PCS_CLK_NUM),
				GFP_KERNEL);
	if (!clk_data)
		return -ENOMEM;

	clk_data->num = QCE2204_PCS_CLK_NUM;

	/* Initialize raw_clk qce2204 pointers */
	qce2204->raw_clk[QCE2204_PCS_RX_CLK].qce2204 = qce2204;
	qce2204->raw_clk[QCE2204_PCS_TX_CLK].qce2204 = qce2204;

	snprintf(name, sizeof(name), "%s::pcs_rx", dev_name(dev));
	init.ops = &qce2204_pcs_clk_ops;
	init.name = name;
	qce2204->raw_clk[QCE2204_PCS_RX_CLK].hw_clk.init = &init;
	ret = devm_clk_hw_register(dev, &qce2204->raw_clk[QCE2204_PCS_RX_CLK].hw_clk);
	if (ret)
		return ret;

	snprintf(name, sizeof(name), "%s::pcs_tx", dev_name(dev));
	init.ops = &qce2204_pcs_clk_ops;
	init.name = name;
	qce2204->raw_clk[QCE2204_PCS_TX_CLK].hw_clk.init = &init;
	ret = devm_clk_hw_register(dev, &qce2204->raw_clk[QCE2204_PCS_TX_CLK].hw_clk);
	if (ret)
		return ret;


	clk_data->hws[QCE2204_PCS_RX_CLK] = &qce2204->raw_clk[QCE2204_PCS_RX_CLK].hw_clk;
	clk_data->hws[QCE2204_PCS_TX_CLK] = &qce2204->raw_clk[QCE2204_PCS_TX_CLK].hw_clk;

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, clk_data);
}

static void qce2204_pcs_get_state_10gbaser(struct qce2204_pcs *qce2204,
					   struct phylink_link_state *state)
{
	int ret;

	ret = mdiodev_c45_read(qce2204->mdiodev, MDIO_MMD_PCS, XPCS_KR_STS);
	if (ret) {
		state->link = 0;
		return;
	}

	state->link = !!(ret & XPCS_KR_LINK_STS);
	if (!state->link)
		return;

	state->speed = SPEED_10000;
	state->duplex = DUPLEX_FULL;
	state->pause |= MLO_PAUSE_TXRX_MASK;
}

static void qce2204_pcs_get_state_usxgmii(struct qce2204_pcs *qce2204,
					  struct phylink_link_state *state)
{
	int ret;

	ret = mdiodev_c45_read(qce2204->mdiodev, MDIO_MMD_VEND2, XPCS_MII_AN_INTR_STS);
	if (ret < 0) {
		state->link = 0;
		return;
	}

	state->link = !!(ret & XPCS_USXG_AN_LINK_STS);
	if (!state->link)
		return;

	switch (FIELD_GET(XPCS_USXG_AN_SPEED_MASK, ret)) {
	case XPCS_USXG_AN_SPEED_10000:
		state->speed = SPEED_10000;
		break;
	case XPCS_USXG_AN_SPEED_5000:
		state->speed = SPEED_5000;
		break;
	case XPCS_USXG_AN_SPEED_2500:
		state->speed = SPEED_2500;
		break;
	case XPCS_USXG_AN_SPEED_1000:
		state->speed = SPEED_1000;
		break;
	case XPCS_USXG_AN_SPEED_100:
		state->speed = SPEED_100;
		break;
	case XPCS_USXG_AN_SPEED_10:
		state->speed = SPEED_10;
		break;
	default:
		state->link = false;
		return;
	}

	state->duplex = DUPLEX_FULL;
}

static void qce2204_pcs_get_state(struct phylink_pcs *pcs,
				  struct phylink_link_state *state)
{
	struct qce2204_pcs *qce2204 = phylink_pcs_to_qce2204(pcs);

	switch (state->interface) {
	case PHY_INTERFACE_MODE_10GBASER:
		qce2204_pcs_get_state_10gbaser(qce2204, state);
		break;
	case PHY_INTERFACE_MODE_USXGMII:
		qce2204_pcs_get_state_usxgmii(qce2204, state);
		break;
	default:
		break;
	}

	dev_dbg_ratelimited(&qce2204->mdiodev->dev,
			    "mode=%s/%s/%s link=%u\n",
			    phy_modes(state->interface),
			    phy_speed_to_str(state->speed),
			    phy_duplex_to_str(state->duplex),
			    state->link);
}

static int qce2204_set_msldo(struct qce2204_pcs *qce2204, phy_interface_t ifmode)
{
	int ret, mldo, sldo;

	switch (ifmode) {
	case PHY_INTERFACE_MODE_SGMII:
		mldo = 0xC9;
		sldo = 0x7F2B;
		break;
	case PHY_INTERFACE_MODE_2500BASEX:
		mldo = 0xCB;
		sldo = 0x7F2B;
		break;
	case PHY_INTERFACE_MODE_10GBASER:
	case PHY_INTERFACE_MODE_USXGMII:
		mldo = 0xCD;
		sldo = 0x7F6D;
		break;
	default:
		return -EOPNOTSUPP;
	}

	ret = mdiodev_c45_write(qce2204->mdiodev, MDIO_MMD_PMAPMD, UPHY_MLDO, mldo);
	if (ret)
		return ret;

	return mdiodev_c45_write(qce2204->mdiodev, MDIO_MMD_PMAPMD, UPHY_SLDO, sldo);
}

static int qce2204_pcs_set_mode(struct qce2204_pcs *qce2204, phy_interface_t ifmode)
{
	unsigned int hw_ifmode, op3_mask = 0;
	bool set_sgmii_mac = false;
	int ret;

	ret = qce2204_set_msldo(qce2204, ifmode);
	if (ret) {
		dev_err(&qce2204->mdiodev->dev, "Failed to set msldo settings.\n");
		return ret;
	}

	ret = reset_control_assert(qce2204->xpcs_rstc);
	if (ret) {
		dev_err(&qce2204->mdiodev->dev, "Failed to assert XPCS reset.\n");
		return ret;
	}

	switch (ifmode) {
	case PHY_INTERFACE_MODE_SGMII:
		hw_ifmode = MODE_CONTROL_SGMII;
		set_sgmii_mac = true;
		break;
	case PHY_INTERFACE_MODE_2500BASEX:
		hw_ifmode = MODE_CONTROL_SGMII_PLUS;
		set_sgmii_mac = true;
		break;
	case PHY_INTERFACE_MODE_10GBASER:
		op3_mask = BIT(1);
		hw_ifmode = MODE_CONTROL_XPCS;
		break;
	case PHY_INTERFACE_MODE_USXGMII:
		op3_mask = GENMASK(4, 1);
		hw_ifmode = MODE_CONTROL_XPCS;
		break;
	default:
		return -EOPNOTSUPP;
	}

	/* Configure the interface mode of PCS */
	ret = mdiodev_c45_modify(qce2204->mdiodev, MDIO_MMD_PMAPMD, MODE_CONTROL,
				 MODE_CONTROL_SEL_MASK,
				 FIELD_PREP(MODE_CONTROL_SEL_MASK, hw_ifmode));
	if (ret) {
		dev_err(&qce2204->mdiodev->dev, "Failed to set mode control.\n");
		return ret;
	}

	/* Configure SGMII with MAC connected */
	if (set_sgmii_mac) {
		ret = mdiodev_c45_modify(qce2204->mdiodev, MDIO_MMD_PMAPMD, MODE_CONTROL,
					 MODE_CONTROL_SGMII_SEL_MASK,
					 FIELD_PREP(MODE_CONTROL_SGMII_SEL_MASK,
						    MODE_CONTROL_SGMII_MAC));
		if (ret) {
			dev_err(&qce2204->mdiodev->dev, "Failed to set sgmii mac mode.\n");
			return ret;
		}
	}

	/* qp option3 configuration */
	if (op3_mask) {
		ret = mdiodev_c45_modify(qce2204->mdiodev, MDIO_MMD_PMAPMD, QP_USXG_OPTION3,
					 op3_mask, op3_mask);
		if (ret) {
			dev_err(&qce2204->mdiodev->dev, "Failed to set option3 mask value.\n");
			return ret;
		}
	}

	return 0;
}

static int qce2204_pcs_config_sgmii(struct qce2204_pcs *qce2204,
				    unsigned int neg_mode,
				    phy_interface_t ifmode,
				    const unsigned long *advertising,
				    bool permit)
{
	int ret, val;

	ret = qce2204_pcs_set_mode(qce2204, ifmode);
	if (ret) {
		dev_err(&qce2204->mdiodev->dev, "Failed to set sgmii interface mode.\n");
		return ret;
	}

	val = neg_mode == PHYLINK_PCS_NEG_INBAND_ENABLED ? 0 : PCS_CH0_AUTONEG_DIS;
	return mdiodev_c45_modify(qce2204->mdiodev, MDIO_MMD_PMAPMD,
				  PCS_CH0_CONFIG,
				  PCS_CH0_AUTONEG_DIS,
				  val);
}

static int qce2204_do_calibration(struct mdio_device *mdio_dev)
{
	int ret;

	ret = mdiodev_c45_modify(mdio_dev, MDIO_MMD_PMAPMD,
				 PLL_POWER_ON_AND_RESET,
				 PCS_ANA_SW_RESET, 0);
	if (ret)
		return ret;

	fsleep(1000);
	ret = mdiodev_c45_modify(mdio_dev, MDIO_MMD_PMAPMD,
				 PLL_POWER_ON_AND_RESET,
				 PCS_ANA_SW_RESET, PCS_ANA_SW_RESET);
	if (ret)
		return ret;

	/* Wait calibration done */
	return read_poll_timeout(mdiodev_c45_read, ret,
				 (ret & CALIBRATION_DONE),
				 1000, 100000, true, mdio_dev,
				 MDIO_MMD_PMAPMD, CALIBRATION4);
}

static int qce2204_pcs_config_10g_mode(struct qce2204_pcs *qce2204,
				       unsigned int neg_mode,
				       phy_interface_t ifmode,
				       const unsigned long *advertising,
				       bool permit)
{
	int ret, val, i;

	ret = qce2204_pcs_set_mode(qce2204, ifmode);
	if (ret) {
		dev_err(&qce2204->mdiodev->dev, "Failed to set interface %s\n", phy_modes(ifmode));
		return ret;
	}

	/* Assert all reset controls */
	for (i = PCS_FUNC_RX; i <= XPCS_FUNC_XGMII_TX; i++) {
		ret = reset_control_assert(qce2204->rstcs[i]);
		if (ret) {
			dev_err(&qce2204->mdiodev->dev,
				"Failed to assert reset %s.\n", pcs_func_name[i]);
			return ret;
		}
	}

	/* Wait 1ms */
	usleep_range(1000, 1100);

	/* Deassert all reset controls */
	for (i = PCS_FUNC_RX; i <= XPCS_FUNC_XGMII_TX; i++) {
		ret = reset_control_deassert(qce2204->rstcs[i]);
		if (ret) {
			dev_err(&qce2204->mdiodev->dev,
				"Failed to deassert reset %s.\n", pcs_func_name[i]);
			return ret;
		}
	}

	/* Wait calibration */
	ret = read_poll_timeout(mdiodev_c45_read, val,
				(val & CALIBRATION_DONE),
				1000, 100000, true, qce2204->mdiodev,
				MDIO_MMD_PMAPMD, CALIBRATION4);
	if (ret) {
		dev_err(&qce2204->mdiodev->dev, "Calibration timeout!\n");
		return ret;
	}

	/* Open SSC clock */
	ret = mdiodev_c45_modify(qce2204->mdiodev, MDIO_MMD_PMAPMD, UPHY_TXPI,
				 UPHY_SSC_PU, UPHY_SSC_PU);
	if (ret)
		return ret;

	ret = mdiodev_c45_modify(qce2204->mdiodev, MDIO_MMD_PMAPMD, CDR_CONTROL,
				 SSC_FIXED_OFFSET, SSC_FIXED_OFFSET);
	if (ret)
		return ret;

	/* XPCS deassert */
	ret = reset_control_deassert(qce2204->xpcs_rstc);
	if (ret) {
		dev_err(&qce2204->mdiodev->dev, "Failed to deassert XPCS reset.\n");
		return ret;
	}

	/* Set BaseR mode */
	ret = mdiodev_c45_modify(qce2204->mdiodev, MDIO_MMD_PCS, XPCS_CONTROL2,
				 XPCS_TYPE_MASK,
				 FIELD_PREP(XPCS_TYPE_MASK, XPCS_TYPE_BASER));
	if (ret)
		return ret;

	/* Wait 10gr link up */
	ret = read_poll_timeout(mdiodev_c45_read, val,
				(val & XPCS_KR_LINK_STS),
				1000, 100000, true, qce2204->mdiodev,
				MDIO_MMD_PCS, XPCS_KR_STS);
	if (ret)
		dev_warn(&qce2204->mdiodev->dev, "10gr link up timeout!\n");

	if (ifmode == PHY_INTERFACE_MODE_USXGMII) {
		/* Enable USXGMII mode */
		ret = mdiodev_c45_modify(qce2204->mdiodev, MDIO_MMD_PCS, XPCS_DIG_CTRL,
					 XPCS_USXG_EN,
					 XPCS_USXG_EN);
		if (ret)
			return ret;

		/* Initialized at 10G Speed */
		ret = mdiodev_c45_modify(qce2204->mdiodev, MDIO_MMD_VEND2, XPCS_MII_CTRL,
					 XPCS_SPEED_MASK, XPCS_SPEED_10000 | XPCS_DUPLEX_FULL);
		if (ret) {
			dev_err(&qce2204->mdiodev->dev, "Failed to set XPCS speed as 10G.\n");
				return ret;
		}
	}

	/* XPCS software reset */
	ret = mdiodev_c45_modify(qce2204->mdiodev, MDIO_MMD_PCS,
				 XPCS_DIG_CTRL,
				 XPCS_SOFT_RESET,
				 XPCS_SOFT_RESET);
	if (ret)
		return ret;

	/* Wait XPCS software reset done */
	ret = read_poll_timeout(mdiodev_c45_read, val,
				!(val & XPCS_SOFT_RESET),
				1000, 100000, true, qce2204->mdiodev,
				MDIO_MMD_PCS, XPCS_DIG_CTRL);
	if (ret) {
		dev_err(&qce2204->mdiodev->dev, "XPCS software reset failed!\n");
		return ret;
	}

	if (ifmode == PHY_INTERFACE_MODE_USXGMII) {
		/* Enable XPCS auto-negotiation complete interrupt, using mii-8bit
		 * and TX configureation of MAC side for USXGMII.
		 */
		ret = mdiodev_c45_modify(qce2204->mdiodev, MDIO_MMD_VEND2,
					 XPCS_MII_AN_CTRL,
					 (XPCS_MII_BIT_CONTROL | XPCS_TX_CONFIG |
					 XPCS_AN_INTR_EN),
					 (XPCS_MII_BIT_CONTROL | XPCS_AN_INTR_EN));
		if (ret)
			return ret;

		/* Enable auto-negotiation capability for USXGMII */
		ret = mdiodev_c45_modify(qce2204->mdiodev, MDIO_MMD_VEND2,
					 XPCS_MII_CTRL,
					 XPCS_MII_AN_EN,
					 XPCS_MII_AN_EN);
		if (ret)
			return ret;

		/* Check EEE capability supported or not */
		ret = mdiodev_c45_read(qce2204->mdiodev, MDIO_MMD_PCS, XPCS_EEE_CONTROL);
		if (ret < 0)
			return ret;

		if (ret & XPCS_EEE_CAPABILITY) {
			ret = mdiodev_c45_modify(qce2204->mdiodev, MDIO_MMD_PCS,
						 XPCS_EEE_MODE_CONTROL,
						 XPCS_EEE_LCT_RES | XPCS_EEE_SIGN,
						 FIELD_PREP(XPCS_EEE_LCT_RES, 0x2));
			if (ret)
				return ret;

			ret = mdiodev_c45_modify(qce2204->mdiodev, MDIO_MMD_PCS,
						 XPCS_EEE_TX_TIMER,
						 XPCS_EEE_TX_TIMER_TSL_RES,
						 FIELD_PREP(XPCS_EEE_TX_TIMER_TSL_RES, 0x7));
			if (ret)
				return ret;

			ret = mdiodev_c45_modify(qce2204->mdiodev, MDIO_MMD_PCS,
						 XPCS_EEE_RX_TIMER,
						 XPCS_EEE_RX_TIMER_100US_RES,
						 FIELD_PREP(XPCS_EEE_RX_TIMER_100US_RES, 0xa6));
			if (ret)
				return ret;

			/* Enable EEE LPI */
			ret = mdiodev_c45_modify(qce2204->mdiodev, MDIO_MMD_PCS,
						 XPCS_EEE_MODE_CONTROL1,
						 XPCS_EEE_TRANS_LPI_EN | XPCS_EEE_TRANS_RX_LPI_EN,
						 XPCS_EEE_TRANS_LPI_EN | XPCS_EEE_TRANS_RX_LPI_EN);
			if (ret)
				return ret;

			/* Enable TX/RX LPI pattern */
			ret = mdiodev_c45_modify(qce2204->mdiodev, MDIO_MMD_PCS,
						 XPCS_EEE_MODE_CONTROL,
						 XPCS_EEE_LRX_EN | XPCS_EEE_LTX_EN,
						 XPCS_EEE_LRX_EN | XPCS_EEE_LTX_EN);
		}
	}

	return 0;
}

static int qce2204_pcs_config(struct phylink_pcs *pcs,
			      unsigned int neg_mode,
			      phy_interface_t ifmode,
			      const unsigned long *advertising,
			      bool permit)
{
	struct qce2204_pcs *qce2204 = phylink_pcs_to_qce2204(pcs);
	int ret;

	/* Check if the requested mode is the same as current mode */
	if (qce2204->curr_mode == ifmode) {
		dev_dbg(&qce2204->mdiodev->dev,
			"PCS mode %s already configured, skipping reconfiguration\n",
			phy_modes(ifmode));
		return 0;
	}

	switch (ifmode) {
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_2500BASEX:
		ret = qce2204_pcs_config_sgmii(qce2204, neg_mode, ifmode, advertising, permit);
		break;
	case PHY_INTERFACE_MODE_10GBASER:
	case PHY_INTERFACE_MODE_USXGMII:
		ret = qce2204_pcs_config_10g_mode(qce2204, neg_mode, ifmode, advertising, permit);
		break;
	default:
		return -EOPNOTSUPP;
	}

	/* Save current mode if configuration was successful */
	if (ret == 0) {
		qce2204->curr_mode = ifmode;
		dev_dbg(&qce2204->mdiodev->dev,
			"PCS mode configured to %s\n", phy_modes(ifmode));
	}

	return ret;
}

static int qce2204_pcs_adpt_reset(struct mdio_device *mdio_dev)
{
	int ret;

	ret = mdiodev_c45_modify(mdio_dev, MDIO_MMD_PMAPMD,
				 PCS_CH0_CONFIG, PCS_CH0_ADPT_RESET, 0);
	if (ret)
		return ret;

	fsleep(1000);

	return mdiodev_c45_modify(mdio_dev, MDIO_MMD_PMAPMD, PCS_CH0_CONFIG,
				  PCS_CH0_ADPT_RESET, PCS_CH0_ADPT_RESET);
}

static int qce2204_pcs_ipg_tune_reset(struct mdio_device *mdio_dev)
{
	int ret;

	ret = mdiodev_c45_modify(mdio_dev, MDIO_MMD_PMAPMD, QP_USXG_RESET,
				 QP_USXG_SGMII_FUNC_RESET, 0);
	if (ret)
		return ret;

	fsleep(1000);

	return mdiodev_c45_modify(mdio_dev, MDIO_MMD_PMAPMD, QP_USXG_RESET,
				  QP_USXG_SGMII_FUNC_RESET,
				  QP_USXG_SGMII_FUNC_RESET);
}

static int qce2204_pcs_link_up_sgmii(struct qce2204_pcs *qce2204,
				     unsigned int neg_mode,
				     phy_interface_t ifmode,
				     int speed, int duplex)
{
	u16 sgmii_config = 0;
	unsigned long rate;
	int ret, i;

	if (neg_mode != PHYLINK_PCS_NEG_INBAND_ENABLED) {
		switch (speed) {
		case SPEED_2500:
			rate = 312500000;
			sgmii_config = PCS_CH0_SPEED_1000;
			break;
		case SPEED_1000:
			rate = 125000000;
			sgmii_config = PCS_CH0_SPEED_1000;
			break;
		case SPEED_100:
			rate = 25000000;
			sgmii_config = PCS_CH0_SPEED_100;
			break;
		case SPEED_10:
			rate = 2500000;
			sgmii_config = PCS_CH0_SPEED_10;
			break;
		case SPEED_UNKNOWN:
		default:
			dev_err(&qce2204->mdiodev->dev,
				"Invalid SGMII speed %d\n", speed);
			return -EINVAL;
		}
	}

	/* Configure auto-negotiation parameters */
	ret = mdiodev_c45_modify(qce2204->mdiodev, MDIO_MMD_PMAPMD,
				 PCS_CH0_CONFIG,
				 PCS_CH0_SPEED_MASK,
				 FIELD_PREP(PCS_CH0_SPEED_MASK, sgmii_config));
	if (ret)
		return ret;

	/* Assert reset for PCS RX and TX */
	for (i = PCS_FUNC_RX; i <= PCS_FUNC_TX; i++) {
		ret = reset_control_assert(qce2204->rstcs[i]);
		if (ret) {
			dev_err(&qce2204->mdiodev->dev,
				"Failed to assert reset %s.\n", pcs_func_name[i]);
			return ret;
		}
	}

	/* Wait 1ms */
	usleep_range(1000, 1100);

	/* Deassert reset for PCS RX and TX */
	for (i = PCS_FUNC_RX; i <= PCS_FUNC_TX; i++) {
		ret = reset_control_deassert(qce2204->rstcs[i]);
		if (ret) {
			dev_err(&qce2204->mdiodev->dev,
				"Failed to deassert reset %s.\n", pcs_func_name[i]);
			return ret;
		}
	}

	ret = qce2204_do_calibration(qce2204->mdiodev);
	if (ret) {
		dev_err(&qce2204->mdiodev->dev, "Calibration timeout!\n");
		return ret;
	}

	/* Set clock rate for PCS RX and TX */
	for (i = PCS_FUNC_RX; i <= PCS_FUNC_TX; i++) {
		ret = clk_set_rate(qce2204->clks[i], rate);
		if (ret) {
			dev_err(&qce2204->mdiodev->dev,
				"Failed to set clock rate for %s.\n", pcs_func_name[i]);
			return ret;
		}
	}

	/* Enable clocks for PCS RX and TX */
	for (i = PCS_FUNC_RX; i <= PCS_FUNC_TX; i++) {
		ret = clk_prepare_enable(qce2204->clks[i]);
		if (ret) {
			dev_err(&qce2204->mdiodev->dev,
				"Failed to enable clock %s.\n", pcs_func_name[i]);
			return ret;
		}
	}

	ret = qce2204_pcs_adpt_reset(qce2204->mdiodev);
	if (ret) {
		dev_err(&qce2204->mdiodev->dev, "Failed to reset PCS adapter.\n");
		return ret;
	}

	return qce2204_pcs_ipg_tune_reset(qce2204->mdiodev);
}

static int qce2204_pcs_link_up_10g_mode(struct qce2204_pcs *qce2204,
					unsigned int neg_mode,
					phy_interface_t ifmode,
					int speed, int duplex)
{
	int ret, val, xpcs_speed, i;
	unsigned long rate;

	switch (speed) {
	case SPEED_10000:
		xpcs_speed = XPCS_SPEED_10000;
		rate = 312500000;
		break;
	case SPEED_5000:
		xpcs_speed = XPCS_SPEED_5000;
		rate = 156250000;
		break;
	case SPEED_2500:
		xpcs_speed = XPCS_SPEED_2500;
		rate = 78125000;
		break;
	case SPEED_1000:
		xpcs_speed = XPCS_SPEED_1000;
		rate = 125000000;
		break;
	case SPEED_100:
		xpcs_speed = XPCS_SPEED_100;
		rate = 12500000;
		break;
	case SPEED_10:
		xpcs_speed = XPCS_SPEED_10;
		rate = 1250000;
		break;
	default:
		dev_err(&qce2204->mdiodev->dev, "Invalid USXGMII speed %d\n", speed);
		return -EINVAL;
	}

	if (ifmode == PHY_INTERFACE_MODE_USXGMII) {
		/* Wait XPCS auto-negotiation complete
		 * enable inband autoneg and pcs_get_state method will ensure
		 * XPCS autoneg compelete and link up, then autoneg interrupt is not needed.
		 */
		ret = read_poll_timeout(mdiodev_c45_read, val,
					(val & XPCS_AN_INTR_CMPLT),
					1000, 100000, true, qce2204->mdiodev,
					MDIO_MMD_VEND2, XPCS_MII_AN_INTR_STS);
		if (ret) {
			dev_err(&qce2204->mdiodev->dev, "XPCS auto-negotiation complete timeout.\n");
			return ret;
		}

		/* Clear XPCS auto-negotiation complete interrupt */
		ret = mdiodev_c45_modify(qce2204->mdiodev, MDIO_MMD_VEND2, XPCS_MII_AN_INTR_STS,
					 XPCS_AN_INTR_CMPLT, 0);
		if (ret) {
			dev_err(&qce2204->mdiodev->dev, "Failed to clear XPCS auto-negotiation complete interrupt.\n");
			return ret;
		}

		/* Set XPCS speed */
		ret = mdiodev_c45_modify(qce2204->mdiodev, MDIO_MMD_VEND2, XPCS_MII_CTRL,
					 XPCS_SPEED_MASK, xpcs_speed | XPCS_DUPLEX_FULL);
		if (ret) {
			dev_err(&qce2204->mdiodev->dev, "Failed to set XPCS speed.\n");
			return ret;
		}
	}

	/* Set MII interface clock rate */
	for (i = PCS_FUNC_RX; i <= XPCS_FUNC_XGMII_TX; i++) {
		ret = clk_set_rate(qce2204->clks[i], rate);
		if (ret) {
			dev_err(&qce2204->mdiodev->dev, "Failed to set clock rate for %d.\n", i);
			return ret;
		}
	}

	/* Enable MII interface clocks */
	for (i = PCS_FUNC_RX; i <= XPCS_FUNC_XGMII_TX; i++) {
		ret = clk_prepare_enable(qce2204->clks[i]);
		if (ret) {
			dev_err(&qce2204->mdiodev->dev, "Failed to enable clock %d.\n", i);
			return ret;
		}
	}

	/* XPCS adapter reset USXGMII */
	return mdiodev_c45_modify(qce2204->mdiodev, MDIO_MMD_PCS,
				  XPCS_DIG_CTRL,
				  XPCS_USXG_ADPT_RESET,
				  XPCS_USXG_ADPT_RESET);
}

static void qce2204_pcs_link_up(struct phylink_pcs *pcs, unsigned int neg_mode,
				phy_interface_t interface, int speed, int duplex)
{
	struct qce2204_pcs *qce2204 = phylink_pcs_to_qce2204(pcs);
	int ret;

	switch (interface) {
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_2500BASEX:
		ret = qce2204_pcs_link_up_sgmii(qce2204, neg_mode, interface, speed, duplex);
		break;
	case PHY_INTERFACE_MODE_10GBASER:
	case PHY_INTERFACE_MODE_USXGMII:
		ret = qce2204_pcs_link_up_10g_mode(qce2204, neg_mode, interface, speed, duplex);
		break;
	default:
		return;
	}

	if (ret)
		dev_err(&qce2204->mdiodev->dev,
			"PCS link up failed for interface %s\n", phy_modes(interface));
}

static const struct phylink_pcs_ops qce2204_pcs_phylink_ops = {
	.pcs_get_state = qce2204_pcs_get_state,
	.pcs_config = qce2204_pcs_config,
	.pcs_link_up = qce2204_pcs_link_up,
};

/**
 * qce2204_pcs_create_fwnode - Create PCS instance based on MDIO fwnode
 * @node: The fwnode of MDIO device
 *
 * The PCS device is registered as MDIO device, this function registers
 * the raw clock provider that supplies clocks to the PCS TX and RX
 * functions. The system clock of PCS is always configured as enabled.
 */
struct phylink_pcs *qce2204_pcs_create_fwnode(struct fwnode_handle *node)
{
	struct mdio_device *mdio_dev;
	struct qce2204_pcs *qce2204;

	if (!fwnode_device_is_available(node))
		return ERR_PTR(-ENODEV);

	mdio_dev = fwnode_mdio_find_device(node);
	if (!mdio_dev)
		return ERR_PTR(-EPROBE_DEFER);

	qce2204 = mdiodev_get_drvdata(mdio_dev);
	if (IS_ERR_OR_NULL(qce2204)) {
		mdio_device_put(mdio_dev);
		return ERR_CAST(qce2204);
	}

	return qce2204_to_phylink_pcs(qce2204);
}
EXPORT_SYMBOL_GPL(qce2204_pcs_create_fwnode);

/**
 * qce2204_pcs_destroy - Destroy PCS instance
 * @pcs: The PCS instance to be destroyed
 *
 * The clock rate of PCS RX and TX clocks are restored to the crystal
 * clock rate so that the raw clock provider can be unregistered.
 */
void qce2204_pcs_destroy(struct phylink_pcs *pcs)
{
	struct qce2204_pcs *qce2204 = phylink_pcs_to_qce2204(pcs);

	mdio_device_put(qce2204->mdiodev);
}
EXPORT_SYMBOL_GPL(qce2204_pcs_destroy);

static int qce2204_pcs_probe(struct mdio_device *mdio_dev)
{
	struct device *dev = &mdio_dev->dev;
	struct reset_control *rstc;
	struct qce2204_pcs *qce2204;
	struct clk *clk;
	const char *initial_mode_str;
	phy_interface_t initial_mode = PHY_INTERFACE_MODE_NA;
	int i, ret;

	qce2204 = devm_kzalloc(dev, sizeof(*qce2204), GFP_KERNEL);
	if (!qce2204)
		return -ENOMEM;

	/* Initialize curr_mode to NA */
	qce2204->curr_mode = PHY_INTERFACE_MODE_NA;

	for (i = 0; i < PCS_FUNC_MAX; i++) {
		clk = devm_clk_get(dev, pcs_func_name[i]);
		if (IS_ERR(clk)) {
			dev_err(dev, "Failed to get clock %s: %ld\n",
				pcs_func_name[i], PTR_ERR(clk));
			return PTR_ERR(clk);
		}

		qce2204->clks[i] = clk;

		rstc = devm_reset_control_get_exclusive(dev,
							pcs_func_name[i]);
		if (IS_ERR(rstc)) {
			dev_err(dev, "Failed to get reset control %s: %ld\n",
				pcs_func_name[i], PTR_ERR(rstc));
			return PTR_ERR(rstc);
		}

		qce2204->rstcs[i] = rstc;
	}

	/* Get XPCS reset control */
	qce2204->xpcs_rstc = devm_reset_control_get_exclusive(dev, "xpcs");
	if (IS_ERR(qce2204->xpcs_rstc)) {
		dev_err(dev, "Failed to get XPCS reset.\n");
		return PTR_ERR(qce2204->xpcs_rstc);
	}

	/* PCS system clock is always kept as enabled, then do reset
	 * on the PCS system.
	 */
	ret = clk_prepare_enable(qce2204->clks[PCS_FUNC_SYS]);
	if (ret) {
		dev_err(dev, "Failed to enable PCS system clock: %d\n", ret);
		return ret;
	}

	ret = reset_control_assert(qce2204->rstcs[PCS_FUNC_SYS]);
	if (ret) {
		dev_err(dev, "Failed to assert PCS system reset: %d\n", ret);
		return ret;
	}

	usleep_range(20000, 21000);

	ret = reset_control_deassert(qce2204->rstcs[PCS_FUNC_SYS]);
	if (ret) {
		dev_err(dev, "Failed to deassert PCS system reset: %d\n", ret);
		return ret;
	}

	mdiodev_set_drvdata(mdio_dev, qce2204);

	qce2204->mdiodev = mdio_dev;
	qce2204->pcs.ops = &qce2204_pcs_phylink_ops;
	qce2204->pcs.neg_mode = true;
	qce2204->pcs.poll = true;

	/* Parse and apply initial_mode property if present */
	ret = device_property_read_string(dev, "initial_mode", &initial_mode_str);
	if (ret == 0) {
		/* Convert string to phy_interface_t using case-insensitive comparison */
		if (!strcasecmp(initial_mode_str, phy_modes(PHY_INTERFACE_MODE_SGMII))) {
			initial_mode = PHY_INTERFACE_MODE_SGMII;
		} else if (!strcasecmp(initial_mode_str, phy_modes(PHY_INTERFACE_MODE_2500BASEX))) {
			initial_mode = PHY_INTERFACE_MODE_2500BASEX;
		} else if (!strcasecmp(initial_mode_str, phy_modes(PHY_INTERFACE_MODE_10GBASER))) {
			initial_mode = PHY_INTERFACE_MODE_10GBASER;
		} else if (!strcasecmp(initial_mode_str, phy_modes(PHY_INTERFACE_MODE_USXGMII))) {
			initial_mode = PHY_INTERFACE_MODE_USXGMII;
		} else {
			dev_warn(dev, "Unknown initial_mode '%s', skipping initialization\n",
				 initial_mode_str);
			initial_mode = PHY_INTERFACE_MODE_NA;
		}

		/* Initialize PCS with the specified mode */
		if (initial_mode != PHY_INTERFACE_MODE_NA) {
			ret = qce2204_pcs_config(&qce2204->pcs,
						 PHYLINK_PCS_NEG_NONE,
						 initial_mode,
						 NULL,
						 false);
			if (ret) {
				dev_err(dev, "Failed to initialize PCS with mode %s: %d\n",
					phy_modes(initial_mode), ret);
				return ret;
			}
			dev_info(dev, "PCS initialized with mode: %s\n", phy_modes(initial_mode));
		}
	}

	/* Register PCS raw clocks */
	ret = qce2204_pcs_clocks_register(mdio_dev);
	if (ret) {
		dev_err(dev, "Failed to register PCS clocks: %d\n", ret);
		return ret;
	}

	return 0;
}

static const struct of_device_id qce2204_pcs_match_table[] = {
	{ .compatible = "qcom,qce2204-pcs" },
	{ }
};
MODULE_DEVICE_TABLE(of, qce2204_pcs_match_table);

static struct mdio_driver qce2204_pcs_driver = {
	.mdiodrv.driver = {
		.name = "qcom,qce2204-pcs",
		.of_match_table	= qce2204_pcs_match_table,
	},
	.probe = qce2204_pcs_probe,
};

mdio_module_driver(qce2204_pcs_driver);

MODULE_DESCRIPTION("Qualcomm QCE2204 PCS Driver");
MODULE_LICENSE("GPL");
