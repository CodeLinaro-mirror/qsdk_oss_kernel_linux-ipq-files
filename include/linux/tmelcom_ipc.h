/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#ifndef _TMELCOM_IPC_H_
#define _TMELCOM_IPC_H_

#define TMEL_MAX_FUSE_ADDR_SIZE 8

struct tmel_msg_param_type_buf_in {
	u32 buf;
	u32 buf_len;
};

struct tmel_msg_param_type_buf_out {
	u32 buf;
	u32 buf_len;
	u32 out_buf_len;
};

struct tmel_msg_param_type_buf_in_out {
	u32 buf;
	u32 buf_len;
	u32 out_buf_len;
};

struct tmel_fuse_payload {
	u32 fuse_addr;
	u32 lsb_val;
	u32 msb_val;
} __packed;

struct tmel_fuse_read_multiple_msg {
	u32 status;
	struct tmel_msg_param_type_buf_in_out fuse_read_data;
} __packed;

int tmelcom_fuse_list_read(struct tmel_fuse_payload *fuse, size_t size);
#endif /* _TMELCOM_IPC_H_ */
