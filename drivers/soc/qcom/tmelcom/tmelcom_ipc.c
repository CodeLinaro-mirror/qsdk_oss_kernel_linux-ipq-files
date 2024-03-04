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
#include <linux/tmelcom_ipc.h>

#include "tmelcom.h"
#include "tmelcom_message_uids.h"

int tmelcom_fuse_list_read(struct tmel_fuse_payload *fuse, size_t size)
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
	msg.fuse_read_data.buf = (u32)dma_fuse;
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

int tmelcom_secboot_sec_auth(u32 sw_id, void *metadata, size_t size)
{
	struct device *dev = tmelcom_get_device();
	struct tmel_secboot_sec_auth msg = {0};
	dma_addr_t elf_buf_phys;
	void *elf_buf;
	int ret;

	elf_buf = dma_alloc_coherent(dev, size, &elf_buf_phys, GFP_KERNEL);
	if (!elf_buf)
		return -ENOMEM;

	memcpy(elf_buf, metadata, size);

	msg.req.sw_id = sw_id;
	msg.req.elf_buf.buf = (u32)elf_buf_phys;
	msg.req.elf_buf.buf_len = (u32)size;

	ret = tmelcom_process_request(TMEL_MSG_UID_SECBOOT_SEC_AUTH, &msg,
				      sizeof(msg));
	if (ret) {
		pr_err("Failed to send IPC: %d\n", ret);
	} else if (msg.resp.status || msg.resp.extended_error) {
		pr_err("Failed with status: %d error: %d\n",
		       msg.resp.status, msg.resp.extended_error);
		ret = msg.resp.status;
	}

	dma_free_coherent(dev, size, elf_buf, elf_buf_phys);

	return ret;
}
EXPORT_SYMBOL_GPL(tmelcom_secboot_sec_auth);

int tmelcom_secboot_teardown(u32 sw_id, u32 secondary_sw_id)
{
	struct tmel_secboot_teardown msg = {0};
	int ret;

	msg.req.sw_id = sw_id;
	msg.req.secondary_sw_id = secondary_sw_id;
	msg.resp.status = TMEL_ERROR_GENERIC;

	ret = tmelcom_process_request(TMEL_MSG_UID_SECBOOT_SS_TEAR_DOWN, &msg,
				      sizeof(msg));
	if (ret) {
		pr_err("Failed to send IPC: %d\n", ret);
	} else if (msg.resp.status) {
		pr_err("Failed with status: %d\n", msg.resp.status);
		ret = msg.resp.status;
	}

	return ret;
}

