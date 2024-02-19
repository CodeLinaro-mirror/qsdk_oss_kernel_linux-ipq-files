// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt)	"tmelcom: [%s][%d]:" fmt, __func__, __LINE__

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/mailbox_client.h>
#include <linux/seq_file.h>
#include <linux/debugfs.h>
#include <linux/platform_device.h>
#include <linux/mailbox/qmp.h>
#include <linux/uaccess.h>
#include <linux/mailbox_controller.h>

#include "tmelcom.h"

struct tmelcom {
	struct device *dev;
	struct mbox_client cl;
	struct mbox_chan *chan;
	struct mutex lock;
	struct qmp_pkt pkt;
	wait_queue_head_t waitq;
	void *txbuf;
	bool rx_done;
};

static struct tmelcom *tmeldev;

/**
 * tmelcom_msg_hdr - Request/Response message header between HLOS and TME.
 *
 * This header is proceeding any request specific parameters.
 * The transaction id is used to match request with response.
 *
 * Note: glink/QMP layer provides the rx/tx data size, so user payload size
 * is calculated by reducing the header size.
 */
struct tmelcom_msg_hdr {
	unsigned int reserved; /* for future use */
	unsigned int txnid;    /* transaction id */
} __packed;
#define TMELCOM_TX_HDR_SIZE sizeof(struct tmelcom_msg_hdr)
#define CBOR_NUM_BYTES (sizeof(unsigned int))
#define TMELCOM_RX_HDR_SIZE (TMELCOM_TX_HDR_SIZE + CBOR_NUM_BYTES)

/*
 * CBOR encode emulation
 * Prepend tmelcom_msg_hdr space
 * CBOR tag is prepended in request
 */
static inline size_t tmelcom_encode(struct tmelcom *tdev, const void *reqbuf,
		size_t size)
{
	unsigned int *msg = tdev->txbuf + TMELCOM_TX_HDR_SIZE;
	unsigned int *src = (unsigned int *)reqbuf;

	memcpy(msg, src, size);
	return (size + TMELCOM_TX_HDR_SIZE);
}

/*
 * CBOR decode emulation
 * Strip tmelcom_msg_hdr & CBOR tag
 */
static inline size_t tmelcom_decode(struct tmelcom *tdev, void *respbuf)
{
	unsigned int *msg = tdev->pkt.data + TMELCOM_RX_HDR_SIZE;
	unsigned int *rbuf = (unsigned int *)respbuf;

	memcpy(rbuf, msg, (tdev->pkt.size - TMELCOM_RX_HDR_SIZE));
	return (tdev->pkt.size - TMELCOM_RX_HDR_SIZE);
}

static bool tmelcom_check_rx_done(struct tmelcom *tdev)
{
	return  tdev->rx_done;
}

int tmelcom_process_request(const void *reqbuf, size_t reqsize, void *respbuf,
		size_t *respsize)
{
	struct tmelcom *tdev = tmedev;
	long time_left = 0;
	int ret = 0;

	/*
	 * Check to handle if probe is not successful or not completed yet
	 */
	if (!tdev) {
		pr_err("%s: tmelcom dev is NULL\n", __func__);
		return -ENODEV;
	}

	if (!reqbuf || !reqsize || (reqsize > MBOX_MAX_MSG_LEN)) {
		dev_err(tdev->dev, "invalid reqbuf or reqsize\n");
		return -EINVAL;
	}

	if (!respbuf || !respsize || (*respsize > MBOX_MAX_MSG_LEN)) {
		dev_err(tdev->dev, "invalid respbuf or respsize\n");
		return -EINVAL;
	}

	mutex_lock(&tdev->lock);

	tdev->rx_done = false;
	tdev->pkt.size = tmelcom_encode(tdev, reqbuf, reqsize);
	/*
	 * Controller expects a 4 byte aligned buffer
	 */
	tdev->pkt.size = (tdev->pkt.size + 0x3) & ~0x3;
	tdev->pkt.data = tdev->txbuf;

	pr_debug("tmelcom encoded request size = %u\n", tdev->pkt.size);
	print_hex_dump_bytes("tmelcom sending bytes : ",
			DUMP_PREFIX_ADDRESS, tdev->pkt.data, tdev->pkt.size);

	if (mbox_send_message(tdev->chan, &tdev->pkt) < 0) {
		dev_err(tdev->dev, "failed to send qmp message\n");
		ret = -EAGAIN;
		goto err_exit;
	}

	time_left = wait_event_interruptible_timeout(tdev->waitq,
			tmelcom_check_rx_done(tdev), tdev->cl.tx_tout);

	if (!time_left) {
		dev_err(tdev->dev, "request timed out\n");
		ret = -ETIMEDOUT;
		goto err_exit;
	}

	dev_info(tdev->dev, "response received\n");

	pr_debug("tmelcom received size = %u\n", tdev->pkt.size);
	print_hex_dump_bytes("tmelcom received bytes : ",
			DUMP_PREFIX_ADDRESS, tdev->pkt.data, tdev->pkt.size);

	if (tdev->pkt.size <= TMELCOM_RX_HDR_SIZE) {
		dev_err(tdev->dev, "invalid pkt.size received\n");
		ret = -EPROTO;
		goto err_exit;
	}

	*respsize = tmelcom_decode(tdev, respbuf);

	tdev->rx_done = false;
	ret = 0;

err_exit:
	mutex_unlock(&tdev->lock);
	return ret;
}
EXPORT_SYMBOL(tmelcom_process_request);

