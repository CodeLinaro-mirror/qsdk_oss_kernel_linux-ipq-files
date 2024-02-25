/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022,2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#ifndef _TMELCOM_H_
#define _TMELCOM_H_

#define HW_MBOX_SIZE		32
#define MBOX_QMP_CTRL_DATA_SIZE	4
#define MBOX_RESERVED_SIZE	4
#define MBOX_IPC_PACKET_SIZE	(HW_MBOX_SIZE - MBOX_QMP_CTRL_DATA_SIZE - MBOX_RESERVED_SIZE)
#define MBOX_IPC_MAX_PARAMS	5

#define MAX_PARAM_IN_PARAM_ID		14
#define PARAM_CNT_FOR_PARAM_TYPE_OUTBUF	3
#define SRAM_IPC_MAX_PARAMS		(MAX_PARAM_IN_PARAM_ID * PARAM_CNT_FOR_PARAM_TYPE_OUTBUF)
#define SRAM_IPC_MAX_BUF_SIZE		(SRAM_IPC_MAX_PARAMS * sizeof(uint32_t))

#define TMEL_SUCCESS                  (0x0U) ///<! Everything is going well
#define TMEL_ERROR_GENERIC            (0x1U) ///<! Generic failure code if error is unknown
#define TMEL_ERROR_NOT_SUPPORTED      (0x2U) ///<! The API or operation requested  not supported/implemented
#define TMEL_ERROR_BAD_PARAMETER      (0x3U) ///<! At least one of the parameters/settings is not correct
#define TMEL_ERROR_BAD_MESSAGE        (0x4U) ///<! Bad and/or malformed message
#define TMEL_ERROR_BAD_ADDRESS        (0x5U) ///<! NULL or out of range memory address
#define TMEL_ERROR_TMELCOM_FAILURE    (0x6U) ///<! Error from TMELCOM/communication layer
#define TMEL_ERROR_TMEL_BUSY          (0x7U) ///<! When TME busy & can't take a new request from client.

enum tmelcom_resp {
	/* Definitions of Successful Returns.
	 * Any positive values within 0 to 127.
	 */
	TMELCOM_RSP_SUCCESS  = 0,
	TMELCOM_RSP_SUCCESS_MAXVAL = 127,
	/* Definitions of Failure Returns.
	 * Any negative values within -32 to -128.
	 * Values from -1 to -31 are reserved for glink defined errors.
	 */
	TMELCOM_RSP_FAILURE = -32,
	TMELCOM_RSP_FAILURE_BAD_ADDR = -33,
	TMELCOM_RSP_FAILURE_INVALID_ARGS = -34,
	TMELCOM_RSP_FAILURE_CHANNEL_ERR = -35,
	TMELCOM_RSP_FAILURE_LINK_ERR = -36,
	TMELCOM_RSP_FAILURE_TX_ERR = -37,
	TMELCOM_RSP_FAILURE_RX_ERR = -38,
	TMELCOM_RSP_FAILURE_TIMEOUT = -39,
	TMELCOM_RSP_FAILURE_BUSY = -40,
	TMELCOM_RSP_FAILURE_INVALID_MESSAGE = -41,
	TMELCOM_RSP_SERVICE_API_RETURNED_ERR = -42,
	TMELCOM_RSP_FAILURE_NOT_SUPPORTED = -43,
	TMELCOM_RSP_FAILURE_MAX = -128,
};

enum ipc_type {
	IPC_MBOX_ONLY,
	IPC_MBOX_SRAM,
};

struct ipc_header {
	uint8_t ipc_type:1;
	uint8_t msg_len:7;
	uint8_t msg_type;
	uint8_t action_id;
	int8_t response;
} __packed;

struct mbox_payload {
	uint32_t param[MBOX_IPC_MAX_PARAMS];
};

struct sram_payload {
	uint32_t payload_ptr;
	uint32_t payload_len;
};

union ipc_payload {
	struct mbox_payload mbox_payload;
	struct sram_payload sram_payload;
} __packed;

struct tmel_ipc_pkt {
	struct ipc_header msg_hdr;
	union ipc_payload payload;
} __packed;

struct tmel_msg_param_type_buf_in {
	uint32_t buf;
	uint32_t buf_len;
};

struct tmel_msg_param_type_buf_out {
	uint32_t buf;
	uint32_t buf_len;
	uint32_t out_buf_len;
};

struct tmel_msg_param_type_buf_in_out {
	uint32_t buf;
	uint32_t buf_len;
	uint32_t out_buf_len;
};

enum tmelcom_resp tmelcom_process_request(uint32_t msg_uid, void *msg_buf,
					  size_t msg_size);

struct device *tmelcom_get_device(void);
#endif  /*_TMELCOM_H_ */
