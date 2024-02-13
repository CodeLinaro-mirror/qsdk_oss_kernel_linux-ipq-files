/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt)	"tmelcom_ipc: %s: %d:" fmt, __func__, __LINE__

#include <linux/delay.h>
#include <linux/dma-direction.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>

#include "tmelcom.h"
#include "tmelcom_ipc.h"
#include "tmelcom_message_uids.h"

int tmelcom_fuse_list_read(struct fuse_payload *fuse, size_t size)
{
	int ret;
	struct tmel_fuse_read_multiple_msg msg = {0};
	struct device *dev = tmelcom_get_device();
	dma_addr_t dma_fuse;

	if (!dev || !fuse || !size)
		return -EINVAL;

	dma_fuse = dma_map_single(dev, fuse, size, DMA_BIDIRECTIONAL);
	ret = dma_mapping_error(dev, dma_fuse);
	if (ret != 0) {
		pr_err("DMA Mapping Error : %d\n", ret);
		return -EINVAL;
	}

	pr_debug("dma_fuse: %pad size: %zu\n", &dma_fuse, size);

	msg.status = TMEL_ERROR_GENERIC;
	msg.fuse_read_data.buf = (uint32_t)dma_fuse;
	msg.fuse_read_data.buf_len = size;

	/*Send Fuse read row IPC call to TME*/
	ret = tmelcom_process_request(TMEL_MSG_UID_FUSE_READ_MULTIPLE_ROW,
				      &msg, sizeof(msg));

	dma_unmap_single(dev, dma_fuse, size, DMA_BIDIRECTIONAL);

	if (!ret)
		ret = msg.status;

	return ret;
}
EXPORT_SYMBOL_GPL(tmelcom_fuse_list_read);
