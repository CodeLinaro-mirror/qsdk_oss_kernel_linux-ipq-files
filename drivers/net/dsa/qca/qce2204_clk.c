/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <linux/clk.h>
#include <linux/reset.h>
#include <linux/gpio/consumer.h>
#include <linux/of.h>
#include <linux/delay.h>

#include "qce2204.h"

/* Switch-level clock management */
int qce2204_get_clocks(struct qce2204_priv *priv)
{
	struct device *dev = priv->dev;
	int ret;

	priv->core_clk = devm_clk_get(dev, QCE2204_CLK_CORE);
	if (IS_ERR(priv->core_clk)) {
		ret = PTR_ERR(priv->core_clk);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Failed to get core clock: %d\n", ret);
		return ret;
	}

	priv->ipe_clk = devm_clk_get(dev, QCE2204_CLK_IPE);
	if (IS_ERR(priv->ipe_clk)) {
		ret = PTR_ERR(priv->ipe_clk);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Failed to get IPE clock: %d\n", ret);
		return ret;
	}

	priv->btq_clk = devm_clk_get(dev, QCE2204_CLK_BTQ);
	if (IS_ERR(priv->btq_clk)) {
		ret = PTR_ERR(priv->btq_clk);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Failed to get BTQ clock: %d\n", ret);
		return ret;
	}

	priv->cfg_clk = devm_clk_get(dev, QCE2204_CLK_CFG);
	if (IS_ERR(priv->cfg_clk)) {
		ret = PTR_ERR(priv->cfg_clk);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Failed to get CFG clock: %d\n", ret);
		return ret;
	}

	priv->apb_clk = devm_clk_get(dev, QCE2204_CLK_APB);
	if (IS_ERR(priv->apb_clk)) {
		ret = PTR_ERR(priv->apb_clk);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Failed to get APB clock: %d\n", ret);
		return ret;
	}

	dev_info(dev, "Switch clocks acquired\n");
	return 0;
}

int qce2204_enable_clocks(struct qce2204_priv *priv)
{
	struct device *dev = priv->dev;
	int ret;

	ret = clk_prepare_enable(priv->core_clk);
	if (ret) {
		dev_err(dev, "Failed to enable core clock: %d\n", ret);
		return ret;
	}

	/* Set switch core clock rate to 250M from serdes1 */
	ret = clk_set_rate(priv->core_clk, 250000000);
	if (ret) {
		dev_err(dev, "Failed to set switch core clock rate to 250MHz: %d\n", ret);
		clk_disable_unprepare(priv->core_clk);
		return ret;
	}

	ret = clk_prepare_enable(priv->ipe_clk);
	if (ret) {
		dev_err(dev, "Failed to enable IPE clock: %d\n", ret);
		goto err_ipe;
	}

	ret = clk_prepare_enable(priv->btq_clk);
	if (ret) {
		dev_err(dev, "Failed to enable BTQ clock: %d\n", ret);
		goto err_btq;
	}

	ret = clk_prepare_enable(priv->cfg_clk);
	if (ret) {
		dev_err(dev, "Failed to enable CFG clock: %d\n", ret);
		goto err_cfg;
	}

	ret = clk_prepare_enable(priv->apb_clk);
	if (ret) {
		dev_err(dev, "Failed to enable APB clock: %d\n", ret);
		goto err_apb;
	}

	dev_info(dev, "Switch clocks enabled\n");
	return 0;

err_apb:
	clk_disable_unprepare(priv->cfg_clk);
err_cfg:
	clk_disable_unprepare(priv->btq_clk);
err_btq:
	clk_disable_unprepare(priv->ipe_clk);
err_ipe:
	clk_disable_unprepare(priv->core_clk);
	return ret;
}

void qce2204_disable_clocks(struct qce2204_priv *priv)
{
	clk_disable_unprepare(priv->apb_clk);
	clk_disable_unprepare(priv->cfg_clk);
	clk_disable_unprepare(priv->btq_clk);
	clk_disable_unprepare(priv->ipe_clk);
	clk_disable_unprepare(priv->core_clk);
	dev_info(priv->dev, "Switch clocks disabled\n");
}