static void tmelcom_receive_message(struct mbox_client *client, void *message)
{
	struct tmelcom *tdev = dev_get_drvdata(client->dev);
	struct qmp_pkt *pkt = NULL;

	if (!message) {
		dev_err(tdev->dev, "spurious message received\n");
		goto tmelcom_receive_end;
	}

	if (tdev->rx_done) {
		dev_err(tdev->dev, "tmelcom response pending\n");
		goto tmelcom_receive_end;
	}
	pkt = (struct qmp_pkt *)message;
	tdev->pkt.size = pkt->size;
	tdev->pkt.data = pkt->data;
	tdev->rx_done = true;
tmelcom_receive_end:
	wake_up_interruptible(&tdev->waitq);
}

static int tmelcom_probe(struct platform_device *pdev)
{
	struct tmelcom *tdev;
	const char *label;
	char name[32];

	tdev = devm_kzalloc(&pdev->dev, sizeof(*tdev), GFP_KERNEL);
	if (!tdev)
		return -ENOMEM;

	tdev->cl.dev = &pdev->dev;
	tdev->cl.tx_block = true;
	tdev->cl.tx_tout = 500;
	tdev->cl.knows_txdone = false;
	tdev->cl.rx_callback = tmelcom_receive_message;

	label = of_get_property(pdev->dev.of_node, "mbox-names", NULL);
	if (!label)
		return -EINVAL;
	snprintf(name, 32, "%s_send_message", label);

	tdev->chan = mbox_request_channel(&tdev->cl, 0);
	if (IS_ERR(tdev->chan)) {
		dev_err(&pdev->dev, "failed to get mbox channel\n");
		return PTR_ERR(tdev->chan);
	}

	mutex_init(&tdev->lock);

	if (tdev->chan) {
		tdev->txbuf =
			devm_kzalloc(&pdev->dev, MBOX_MAX_MSG_LEN, GFP_KERNEL);
		if (!tdev->txbuf) {
			dev_err(&pdev->dev, "message buffer alloc faile\n");
			return -ENOMEM;
		}
	}

	init_waitqueue_head(&tdev->waitq);

	tdev->rx_done = false;
	tdev->dev = &pdev->dev;
	dev_set_drvdata(&pdev->dev, tdev);

	tmedev = tdev;

	dev_info(&pdev->dev, "tmelcom probe success\n");
	return 0;
err:
	mbox_free_channel(tdev->chan);
	return -ENOMEM;
}

static int tmelcom_remove(struct platform_device *pdev)
{
	struct tmelcom *tdev = platform_get_drvdata(pdev);

	if (tdev->chan)
		mbox_free_channel(tdev->chan);

	dev_info(&pdev->dev, "tmelcom remove success\n");
	return 0;
}

static const struct of_device_id tmelcom_match_tbl[] = {
	{.compatible = "qcom,tmelcom-qmp-client"},
	{},
};

static struct platform_driver tmelcom_driver = {
	.probe = tmelcom_probe,
	.remove = tmelcom_remove,
	.driver = {
		.name = "tmelcom-qmp-client",
		.suppress_bind_attrs = true,
		.of_match_table = tmelcom_match_tbl,
	},
};
module_platform_driver(tmelcom_driver);

MODULE_DESCRIPTION("QCOM TME-LCom mailbox protocol client");
MODULE_LICENSE("GPL");
