/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022,2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#ifndef _TMELCOM_H_
#define _TMELCOM_H_

#define MBOX_MAX_MSG_LEN	1024

int tmelcom_process_request(const void *reqbuf, size_t reqsize, void *respbuf,
		size_t *respsize);
#endif  /*_TMELCOM_H_ */