/* Per-port clock management */
int qce2204_get_port_clocks(struct qce2204_priv *priv, int port,
			    struct device_node *port_np)
{
	struct device *dev = priv->dev;
	int ret;

	priv->port_clks[port].tx_clk = of_clk_get_by_name(port_np, QCE2204_PORT_CLK_TX);
	if (IS_ERR(priv->port_clks[port].tx_clk)) {
		ret = PTR_ERR(priv->port_clks[port].tx_clk);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Failed to get TX clock for port %d: %d\n", port, ret);
		return ret;
	}

	priv->port_clks[port].rx_clk = of_clk_get_by_name(port_np, QCE2204_PORT_CLK_RX);
	if (IS_ERR(priv->port_clks[port].rx_clk)) {
		ret = PTR_ERR(priv->port_clks[port].rx_clk);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Failed to get RX clock for port %d: %d\n", port, ret);
		clk_put(priv->port_clks[port].tx_clk);
		return ret;
	}

	dev_dbg(dev, "Port %d clocks acquired\n", port);
	return 0;
}

int qce2204_enable_port_clocks(struct qce2204_priv *priv, int port)
{
	struct device *dev = priv->dev;
	int ret;

	if (!priv->port_clks[port].tx_clk || !priv->port_clks[port].rx_clk)
		return 0;

	ret = clk_prepare_enable(priv->port_clks[port].tx_clk);
	if (ret) {
		dev_err(dev, "Failed to enable TX clock for port %d: %d\n", port, ret);
		return ret;
	}

	ret = clk_prepare_enable(priv->port_clks[port].rx_clk);
	if (ret) {
		dev_err(dev, "Failed to enable RX clock for port %d: %d\n", port, ret);
		clk_disable_unprepare(priv->port_clks[port].tx_clk);
		return ret;
	}

	dev_dbg(dev, "Port %d clocks enabled\n", port);
	return 0;
}

void qce2204_disable_port_clocks(struct qce2204_priv *priv, int port)
{
	if (!priv->port_clks[port].tx_clk || !priv->port_clks[port].rx_clk)
		return;

	clk_disable_unprepare(priv->port_clks[port].rx_clk);
	clk_disable_unprepare(priv->port_clks[port].tx_clk);
	dev_dbg(priv->dev, "Port %d clocks disabled\n", port);
}

/* Reset management */
int qce2204_get_resets(struct qce2204_priv *priv)
{
	struct device *dev = priv->dev;
	int ret;

	priv->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(priv->reset_gpio)) {
		ret = PTR_ERR(priv->reset_gpio);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Failed to get reset GPIO: %d\n", ret);
		return ret;
	}

	if (priv->reset_gpio)
		dev_info(dev, "Reset GPIO acquired\n");

	priv->core_reset = devm_reset_control_get(dev, QCE2204_RESET_CORE);
	if (IS_ERR(priv->core_reset)) {
		ret = PTR_ERR(priv->core_reset);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Failed to get core reset: %d\n", ret);
		return ret;
	}

	dev_info(dev, "Core reset acquired\n");
	return 0;
}

int qce2204_get_port_resets(struct qce2204_priv *priv, int port,
			    struct device_node *port_np)
{
	struct device *dev = priv->dev;
	int ret;

	priv->port_resets[port].tx_reset = of_reset_control_get_exclusive(port_np, QCE2204_PORT_RESET_TX);
	if (IS_ERR(priv->port_resets[port].tx_reset)) {
		ret = PTR_ERR(priv->port_resets[port].tx_reset);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Failed to get TX reset for port %d: %d\n", port, ret);
		return ret;
	}

