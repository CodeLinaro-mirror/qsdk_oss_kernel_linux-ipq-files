/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef _DT_BINDINGS_CLK_QCOM_IPQ5610_CMN_PLL_H
#define _DT_BINDINGS_CLK_QCOM_IPQ5610_CMN_PLL_H

/* Parent clock */
#define IPQ5610_CMN_PLL_CLK		0

/* Fixed-rate clocks */
#define IPQ5610_XO_24MHZ_CLK		1
#define IPQ5610_SLEEP_32KHZ_CLK		2

/* Configurable divider clocks */
#define IPQ5610_NSS_CLK			3
#define IPQ5610_PPE_CLK			4
#define IPQ5610_PON_CLK			5
#define IPQ5610_EPHY_RAW_CLK		6

/* Gate clocks */
#define IPQ5610_PCS_31P25MHZ_CLK	7
#define IPQ5610_ETH0_50MHZ_CLK		8
#define IPQ5610_ETH1_50MHZ_CLK		9
#define IPQ5610_ETH2_50MHZ_CLK		10
#define IPQ5610_EPHY_50MHZ_CLK		11
#define IPQ5610_ETH_25MHZ_CLK		12

#endif
