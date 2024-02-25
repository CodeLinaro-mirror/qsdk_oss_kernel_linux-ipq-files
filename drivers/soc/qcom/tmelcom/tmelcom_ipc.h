/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#ifndef _TMELCOM_IPC_H_
#define _TMELCOM_IPC_H_

#define MAX_FUSE_ADDR_SIZE 8

struct fuse_payload {
	uint32_t fuse_addr;
	uint32_t lsb_val;
	uint32_t msb_val;
} __packed;

struct tmel_fuse_read_multiple_msg {
	uint32_t status;
	struct tmel_msg_param_type_buf_in_out fuse_read_data;
} __packed;

struct sram_add_vals {
	int32_t val1;
	int32_t val2;
	int32_t val3;
	int32_t val4;
	int32_t val5;
	int32_t result;
};

struct mbox_add_vals {
	int32_t val1;
	int32_t val2;
	int32_t result;
};

int tmelcom_fuse_list_read(struct fuse_payload *fuse, size_t size);
#endif /* _TMELCOM_IPC_H_ */