	priv->port_resets[port].rx_reset = of_reset_control_get_exclusive(port_np, QCE2204_PORT_RESET_RX);
	if (IS_ERR(priv->port_resets[port].rx_reset)) {
		ret = PTR_ERR(priv->port_resets[port].rx_reset);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Failed to get RX reset for port %d: %d\n", port, ret);
		reset_control_put(priv->port_resets[port].tx_reset);
		return ret;
	}

	dev_dbg(dev, "Port %d resets acquired\n", port);
	return 0;
}

int qce2204_reset_switch(struct qce2204_priv *priv)
{
	struct device *dev = priv->dev;
	int ret;

	if (priv->reset_gpio) {
		gpiod_set_value_cansleep(priv->reset_gpio, 1);
		usleep_range(10000, 20000);
		gpiod_set_value_cansleep(priv->reset_gpio, 0);
		usleep_range(10000, 20000);
		dev_info(dev, "GPIO reset completed\n");
	}

	ret = reset_control_assert(priv->core_reset);
	if (ret) {
		dev_err(dev, "Failed to assert core reset: %d\n", ret);
		return ret;
	}

	usleep_range(1000, 2000);

	ret = reset_control_deassert(priv->core_reset);
	if (ret) {
		dev_err(dev, "Failed to deassert core reset: %d\n", ret);
		return ret;
	}

	usleep_range(10000, 20000);
	dev_info(dev, "Switch reset completed\n");
	return 0;
}

int qce2204_reset_port(struct qce2204_priv *priv, int port)
{
	struct device *dev = priv->dev;
	int ret;

	if (!priv->port_resets[port].tx_reset || !priv->port_resets[port].rx_reset)
		return 0;

	ret = reset_control_assert(priv->port_resets[port].tx_reset);
	if (ret) {
		dev_err(dev, "Failed to assert TX reset for port %d: %d\n", port, ret);
		return ret;
	}

	ret = reset_control_assert(priv->port_resets[port].rx_reset);
	if (ret) {
		dev_err(dev, "Failed to assert RX reset for port %d: %d\n", port, ret);
		reset_control_deassert(priv->port_resets[port].tx_reset);
		return ret;
	}

	usleep_range(1000, 2000);

	ret = reset_control_deassert(priv->port_resets[port].tx_reset);
	if (ret) {
		dev_err(dev, "Failed to deassert TX reset for port %d: %d\n", port, ret);
		return ret;
	}

	ret = reset_control_deassert(priv->port_resets[port].rx_reset);
	if (ret) {
		dev_err(dev, "Failed to deassert RX reset for port %d: %d\n", port, ret);
		return ret;
	}

	usleep_range(1000, 2000);
	dev_dbg(dev, "Port %d reset completed\n", port);
	return 0;
}

/* Unified switch-level initialization */
int qce2204_init_switch_clocks_resets(struct qce2204_priv *priv)
{
	struct device *dev = priv->dev;
	int ret;

	/* Get switch-level resets */
	ret = qce2204_get_resets(priv);
	if (ret) {
		dev_err(dev, "Failed to get switch resets: %d\n", ret);
		return ret;
	}

	/* Get switch-level clocks */
	ret = qce2204_get_clocks(priv);
	if (ret) {
		dev_err(dev, "Failed to get switch clocks: %d\n", ret);
		return ret;
	}

	/* Reset the switch */
	ret = qce2204_reset_switch(priv);
	if (ret) {
		dev_err(dev, "Failed to reset switch: %d\n", ret);
		qce2204_disable_clocks(priv);
		return ret;
	}

	/* Enable switch-level clocks */
	ret = qce2204_enable_clocks(priv);
	if (ret) {
		dev_err(dev, "Failed to enable switch clocks: %d\n", ret);
		return ret;
	}

	dev_info(dev, "Switch clocks and resets initialized\n");
	return 0;
}

/* Cleanup switch-level resources */
void qce2204_cleanup_switch_clocks_resets(struct qce2204_priv *priv)
{
	qce2204_disable_clocks(priv);
	dev_info(priv->dev, "Switch clocks and resets cleaned up\n");
}

