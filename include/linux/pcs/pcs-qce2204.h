/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef __LINUX_PCS_QCE2204_H
#define __LINUX_PCS_QCE2204_H

#include <linux/phylink.h>

/**
 * define P_XO_CLOCK_RATE - Clock frequency of crystal(External clock)
 *
 * The reference clock frequency of QCE2204 is fixed to 50000000 HZ,
 * which is used to restore the parent of PCS clocks to crystal clock
 * (External clock connected to QCE2204) by configuring the clock rate of
 * PCS to 50000000 HZ.
 */
#define P_XO_CLOCK_RATE		50000000

struct phylink_pcs *qce2204_pcs_create_fwnode(struct fwnode_handle *node);
void qce2204_pcs_destroy(struct phylink_pcs *pcs);

#endif /* __LINUX_PCS_QCE2204_H */