/* Initialize all ports' clocks and resets */
int qce2204_init_port_clocks_resets(struct qce2204_priv *priv)
{
	struct device *dev = priv->dev;
	struct device_node *ports_np, *port_np;
	int port, ret;

	ports_np = of_get_child_by_name(dev->of_node, "ports");
	if (!ports_np) {
		dev_warn(dev, "No ports node found in device tree\n");
		return 0;
	}

	for_each_available_child_of_node(ports_np, port_np) {
		/* Get port number from reg property */
		ret = of_property_read_u32(port_np, "reg", &port);
		if (ret) {
			dev_err(dev, "Failed to get port number from node %pOFn: %d\n",
				port_np, ret);
			of_node_put(port_np);
			continue;
		}

		if (port >= QCE2204_NUM_PORTS) {
			dev_err(dev, "Invalid port number %d in device tree\n", port);
			of_node_put(port_np);
			continue;
		}

		/* Try to get per-port clocks */
		ret = qce2204_get_port_clocks(priv, port, port_np);
		if (ret && ret != -ENOENT && ret != -EPROBE_DEFER) {
			dev_warn(dev, "No clocks for port %d\n", port);
			ret = 0;
		} else if (ret == 0) {
			ret = qce2204_enable_port_clocks(priv, port);
			if (ret) {
				dev_err(dev, "Failed to enable clocks for port %d\n", port);
				of_node_put(port_np);
				of_node_put(ports_np);
				return ret;
			}
		}

		/* Try to get per-port resets */
		ret = qce2204_get_port_resets(priv, port, port_np);
		if (ret && ret != -ENOENT && ret != -EPROBE_DEFER) {
			dev_warn(dev, "No resets for port %d\n", port);
			ret = 0;
		} else if (ret == 0) {
			ret = qce2204_reset_port(priv, port);
			if (ret) {
				dev_err(dev, "Failed to reset port %d\n", port);
				of_node_put(port_np);
				of_node_put(ports_np);
				return ret;
			}
		}
	}

	of_node_put(ports_np);
	dev_info(dev, "Port clocks and resets initialized\n");
	return 0;
}

/* Cleanup all ports' clocks and resets */
void qce2204_cleanup_port_clocks_resets(struct qce2204_priv *priv)
{
	int port;

	for (port = 0; port < QCE2204_NUM_PORTS; port++) {
		qce2204_disable_port_clocks(priv, port);

		/* Put clock references */
		if (priv->port_clks[port].tx_clk && !IS_ERR(priv->port_clks[port].tx_clk))
			clk_put(priv->port_clks[port].tx_clk);
		if (priv->port_clks[port].rx_clk && !IS_ERR(priv->port_clks[port].rx_clk))
			clk_put(priv->port_clks[port].rx_clk);

		/* Put reset references */
		if (priv->port_resets[port].tx_reset && !IS_ERR(priv->port_resets[port].tx_reset))
			reset_control_put(priv->port_resets[port].tx_reset);
		if (priv->port_resets[port].rx_reset && !IS_ERR(priv->port_resets[port].rx_reset))
			reset_control_put(priv->port_resets[port].rx_reset);
	}

	dev_info(priv->dev, "Port clocks and resets cleaned up\n");
}

/* Update ahb clock rate */
int qce2204_ahb_clk_set_rate(struct qce2204_priv *priv, unsigned long rate)
{
	struct device *dev = priv->dev;
	int ret;

	priv->ahb_clk = devm_clk_get(dev, QCE2204_CLK_AHB);
	if (IS_ERR(priv->ahb_clk)) {
		ret = PTR_ERR(priv->ahb_clk);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Failed to get ahb clock: %d\n", ret);
		return ret;
	}

	ret = clk_set_rate(priv->ahb_clk, rate);
	if (ret < 0) {
		dev_err(dev, "Failed to set AHB clock rate to %lu Hz: %d\n", rate, ret);
		return ret;
	}

	dev_info(dev, "Set AHB clock rate to %lu Hz\n", rate);
	return 0;
}
