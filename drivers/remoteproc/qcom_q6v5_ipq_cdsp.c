// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * IPQ9650 CDSP (NSP) remoteproc driver.
 *
 * Owns the full CDSP lifecycle: PAS image load/auth/reset and the SMEM
 * DCVS/LPM power-management protocol handling, serialized under a single
 * lock covering every vdd_cx/vdd_mx regulator transition.
 *
 * The SMEM channel layout, DCVS KVP encoding, IPCC PING mailbox mechanism
 * and MPM 4-phase handshake are dictated by the NSP Q6 firmware and are not
 * altered by this driver.
 */

#include <linux/delay.h>
#include <linux/elf.h>
#include <linux/firmware.h>
#include <linux/firmware/qcom/qcom_pas.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/remoteproc.h>
#include <linux/soc/qcom/mdt_loader.h>
#include <linux/soc/qcom/smem.h>
#include <linux/workqueue.h>

#include "qcom_common.h"
#include "qcom_pil_info.h"
#include "qcom_q6v5.h"
#include "remoteproc_internal.h"

/* IPQ9650-CDSP-specific break-at-start debug hook */
#define TCSR_SPARE_REG0		0x1959000
#define TCSR_SPARE_REG0_SIZE	4

static int debug_q6;
module_param(debug_q6, int, 0644);

/* PAS image identity, fixed for this SoC */
#define CDSP_PAS_ID		18
#define CDSP_DTB_PAS_ID		0x25
#define CDSP_FIRMWARE_NAME	"cdsp.mbn"
#define CDSP_DTB_FIRMWARE_NAME	"cdsp_dtb.mbn"
#define CDSP_CRASH_REASON_SMEM	601
#define CDSP_SSCTL_ID		0x17
#define CDSP_SSR_NAME		"cdsp"

/* SMEM Protocol Definitions */
/* NSP Q6 SMEM host ID */
#define CDSP_SMEM_NSP_HOST_ID	5
/* CDSP DCVS SMEM item ID (driver-internal constant, not a DT property) */
#define CDSP_SMEM_ITEM		503
/* hdr(64) + request_area(1024) + response_area(256) */
#define CDSP_SMEM_SIZE		1344
/* Protocol magic number 'RPMH' */
#define CDSP_SMEM_MAGIC		0x52504D48
/* Protocol version v1.0 */
#define CDSP_SMEM_VERSION	0x00010001
#define CDSP_MSG_ID_REQUEST	0x01
#define CDSP_MSG_ID_RESPONSE	0x02
#define CDSP_RESP_MSG_SIZE	24
#define CDSP_MAX_KVP		64

/* SMEM layout fixed offsets (relative to start of SMEM item) */
#define CDSP_SMEM_REQUEST_OFFSET	64
#define CDSP_SMEM_REQUEST_SIZE		1024
#define CDSP_SMEM_RESPONSE_OFFSET	1088
#define CDSP_SMEM_RESPONSE_SIZE		256

/* NSP CX voltage rail resource ID */
#define CDSP_RESOURCE_ID_CX	0x03
/* NSP MX voltage rail resource ID */
#define CDSP_RESOURCE_ID_MX	0x04

/* HLVL corner values for the NSP CX/MX rails */
#define CDSP_HLVL_SVS		0
#define CDSP_HLVL_SVS_L1	1
#define CDSP_HLVL_NOM		2
#define CDSP_HLVL_TUR_L1	3
#define CDSP_HLVL_MAX		CDSP_HLVL_TUR_L1

/* Response status codes written to cdsp_smem_response.status */
#define STATUS_OK			0x00000000
#define STATUS_ERR_RAIL_ABSENT		0x00000001
/* Corner exceeds this board's rail capability */
#define STATUS_ERR_CORNER_LIMIT		0x00000002
/* Unrecognized resource key */
#define STATUS_ERR_UNKNOWN_KEY		0x00000003

/* MPM Register Offsets */
#define RSC_HDSHK_IRQ_STAT		0x0004
#define CLIENT_RSC_HDSHK(n)		(0x0010 + (n) * 0x10)
#define CLIENT_RSC_IRQ_STAT(n)		(0x0014 + (n) * 0x10)
#define CLIENT_RSC_IRQ_CLR(n)		(0x0018 + (n) * 0x10)
#define VDD_RAIL_ISO_CTRL(n)		(0x0330 + (n) * 0x4)

/* RSCC Register Offsets */
#define RSCC_BR_EVENT			0x0
#define RSCC_BR_EVENT_PC_MODE		BIT(3)

/* MPM Handshake Bits */
#define MPM_SHUTDOWN_REQ		BIT(0)
#define MPM_SHUTDOWN_ACK		BIT(1)
#define MPM_BRINGUP_REQ			BIT(2)
#define MPM_BRINGUP_ACK			BIT(3)

/* MPM Isolation Control Bits */
#define ISO_CLK_DIS			BIT(0)
#define ISO_CLK_DIS_ACK			BIT(1)
#define ISO_INPUT			BIT(8)
#define ISO_INPUT_DFT			BIT(9)
#define ISO_INPUT_CLKS			BIT(10)
#define ISO_CLAMP_MEM			BIT(11)
#define ISO_RET_0PIN			BIT(16)
#define ISO_SAVE_FF			BIT(17)
#define ISO_RESTORE_FF			BIT(18)
#define ISO_FREEZE_OUTPUT		BIT(24)
#define ISO_PWR_UP_RESET		BIT(25)

/* MXC rail mapped to isolation control register index 0 */
#define VDD_RAIL_MX			0
/* NSP/CX rail mapped to isolation control register index 1 */
#define VDD_RAIL_CX			1

/* Timeouts */
#define MPM_POLL_TIMEOUT_US		10000
#define MPM_POLL_SLEEP_US		10

/**
 * struct cdsp_smem_channel_hdr - SMEM channel header (64 bytes, at offset 0x000)
 * @magic:             Protocol magic number (0x52504D48 = 'RPMH')
 * @version:           Protocol version (0x00010001 = v1.0)
 * @cdsp_state:        NSP Q6 readiness flag; 0 = not ready, 1 = ready (written by NSP)
 * @apss_state:        APSS readiness flag; 0 = not ready, 1 = ready (written by APSS)
 * @request_offset:    Byte offset of the request area from the start of the SMEM item
 * @request_size:      Size of the request area in bytes
 * @response_offset:   Byte offset of the response area from the start of the SMEM item
 * @response_size:     Size of the response area in bytes
 * @request_in_flight: Channel busy flag; 0 = idle, 1 = busy (NSP sets, APSS clears)
 * @cdsp_to_apss_irq:  IPCC signal ID used for NSP-to-APSS interrupts
 * @apss_to_cdsp_irq:  IPCC signal ID used for APSS-to-NSP interrupts
 * @cdsp_tx_count:     Number of DCVS requests sent by NSP
 * @cdsp_rx_count:     Number of DCVS responses received by NSP
 * @apss_tx_count:     Number of DCVS responses sent by APSS
 * @apss_rx_count:     Number of DCVS requests received by APSS
 * @reserved:          Reserved, must be zero
 */
struct cdsp_smem_channel_hdr {
	u32 magic;
	u32 version;
	volatile u32 cdsp_state;
	volatile u32 apss_state;
	u32 request_offset;
	u32 request_size;
	u32 response_offset;
	u32 response_size;
	volatile u32 request_in_flight;
	u32 cdsp_to_apss_irq;
	u32 apss_to_cdsp_irq;
	u32 cdsp_tx_count;
	u32 cdsp_rx_count;
	u32 apss_tx_count;
	u32 apss_rx_count;
	u32 reserved;
} __packed;

/**
 * struct cdsp_kvp - Key-Value-Pair entry in a DCVS request
 * @key:    Resource identifier; upper 16 bits = CDSP_RESOURCE_ID_CX or _MX
 * @length: Length of the value field in bytes (always 4)
 * @value:  Requested HLVL corner integer (CDSP_HLVL_SVS..CDSP_HLVL_MAX)
 */
struct cdsp_kvp {
	u32 key;
	u32 length;
	u32 value;
} __packed;

/**
 * struct cdsp_smem_request - DCVS request message written by NSP Q6, read by APSS
 * @msg_size:     Total message size in bytes
 * @sequence:     Monotonically increasing sequence number
 * @msg_id:       Message type; CDSP_MSG_ID_REQUEST (0x01) for DCVS requests
 * @req_id:       Request identifier echoed in the response
 * @set:          Power set being requested (ACTIVE / SLEEP / WAKE)
 * @num_commands: Number of KVP entries that follow
 * @timestamp_us: Request timestamp in microseconds
 * @kvp:          Array of key-value-pair voltage commands (up to CDSP_MAX_KVP)
 */
struct cdsp_smem_request {
	u32 msg_size;
	u32 sequence;
	u32 msg_id;
	u32 req_id;
	u32 set;
	u32 num_commands;
	u32 timestamp_us;
	struct cdsp_kvp kvp[CDSP_MAX_KVP];
} __packed;

/**
 * struct cdsp_smem_response - DCVS response message written by APSS, read by NSP Q6
 * @msg_size:     Total message size in bytes (CDSP_RESP_MSG_SIZE = 24)
 * @sequence:     Echo of the request sequence number
 * @msg_id:       Message type; CDSP_MSG_ID_RESPONSE (0x02)
 * @status:       Result code
 * @data:         On success: the HLVL corner applied. On failure: the
 *                resource ID (CDSP_RESOURCE_ID_CX/_MX) that failed
 * @timestamp_us: Completion timestamp in microseconds
 */
struct cdsp_smem_response {
	u32 msg_size;
	u32 sequence;
	u32 msg_id;
	u32 status;
	u32 data;
	u32 timestamp_us;
} __packed;

/**
 * struct cdsp_smem_region - Full SMEM item layout (1344 bytes total)
 * @hdr:       Channel header at offset 0x000 (64 bytes)
 * @request:   DCVS request area at offset 0x040 (padded to 1024 bytes)
 * @_req_pad:  Padding to align response area to offset 0x440
 * @response:  DCVS response area at offset 0x440 (padded to 256 bytes)
 * @_resp_pad: Padding to complete the 256-byte response area
 */
struct cdsp_smem_region {
	struct cdsp_smem_channel_hdr hdr;
	struct cdsp_smem_request  request;
	u8 _req_pad[228];
	struct cdsp_smem_response response;
	u8 _resp_pad[232];
} __packed;

/**
 * struct cdsp_pas_priv - Merged CDSP PAS + power-management driver context
 * @dev: Device pointer
 * @rproc: Backing remoteproc instance
 * @q6v5: Common Q6V5 wdog/fatal/ready/stop-ack/handover IPC helper state
 * @glink_subdev, @smd_subdev, @ssr_subdev, @sysmon: Common remoteproc subdevices
 * @mem_phys, @mem_reloc, @mem_size, @mem_region: Main PAS memory-region mapping
 * @dtb_mem_phys, @dtb_mem_reloc, @dtb_mem_size, @dtb_mem_region: DTB memory-region mapping
 * @firmware, @dtb_firmware: Firmware handles held between .load and .start
 * @pas_ctx, @dtb_pas_ctx: PAS metadata contexts for the main/dtb images
 * @cx, @mx: Sole-owned NSP CX/MX voltage regulators
 * @smem: Pointer to the DCVS SMEM channel
 * @dcvs_mbox_client, @dcvs_mbox_chan: Mailbox client/channel for DCVS response IPCC PING
 * @dcvs_irq: DCVS request interrupt (NSP -> APSS IPCC PING)
 * @dcvs_work: Work item processing DCVS requests
 * @mpm_regmap: Regmap for MPM handshake/isolation registers
 * @rscc_regmap: Regmap for RSCC power-collapse-mode detection register
 * @lpm_irq: LPM request interrupt (MPM)
 * @lpm_wq, @lpm_work: Workqueue/work item processing LPM requests
 * @lock: Serialises PAS start/stop, LPM isolate/restore and DCVS voltage-set
 * @pas_active: True for the whole PAS start()->stop() window
 * @lpm_active_cx, @lpm_active_mx: True while LPM independently holds the cx/mx vote
 * @nsp_up: Defensive duplicate-IRQ / DCVS-drop tracking of LPM's last-known state
 */
struct cdsp_pas_priv {
	struct device *dev;
	struct rproc *rproc;
	struct qcom_q6v5 q6v5;

	struct qcom_rproc_glink glink_subdev;
	struct qcom_rproc_subdev smd_subdev;
	struct qcom_rproc_ssr ssr_subdev;
	struct qcom_sysmon *sysmon;

	phys_addr_t mem_phys;
	phys_addr_t mem_reloc;
	size_t mem_size;
	void *mem_region;

	phys_addr_t dtb_mem_phys;
	phys_addr_t dtb_mem_reloc;
	size_t dtb_mem_size;
	void *dtb_mem_region;

	const struct firmware *firmware;
	const struct firmware *dtb_firmware;
	struct qcom_pas_context pas_ctx;
	struct qcom_pas_context dtb_pas_ctx;

	struct regulator *cx;
	struct regulator *mx;

	struct cdsp_smem_region *smem;

	struct mbox_client dcvs_mbox_client;
	struct mbox_chan *dcvs_mbox_chan;
	int dcvs_irq;
	struct work_struct dcvs_work;

	struct regmap *mpm_regmap;
	struct regmap *rscc_regmap;
	int lpm_irq;
	struct workqueue_struct *lpm_wq;
	struct work_struct lpm_work;

	struct mutex lock;
	bool pas_active;
	bool lpm_active_cx;
	bool lpm_active_mx;
	bool nsp_up;
};

static const char *cdsp_hlvl_name(u32 corner)
{
	switch (corner) {
	case CDSP_HLVL_SVS:	return "SVS";
	case CDSP_HLVL_SVS_L1:	return "SVS_L1";
	case CDSP_HLVL_NOM:	return "NOM";
	case CDSP_HLVL_TUR_L1:	return "TUR_L1";
	default:		return "INVALID";
	}
}

/**
 * cdsp_is_full_pc_mode() - Check if FULL_PC mode is selected
 * @priv: Driver context
 *
 * Reads the CDSP RSCC_BR_EVENT register to determine power collapse mode.
 * Bit 3: 0 = FULL_PC (collapse both CX and MX)
 *        1 = LONG_APCR (collapse only CX)
 *
 * Return: true if FULL_PC mode, false if LONG_APCR mode
 */
static bool cdsp_is_full_pc_mode(struct cdsp_pas_priv *priv)
{
	u32 br_event;
	int ret;

	ret = regmap_read(priv->rscc_regmap, RSCC_BR_EVENT, &br_event);
	if (ret) {
		dev_err(priv->dev, "failed to read RSCC_BR_EVENT: %d\n", ret);
		return true; /* Default to FULL_PC on error */
	}

	return !(br_event & RSCC_BR_EVENT_PC_MODE);
}

/**
 * cdsp_dcvs_irq_handler() - DCVS interrupt handler
 *
 * Called when CDSP sends a DCVS request via IPCC PING interrupt.
 * Schedules work to process the request.
 */
static irqreturn_t cdsp_dcvs_irq_handler(int irq, void *data)
{
	struct cdsp_pas_priv *priv = data;

	schedule_work(&priv->dcvs_work);

	return IRQ_HANDLED;
}

/**
 * cdsp_dcvs_work_fn() - Process a DCVS corner scaling request from the NSP Q6
 *
 * Reads the KVP request from the SMEM channel, decodes each entry's HLVL
 * corner and resource ID, and applies it via regulator_set_voltage() (DCVS
 * never toggles a rail on/off, only its corner). Writes the response back
 * to SMEM and signals the NSP Q6 via the IPCC PING mailbox channel.
 */
static void cdsp_dcvs_work_fn(struct work_struct *work)
{
	struct cdsp_pas_priv *priv = container_of(work, struct cdsp_pas_priv, dcvs_work);
	struct cdsp_smem_region *smem = priv->smem;
	u32 sequence, num_commands;
	u32 resp_status = STATUS_OK;
	u32 resp_data = 0;
	int i;

	mutex_lock(&priv->lock);

	/* Drop DCVS requests while the NSP Q6 is isolated via LPM */
	if (!priv->nsp_up) {
		dev_warn(priv->dev, "DCVS request while powered off, dropping\n");
		mutex_unlock(&priv->lock);
		return;
	}

	rmb();
	sequence     = smem->request.sequence;
	num_commands = smem->request.num_commands;

	if (smem->request.msg_id != CDSP_MSG_ID_REQUEST) {
		dev_err(priv->dev, "unexpected msg_id: 0x%x (expected 0x%x)\n",
			smem->request.msg_id, CDSP_MSG_ID_REQUEST);
		resp_status = STATUS_ERR_UNKNOWN_KEY;
		goto send_response;
	}

	dev_dbg(priv->dev, "DCVS request: seq=%u, num_commands=%u, timestamp=%u us\n",
		sequence, num_commands, smem->request.timestamp_us);

	if (num_commands > CDSP_MAX_KVP) {
		dev_err(priv->dev, "too many KVP commands: %u (max %d)\n",
			num_commands, CDSP_MAX_KVP);
		resp_status = STATUS_ERR_UNKNOWN_KEY;
		goto send_response;
	}

	for (i = 0; i < num_commands; i++) {
		u32 key    = smem->request.kvp[i].key;
		u32 corner = smem->request.kvp[i].value;
		u32 resource_id = (key >> 16) & 0xFFFF;
		struct regulator *reg;
		const char *rail_name;
		int ret;

		dev_dbg(priv->dev,
			"DCVS req[%d/%u]: seq=%u key=0x%x resource_id=0x%x corner=%u (%s)\n",
			i, num_commands, sequence, key, resource_id, corner,
			cdsp_hlvl_name(corner));

		if (resource_id == CDSP_RESOURCE_ID_CX) {
			reg = priv->cx;
			rail_name = "CX";
		} else if (resource_id == CDSP_RESOURCE_ID_MX) {
			if (!priv->mx) {
				dev_info(priv->dev, "KVP[%d]: MX rail not present\n", i);
				if (resp_status == STATUS_OK) {
					resp_status = STATUS_ERR_RAIL_ABSENT;
					resp_data = CDSP_RESOURCE_ID_MX;
				}
				continue;
			}
			reg = priv->mx;
			rail_name = "MX";
		} else {
			dev_err(priv->dev, "KVP[%d]: unknown key 0x%x\n", i, key);
			resp_status = STATUS_ERR_UNKNOWN_KEY;
			resp_data = resource_id;
			goto send_response;
		}

		if (corner > CDSP_HLVL_MAX) {
			dev_err(priv->dev, "KVP[%d]: %s corner %u exceeds max %d\n",
				i, rail_name, corner, CDSP_HLVL_MAX);
			resp_status = STATUS_ERR_CORNER_LIMIT;
			resp_data = resource_id;
			goto send_response;
		}

		dev_dbg(priv->dev,
			"KVP[%d]: requesting %s -> HLVL %s (corner=%u), regulator mode selector=%u\n",
			i, rail_name, cdsp_hlvl_name(corner), corner, corner + 1);
		/*
		 * TUR_L1 mode for NSP_CX rail is not supported on GPIO regulator
		 * based RDPs. Hence query the regulator framework to check if
		 * the mode is supported before calling regulator_set_voltage()
		 */
		if (resource_id == CDSP_RESOURCE_ID_CX &&
		    !regulator_is_supported_voltage(reg, corner + 1, corner + 1)) {
			dev_err(priv->dev,
				"KVP[%d]: %s HLVL %s (corner=%u) not supported on this RDP\n",
				i, rail_name, cdsp_hlvl_name(corner), corner);
			resp_status = STATUS_ERR_CORNER_LIMIT;
			resp_data = resource_id;
			goto send_response;
		}

		ret = regulator_set_voltage(reg, corner + 1, corner + 1);
		if (ret) {
			dev_err(priv->dev, "KVP[%d]: failed to set %s corner %u (%s): %d\n",
				i, rail_name, corner, cdsp_hlvl_name(corner), ret);
			resp_status = STATUS_ERR_CORNER_LIMIT;
			resp_data = resource_id;
			goto send_response;
		}

		if (resp_status == STATUS_OK)
			resp_data = corner;

		dev_dbg(priv->dev, "KVP[%d]: DCVS applied -> %s now at HLVL %s (corner=%u)\n",
			i, rail_name, cdsp_hlvl_name(corner), corner);
	}

send_response:
	dev_dbg(priv->dev, "DCVS response: seq=%u status=%u data=%u\n",
		sequence, resp_status, resp_data);

	smem->response.msg_size     = CDSP_RESP_MSG_SIZE;
	smem->response.sequence     = sequence;
	smem->response.msg_id       = CDSP_MSG_ID_RESPONSE;
	smem->response.status       = resp_status;
	smem->response.data         = resp_data;
	smem->response.timestamp_us = ktime_to_us(ktime_get());

	smem->hdr.apss_rx_count++;
	smem->hdr.apss_tx_count++;
	smem->hdr.request_in_flight = 0;
	wmb();

	mbox_send_message(priv->dcvs_mbox_chan, NULL);
	mbox_client_txdone(priv->dcvs_mbox_chan, 0);

	mutex_unlock(&priv->lock);
}

/**
 * cdsp_execute_isolation_sequence() - MPM isolation bit-bang + regulator disable
 * @priv: Driver context
 * @full_pc: true to also isolate/disable the MX rail
 *
 * Processes NSP/CX rail first (always), then MXC/MX rail (FULL_PC mode
 * only). The regulator is disabled unconditionally on every cycle -- Q6
 * genuinely wants the rail powered down while isolated, cycling this many
 * times a second during normal runtime. cdsp_pas_start()/cdsp_pas_handover()
 * consult lpm_active_cx/lpm_active_mx before enabling/disabling the same
 * regulator themselves, so the two paths never double-toggle it.
 */
static void cdsp_execute_isolation_sequence(struct cdsp_pas_priv *priv, bool full_pc)
{
	int num_rails = full_pc ? 2 : 1;
	unsigned int iso_regs[] = {
		VDD_RAIL_ISO_CTRL(VDD_RAIL_CX),  /* NSP/CX rail - always processed */
		VDD_RAIL_ISO_CTRL(VDD_RAIL_MX),  /* MXC/MX rail - FULL_PC only */
	};
	struct regulator *regulators[] = { priv->cx, priv->mx };
	struct regmap *mpm_map = priv->mpm_regmap;
	int ret, i;
	u32 val;

	for (i = 0; i < num_rails; i++) {
		unsigned int iso = iso_regs[i];

		/* Step 1: Disable clocks */
		regmap_update_bits(mpm_map, iso, ISO_CLK_DIS, ISO_CLK_DIS);

		/* Poll CLK_DIS_ACK only for NSP/CX rail (MXC CLK_DIS_ACK is tied to 0) */
		if (i == 0) {
			ret = regmap_read_poll_timeout(mpm_map, iso, val,
						       (val & ISO_CLK_DIS_ACK),
						       MPM_POLL_SLEEP_US, MPM_POLL_TIMEOUT_US);
			if (ret)
				dev_err(priv->dev, "timeout waiting for NSP CLK_DIS_ACK\n");
		}

		/* Step 2: Isolate inputs */
		regmap_update_bits(mpm_map, iso,
				   ISO_INPUT | ISO_INPUT_CLKS | ISO_INPUT_DFT,
				   ISO_INPUT | ISO_INPUT_CLKS | ISO_INPUT_DFT);
		/* Step 3: Clamp memories */
		regmap_update_bits(mpm_map, iso, ISO_CLAMP_MEM, ISO_CLAMP_MEM);
		/* Step 4: Enable 0-pin retention */
		regmap_update_bits(mpm_map, iso, ISO_RET_0PIN, ISO_RET_0PIN);
		/* Step 5: Save to balloon latch (low-high-low pulse) */
		regmap_update_bits(mpm_map, iso, ISO_SAVE_FF, 0x0);
		regmap_update_bits(mpm_map, iso, ISO_SAVE_FF, ISO_SAVE_FF);
		regmap_update_bits(mpm_map, iso, ISO_SAVE_FF, 0x0);
		/* Step 6: Prepare restore signal (low-high) */
		regmap_update_bits(mpm_map, iso, ISO_RESTORE_FF, ISO_RESTORE_FF);
		/* Step 7: Freeze outputs */
		regmap_update_bits(mpm_map, iso, ISO_FREEZE_OUTPUT, ISO_FREEZE_OUTPUT);

		/* Step 8: Drop the LPM vote and disable the rail */
		if (i == 0)
			priv->lpm_active_cx = false;
		else
			priv->lpm_active_mx = false;

		if (regulators[i]) {
			ret = regulator_disable(regulators[i]);
			if (ret)
				dev_err(priv->dev, "failed to disable %s rail: %d\n",
					i == 0 ? "CX" : "MX", ret);
			else
				usleep_range(8000, 10000);
		}
	}

	dev_dbg(priv->dev, "isolation sequence complete (%s mode)\n",
		num_rails > 1 ? "FULL_PC" : "LONG_APCR");
}

/**
 * cdsp_execute_restoration_sequence() - MPM restoration bit-bang + regulator enable
 * @priv: Driver context
 * @full_pc: true to also restore/enable the MX rail
 *
 * FULL_PC: Restores MXC (MX) rail first, then NSP (CX) rail
 * LONG_APCR: Restores NSP (CX) rail only
 */
static void cdsp_execute_restoration_sequence(struct cdsp_pas_priv *priv, bool full_pc)
{
	int start_rail = full_pc ? 0 : 1;
	unsigned int iso_regs[] = {
		VDD_RAIL_ISO_CTRL(VDD_RAIL_MX),  /* MXC/MX rail - FULL_PC only, restored first */
		VDD_RAIL_ISO_CTRL(VDD_RAIL_CX),  /* NSP/CX rail - always restored */
	};
	struct regulator *regulators[] = { priv->mx, priv->cx };
	struct regmap *mpm_map = priv->mpm_regmap;
	int ret, i;

	for (i = start_rail; i < ARRAY_SIZE(iso_regs); i++) {
		unsigned int iso = iso_regs[i];
		bool is_mx = (i == 0);

		/* Step 1: Assert power-up reset */
		regmap_update_bits(mpm_map, iso, ISO_PWR_UP_RESET, ISO_PWR_UP_RESET);

		/* Step 2: Vote the rail on while reset is asserted */
		if (regulators[i]) {
			ret = regulator_enable(regulators[i]);
			if (ret)
				dev_err(priv->dev, "failed to enable %s: %d\n",
					is_mx ? "MX" : "CX", ret);
			else
				usleep_range(8000, 10000);
		}
		if (is_mx)
			priv->lpm_active_mx = true;
		else
			priv->lpm_active_cx = true;

		/* Step 3: Clear power-up reset */
		regmap_update_bits(mpm_map, iso, ISO_PWR_UP_RESET, 0);
		/* Step 4: Restore from balloon latch */
		regmap_update_bits(mpm_map, iso, ISO_RESTORE_FF, 0);
		/* Step 5: Remove memory clamp */
		regmap_update_bits(mpm_map, iso, ISO_CLAMP_MEM, 0);
		/* Step 6: Restore from 0-pin retention */
		regmap_update_bits(mpm_map, iso, ISO_RET_0PIN, 0);
		/* Step 7: Remove output freeze */
		regmap_update_bits(mpm_map, iso, ISO_FREEZE_OUTPUT, 0);
		/* Step 8: Remove input isolation */
		regmap_update_bits(mpm_map, iso, ISO_INPUT | ISO_INPUT_DFT | ISO_INPUT_CLKS, 0);
		/* Step 9: Enable clocks */
		regmap_update_bits(mpm_map, iso, ISO_CLK_DIS, 0);
	}

	dev_dbg(priv->dev, "restoration sequence complete (%s mode)\n",
		start_rail == 0 ? "FULL_PC" : "LONG_APCR");
}

/**
 * cdsp_lpm_shutdown_sequence() - Handle a power collapse request from CDSP
 * @priv: Driver context, called with priv->lock held
 *
 * Runs the isolation + regulator-drop sequence first, then completes the
 * MPM 4-phase handshake -- the lpm_active_cx/mx state is settled before the
 * handshake is acknowledged, not after.
 */
static void cdsp_lpm_shutdown_sequence(struct cdsp_pas_priv *priv)
{
	struct regmap *mpm_map = priv->mpm_regmap;
	bool is_full_pc = cdsp_is_full_pc_mode(priv);
	u32 hdshk_status;
	int ret;

	dev_dbg(priv->dev, "LPM: starting power collapse sequence\n");

	cdsp_execute_isolation_sequence(priv, is_full_pc);

	/* Phase 1: Acknowledge shutdown request */
	regmap_update_bits(mpm_map, CLIENT_RSC_HDSHK(0), MPM_SHUTDOWN_ACK, MPM_SHUTDOWN_ACK);

	/* Phase 2: Wait for request de-assertion */
	ret = regmap_read_poll_timeout(mpm_map, CLIENT_RSC_HDSHK(0), hdshk_status,
				       !(hdshk_status & MPM_SHUTDOWN_REQ),
				       MPM_POLL_SLEEP_US, MPM_POLL_TIMEOUT_US);
	if (ret) {
		dev_err(priv->dev, "timeout waiting for shutdown_req clear\n");
		return;
	}

	/* Phase 3: Clear acknowledgment */
	regmap_update_bits(mpm_map, CLIENT_RSC_HDSHK(0), MPM_SHUTDOWN_ACK, 0x0);

	priv->nsp_up = false;

	dev_dbg(priv->dev, "LPM: power collapse complete (%s mode)\n",
		is_full_pc ? "FULL_PC" : "LONG_APCR");
}

/**
 * cdsp_lpm_restore_sequence() - Handle a power restore request from CDSP
 * @priv: Driver context, called with priv->lock held
 */
static void cdsp_lpm_restore_sequence(struct cdsp_pas_priv *priv)
{
	struct regmap *mpm_map = priv->mpm_regmap;
	bool is_full_pc = cdsp_is_full_pc_mode(priv);
	u32 hdshk_status;
	int ret;

	dev_dbg(priv->dev, "LPM: starting power restore sequence\n");

	cdsp_execute_restoration_sequence(priv, is_full_pc);

	/* Assert BRINGUP_ACK */
	regmap_update_bits(mpm_map, CLIENT_RSC_HDSHK(0), MPM_BRINGUP_ACK, MPM_BRINGUP_ACK);

	/* Wait for BRINGUP_REQ to de-assert */
	ret = regmap_read_poll_timeout(mpm_map, CLIENT_RSC_HDSHK(0), hdshk_status,
				       !(hdshk_status & MPM_BRINGUP_REQ),
				       MPM_POLL_SLEEP_US, MPM_POLL_TIMEOUT_US);
	if (ret) {
		dev_err(priv->dev, "timeout waiting for BRINGUP_REQ clear\n");
		return;
	}

	/* Clear BRINGUP_ACK */
	regmap_update_bits(mpm_map, CLIENT_RSC_HDSHK(0), MPM_BRINGUP_ACK, 0);

	priv->nsp_up = true;

	dev_dbg(priv->dev, "LPM: power restore complete\n");
}

/**
 * cdsp_lpm_irq_handler() - LPM interrupt handler
 *
 * Called when CDSP sends an LPM request via MPM. Schedules work to process
 * the request.
 */
static irqreturn_t cdsp_lpm_irq_handler(int irq, void *data)
{
	struct cdsp_pas_priv *priv = data;
	unsigned int client_rsc_hdsk_irq_val;
	unsigned int rsc_hdsk_irq_val;

	regmap_read(priv->mpm_regmap, RSC_HDSHK_IRQ_STAT, &rsc_hdsk_irq_val);
	regmap_read(priv->mpm_regmap, CLIENT_RSC_IRQ_STAT(0), &client_rsc_hdsk_irq_val);
	if (!client_rsc_hdsk_irq_val || !rsc_hdsk_irq_val)
		return IRQ_NONE;

	queue_work(priv->lpm_wq, &priv->lpm_work);

	return IRQ_HANDLED;
}

/**
 * cdsp_lpm_work_fn() - Process an LPM request from CDSP
 *
 * Reads MPM handshake status and dispatches to the shutdown or restore
 * sequence.
 */
static void cdsp_lpm_work_fn(struct work_struct *work)
{
	struct cdsp_pas_priv *priv = container_of(work, struct cdsp_pas_priv, lpm_work);
	u32 hdshk_status;
	bool is_shutdown;

	synchronize_irq(priv->dcvs_irq);

	/*
	 * Ensure any pending DCVS vote is applied before entering power
	 * collapse. The Q6 sends its final voltage vote (IPCC PING) before
	 * asserting the LPM shutdown request (MPM). Since dcvs_work runs on
	 * the global system_wq and lpm_work runs on cdsp_lpm_wq there is no
	 * ordering guarantee between them. flush_work() must be called before
	 * taking priv->lock because dcvs_work_fn also acquires it.
	 */
	flush_work(&priv->dcvs_work);

	mutex_lock(&priv->lock);

	regmap_read(priv->mpm_regmap, CLIENT_RSC_HDSHK(0), &hdshk_status);

	regmap_write(priv->mpm_regmap, CLIENT_RSC_IRQ_CLR(0), 0x0);
	regmap_write(priv->mpm_regmap, CLIENT_RSC_IRQ_CLR(0), 0x1);
	regmap_write(priv->mpm_regmap, CLIENT_RSC_IRQ_CLR(0), 0x0);

	is_shutdown = !!(hdshk_status & MPM_SHUTDOWN_REQ);

	if (is_shutdown) {
		if (!priv->nsp_up) {
			dev_warn(priv->dev, "spurious shutdown request, already isolated\n");
			goto out_unlock;
		}
		cdsp_lpm_shutdown_sequence(priv);
	} else {
		if (priv->nsp_up) {
			dev_warn(priv->dev, "spurious bringup request, already restored\n");
			goto out_unlock;
		}
		cdsp_lpm_restore_sequence(priv);
	}

out_unlock:
	mutex_unlock(&priv->lock);
}

static const struct regmap_config cdsp_rscc_regmap_config = {
	.reg_bits       = 32,
	.reg_stride     = 4,
	.val_bits       = 32,
	.max_register   = 0x4,
	.fast_io        = true,
};

static const struct regmap_config cdsp_mpm_regmap_config = {
	.reg_bits       = 32,
	.reg_stride     = 4,
	.val_bits       = 32,
	.max_register   = 0x1000,
	.fast_io        = true,
};

/**
 * cdsp_alloc_memory_region() - Resolve the main and dtb memory-region reservations
 */
static int cdsp_alloc_memory_region(struct cdsp_pas_priv *priv)
{
	struct device *dev = priv->dev;
	struct device_node *node;
	struct reserved_mem *rmem;

	node = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!node)
		return dev_err_probe(dev, -EINVAL, "no memory-region specified\n");

	rmem = of_reserved_mem_lookup(node);
	of_node_put(node);
	if (!rmem)
		return dev_err_probe(dev, -EINVAL, "unable to resolve memory-region\n");

	priv->mem_phys = priv->mem_reloc = rmem->base;
	priv->mem_size = rmem->size;
	priv->mem_region = devm_ioremap_wc(dev, priv->mem_phys, priv->mem_size);
	if (!priv->mem_region)
		return dev_err_probe(dev, -EBUSY, "unable to map memory region: %pa+%pa\n",
				     &rmem->base, &rmem->size);

	node = of_parse_phandle(dev->of_node, "memory-region", 1);
	if (!node)
		return 0;

	rmem = of_reserved_mem_lookup(node);
	of_node_put(node);
	if (!rmem)
		return dev_err_probe(dev, -EINVAL, "unable to resolve dtb memory-region\n");

	priv->dtb_mem_phys = priv->dtb_mem_reloc = rmem->base;
	priv->dtb_mem_size = rmem->size;
	priv->dtb_mem_region = devm_ioremap_wc(dev, priv->dtb_mem_phys, priv->dtb_mem_size);
	if (!priv->dtb_mem_region)
		return dev_err_probe(dev, -EBUSY, "unable to map dtb memory region: %pa+%pa\n",
				     &rmem->base, &rmem->size);

	return 0;
}

/**
 * cdsp_pas_load() - .load rproc_op: stash the main firmware, load the dtb image
 */
static int cdsp_pas_load(struct rproc *rproc, const struct firmware *fw)
{
	struct cdsp_pas_priv *priv = rproc->priv;
	int ret;

	priv->firmware = fw;

	ret = request_firmware(&priv->dtb_firmware, CDSP_DTB_FIRMWARE_NAME, priv->dev);
	if (ret) {
		dev_err(priv->dev, "failed to load %s: %d\n", CDSP_DTB_FIRMWARE_NAME, ret);
		return ret;
	}

	ret = qcom_mdt_pas_init(priv->dev, priv->dtb_firmware, CDSP_DTB_FIRMWARE_NAME,
				CDSP_DTB_PAS_ID, priv->dtb_mem_phys, &priv->dtb_pas_ctx);
	if (ret)
		goto release_dtb_firmware;

	ret = qcom_mdt_load_no_init(priv->dev, priv->dtb_firmware, CDSP_DTB_FIRMWARE_NAME,
				    CDSP_DTB_PAS_ID, priv->dtb_mem_region, priv->dtb_mem_phys,
				    priv->dtb_mem_size, &priv->dtb_mem_reloc);
	if (ret)
		goto release_dtb_firmware;

	return 0;

release_dtb_firmware:
	release_firmware(priv->dtb_firmware);
	priv->dtb_firmware = NULL;
	return ret;
}

/**
 * cdsp_pas_handover() - Drop the PAS regulator vote, gated by the LPM state machine
 *
 * Registered as the qcom_q6v5 handover callback and also invoked directly
 * and synchronously from cdsp_pas_stop() -- IPQ9650 has no AOSS, so the
 * handover IRQ this would normally fire on never arrives, and
 * qcom_q6v5_unprepare() always reports handover-already-issued=true.
 */
static void cdsp_pas_handover(struct qcom_q6v5 *q6v5)
{
	struct cdsp_pas_priv *priv = container_of(q6v5, struct cdsp_pas_priv, q6v5);

	mutex_lock(&priv->lock);

	priv->pas_active = false;

	if (!priv->lpm_active_mx && priv->mx)
		regulator_disable(priv->mx);
	if (!priv->lpm_active_cx)
		regulator_disable(priv->cx);

	mutex_unlock(&priv->lock);
}

/**
 * cdsp_dump_start_timeout_diagnostics() - Dump state useful for root-causing a start timeout
 * @priv: Driver context, called with priv->lock held
 *
 * qcom_q6v5_wait_for_start() times out when the NSP Q6 never signals "ready"
 * via SMP2P. This alone doesn't say why -- dump the CX/MX rail state the Q6
 * saw at reset-release, the MPM handshake/IRQ-status registers (in case Q6
 * already tried an LPM request that's stuck), the RSCC power-mode register,
 * and the SMEM channel state (cdsp_state != 0 means Q6 got far enough into
 * its own boot to touch SMEM, independent of the SMP2P ready signal).
 */
static void cdsp_dump_start_timeout_diagnostics(struct cdsp_pas_priv *priv)
{
	struct cdsp_smem_region *smem = priv->smem;
	u32 rsc_irq_stat = 0, hdshk = 0, br_event = 0;
	int cx_uv = -1, mx_uv = -1;

	regmap_read(priv->mpm_regmap, RSC_HDSHK_IRQ_STAT, &rsc_irq_stat);
	regmap_read(priv->mpm_regmap, CLIENT_RSC_HDSHK(0), &hdshk);
	regmap_read(priv->rscc_regmap, RSCC_BR_EVENT, &br_event);

	if (priv->cx)
		cx_uv = regulator_get_voltage(priv->cx);
	if (priv->mx)
		mx_uv = regulator_get_voltage(priv->mx);

	dev_err(priv->dev,
		"start timeout diagnostics: cx=%s(%duV) mx=%s(%duV) rsc_hdshk_irq_stat=0x%x "
		"client_rsc_hdshk=0x%x rscc_br_event=0x%x smem: magic=0x%x cdsp_state=%u "
		"apss_state=%u req_in_flight=%u cdsp_tx=%u cdsp_rx=%u\n",
		priv->cx ? (regulator_is_enabled(priv->cx) ? "on" : "off") : "n/a", cx_uv,
		priv->mx ? (regulator_is_enabled(priv->mx) ? "on" : "off") : "n/a", mx_uv,
		rsc_irq_stat, hdshk, br_event, smem->hdr.magic, smem->hdr.cdsp_state,
		smem->hdr.apss_state, smem->hdr.request_in_flight,
		smem->hdr.cdsp_tx_count, smem->hdr.cdsp_rx_count);
}

/**
 * cdsp_pas_start() - .start rproc_op: authenticate and reset both PAS images
 */
static int cdsp_pas_start(struct rproc *rproc)
{
	struct cdsp_pas_priv *priv = rproc->priv;
	void __iomem *tcsr;
	int ret;

	ret = qcom_q6v5_prepare(&priv->q6v5);
	if (ret)
		return ret;

	mutex_lock(&priv->lock);

	priv->pas_active = true;

	if (!priv->lpm_active_cx) {
		ret = regulator_enable(priv->cx);
		if (ret)
			goto clear_pas_active;
	}

	if (!priv->lpm_active_mx && priv->mx) {
		ret = regulator_enable(priv->mx);
		if (ret)
			goto disable_cx;
	}

	ret = qcom_pas_auth_and_reset(CDSP_DTB_PAS_ID);
	if (ret) {
		dev_err(priv->dev, "failed to authenticate dtb image and release reset\n");
		goto disable_mx;
	}

	ret = qcom_mdt_pas_init(priv->dev, priv->firmware, CDSP_FIRMWARE_NAME,
				CDSP_PAS_ID, priv->mem_phys, &priv->pas_ctx);
	if (ret)
		goto disable_mx;

	ret = qcom_mdt_load_no_init(priv->dev, priv->firmware, CDSP_FIRMWARE_NAME,
				    CDSP_PAS_ID, priv->mem_region, priv->mem_phys,
				    priv->mem_size, &priv->mem_reloc);
	if (ret)
		goto disable_mx;

	qcom_pil_info_store(CDSP_SSR_NAME, priv->mem_phys, priv->mem_size);

	if (debug_q6) {
		tcsr = ioremap(TCSR_SPARE_REG0, TCSR_SPARE_REG0_SIZE);
		if (tcsr) {
			writel(0x1, tcsr);
			iounmap(tcsr);
		}
	}

	ret = qcom_pas_auth_and_reset(CDSP_PAS_ID);
	if (ret) {
		dev_err(priv->dev, "failed to authenticate image and release reset\n");
		goto disable_mx;
	}

	dev_info(priv->dev, "cx=%s(%duV) mx=%s(%duV): reset released, waiting for Q6 ready ack\n",
		 regulator_is_enabled(priv->cx) ? "on" : "off", regulator_get_voltage(priv->cx),
		 priv->mx ? (regulator_is_enabled(priv->mx) ? "on" : "off") : "n/a",
		 priv->mx ? regulator_get_voltage(priv->mx) : -1);

	/*
	 * Drop priv->lock across the blocking wait: the NSP Q6 sends a DCVS
	 * request over SMEM before/while it brings itself up, and
	 * cdsp_dcvs_work_fn() needs this same lock to process it and reply.
	 * Holding the lock here deadlocks against Q6, which stalls waiting on
	 * that reply before it ever raises its ready ack.
	 */
	mutex_unlock(&priv->lock);

wait_for_start:
	ret = qcom_q6v5_wait_for_start(&priv->q6v5, msecs_to_jiffies(5000));
	if (ret == -ETIMEDOUT && debug_q6) {
		dev_err(priv->dev, "start timed out, debug_q6 set, retrying wait\n");
		goto wait_for_start;
	}

	mutex_lock(&priv->lock);

	if (ret) {
		dev_err(priv->dev, "start timed out\n");
		cdsp_dump_start_timeout_diagnostics(priv);
	}

	qcom_pas_metadata_release(&priv->dtb_pas_ctx);
	qcom_pas_metadata_release(&priv->pas_ctx);
	release_firmware(priv->dtb_firmware);
	priv->dtb_firmware = NULL;
	priv->firmware = NULL;

	mutex_unlock(&priv->lock);

	return ret;

disable_mx:
	if (!priv->lpm_active_mx && priv->mx)
		regulator_disable(priv->mx);
disable_cx:
	if (!priv->lpm_active_cx)
		regulator_disable(priv->cx);
clear_pas_active:
	priv->pas_active = false;
	mutex_unlock(&priv->lock);
	qcom_q6v5_unprepare(&priv->q6v5);
	release_firmware(priv->dtb_firmware);
	priv->dtb_firmware = NULL;
	priv->firmware = NULL;
	return ret;
}

/**
 * cdsp_pas_stop() - .stop rproc_op: request stop, shut down both PAS images
 */
static int cdsp_pas_stop(struct rproc *rproc)
{
	struct cdsp_pas_priv *priv = rproc->priv;
	int handover;
	int dtb_ret;
	int ret;

	ret = qcom_q6v5_request_stop(&priv->q6v5, priv->sysmon);
	if (ret == -ETIMEDOUT)
		dev_err(priv->dev, "timed out on wait\n");

	ret = qcom_pas_shutdown(CDSP_PAS_ID);
	if (ret)
		dev_err(priv->dev, "failed to shutdown: %d\n", ret);

	dtb_ret = qcom_pas_shutdown(CDSP_DTB_PAS_ID);
	if (dtb_ret)
		dev_err(priv->dev, "failed to shutdown dtb: %d\n", dtb_ret);
	if (!ret && dtb_ret)
		ret = dtb_ret;

	handover = qcom_q6v5_unprepare(&priv->q6v5);
	if (handover)
		cdsp_pas_handover(&priv->q6v5);

	return ret;
}

/**
 * cdsp_pas_unprepare() - .unprepare rproc_op: release PAS metadata contexts
 */
static int cdsp_pas_unprepare(struct rproc *rproc)
{
	struct cdsp_pas_priv *priv = rproc->priv;

	qcom_pas_metadata_release(&priv->pas_ctx);
	qcom_pas_metadata_release(&priv->dtb_pas_ctx);

	return 0;
}

static void *cdsp_pas_da_to_va(struct rproc *rproc, u64 da, size_t len, bool *is_iomem)
{
	struct cdsp_pas_priv *priv = rproc->priv;
	int offset;

	offset = da - priv->mem_reloc;
	if (offset < 0 || offset + len > priv->mem_size)
		return NULL;

	if (is_iomem)
		*is_iomem = true;

	return priv->mem_region + offset;
}

static unsigned long cdsp_pas_panic(struct rproc *rproc)
{
	struct cdsp_pas_priv *priv = rproc->priv;

	return qcom_q6v5_panic(&priv->q6v5);
}

static const struct rproc_ops cdsp_pas_ops = {
	.unprepare = cdsp_pas_unprepare,
	.start = cdsp_pas_start,
	.stop = cdsp_pas_stop,
	.da_to_va = cdsp_pas_da_to_va,
	.parse_fw = qcom_register_dump_segments,
	.load = cdsp_pas_load,
	.panic = cdsp_pas_panic,
};

/**
 * cdsp_smem_init() - Allocate and initialise the DCVS SMEM channel
 *
 * Zeroes the entire SMEM region first so all padding/reserved fields are
 * clean, then fills in the fixed protocol fields. apss_state is set last
 * (after wmb) so the NSP Q6 only observes a fully-populated header once
 * APSS is ready -- this must run after every other dependency (regulators,
 * mailbox, IRQs) is already live.
 */
static int cdsp_smem_init(struct cdsp_pas_priv *priv)
{
	size_t smem_size;
	int ret;

	ret = qcom_smem_alloc(CDSP_SMEM_NSP_HOST_ID, CDSP_SMEM_ITEM, CDSP_SMEM_SIZE);
	if (ret && ret != -EEXIST)
		return dev_err_probe(priv->dev, ret, "failed to allocate SMEM\n");

	priv->smem = qcom_smem_get(CDSP_SMEM_NSP_HOST_ID, CDSP_SMEM_ITEM, &smem_size);
	if (IS_ERR(priv->smem))
		return dev_err_probe(priv->dev, PTR_ERR(priv->smem), "failed to get SMEM\n");

	if (smem_size < CDSP_SMEM_SIZE)
		return dev_err_probe(priv->dev, -EINVAL,
				     "SMEM region too small: got %zu, expected %u\n",
				     smem_size, CDSP_SMEM_SIZE);

	memset(priv->smem, 0, sizeof(*priv->smem));
	priv->smem->hdr.magic           = CDSP_SMEM_MAGIC;
	priv->smem->hdr.version         = CDSP_SMEM_VERSION;
	priv->smem->hdr.request_offset  = CDSP_SMEM_REQUEST_OFFSET;
	priv->smem->hdr.request_size    = CDSP_SMEM_REQUEST_SIZE;
	priv->smem->hdr.response_offset = CDSP_SMEM_RESPONSE_OFFSET;
	priv->smem->hdr.response_size   = CDSP_SMEM_RESPONSE_SIZE;
	priv->smem->hdr.apss_state = 1;
	wmb();

	return 0;
}

static int cdsp_pas_probe(struct platform_device *pdev)
{
	struct cdsp_pas_priv *priv;
	struct nvmem_cell *nvmem;
	void __iomem *mpm_base;
	void __iomem *rscc_base;
	struct rproc *rproc;
	u8 *disable_status;
	size_t len;
	int ret;

	/*
	 * If nvmem-cells present on NSP/Turing node in DTSI, check QFPROM fuse
	 * for Turing SS disable status (SOFTSKU_STATUS_TURING, bit 0).
	 * 0 = enabled, 1 = disabled.
	 */
	nvmem = devm_nvmem_cell_get(&pdev->dev, NULL);
	if (IS_ERR(nvmem)) {
		if (PTR_ERR(nvmem) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
	} else {
		disable_status = nvmem_cell_read(nvmem, &len);
		devm_nvmem_cell_put(&pdev->dev, nvmem);
		if (!IS_ERR(disable_status) && ((unsigned int)(*disable_status) == 1)) {
			dev_info(&pdev->dev, "NSP/Turing disabled in qfprom efuse\n");
			kfree(disable_status);
			return -ENODEV;
		}
		kfree(disable_status);
	}

	if (!qcom_pas_is_available())
		return -EPROBE_DEFER;

	rproc = rproc_alloc(&pdev->dev, pdev->name, &cdsp_pas_ops,
			    CDSP_FIRMWARE_NAME, sizeof(*priv));
	if (!rproc)
		return -ENOMEM;

	rproc->auto_boot = false;
	rproc_coredump_set_elf_info(rproc, ELFCLASS32, EM_NONE);
	rproc->dump_conf = RPROC_COREDUMP_INLINE;

	priv = rproc->priv;
	priv->dev = &pdev->dev;
	priv->rproc = rproc;
	mutex_init(&priv->lock);
	priv->nsp_up = true;
	platform_set_drvdata(pdev, priv);
	device_init_wakeup(&pdev->dev, true);

	ret = cdsp_alloc_memory_region(priv);
	if (ret)
		goto free_rproc;

	priv->cx = devm_regulator_get(&pdev->dev, "vdd-cx");
	if (IS_ERR(priv->cx)) {
		ret = dev_err_probe(&pdev->dev, PTR_ERR(priv->cx), "failed to get vdd-cx\n");
		goto free_rproc;
	}

	priv->mx = devm_regulator_get_optional(&pdev->dev, "vdd-mx");
	if (IS_ERR(priv->mx)) {
		if (PTR_ERR(priv->mx) != -ENODEV) {
			ret = dev_err_probe(&pdev->dev, PTR_ERR(priv->mx),
					    "failed to get vdd-mx\n");
			goto free_rproc;
		}
		priv->mx = NULL;
		dev_dbg(&pdev->dev, "no vdd-mx regulator, MX rail absent on this board\n");
	}

	priv->dcvs_mbox_client.dev = &pdev->dev;
	priv->dcvs_mbox_client.knows_txdone = true;
	priv->dcvs_mbox_chan = mbox_request_channel(&priv->dcvs_mbox_client, 0);
	if (IS_ERR(priv->dcvs_mbox_chan)) {
		ret = dev_err_probe(&pdev->dev, PTR_ERR(priv->dcvs_mbox_chan),
				    "failed to get dcvs mbox channel\n");
		goto free_rproc;
	}

	priv->dcvs_irq = platform_get_irq_byname(pdev, "dcvs");
	if (priv->dcvs_irq < 0) {
		ret = priv->dcvs_irq;
		goto free_mbox;
	}

	ret = devm_request_threaded_irq(&pdev->dev, priv->dcvs_irq, NULL,
					cdsp_dcvs_irq_handler, IRQF_ONESHOT,
					"cdsp-dcvs", priv);
	if (ret) {
		dev_err_probe(&pdev->dev, ret, "failed to request dcvs irq\n");
		goto free_mbox;
	}

	mpm_base = devm_platform_ioremap_resource_byname(pdev, "mpm");
	if (IS_ERR(mpm_base)) {
		ret = PTR_ERR(mpm_base);
		goto free_mbox;
	}

	priv->mpm_regmap = devm_regmap_init_mmio(&pdev->dev, mpm_base, &cdsp_mpm_regmap_config);
	if (IS_ERR(priv->mpm_regmap)) {
		ret = PTR_ERR(priv->mpm_regmap);
		goto free_mbox;
	}

	rscc_base = devm_platform_ioremap_resource_byname(pdev, "rscc");
	if (IS_ERR(rscc_base)) {
		ret = PTR_ERR(rscc_base);
		goto free_mbox;
	}

	priv->rscc_regmap = devm_regmap_init_mmio(&pdev->dev, rscc_base, &cdsp_rscc_regmap_config);
	if (IS_ERR(priv->rscc_regmap)) {
		ret = PTR_ERR(priv->rscc_regmap);
		goto free_mbox;
	}

	priv->lpm_irq = platform_get_irq_byname(pdev, "lpm");
	if (priv->lpm_irq < 0) {
		ret = priv->lpm_irq;
		goto free_mbox;
	}

	INIT_WORK(&priv->dcvs_work, cdsp_dcvs_work_fn);
	INIT_WORK(&priv->lpm_work, cdsp_lpm_work_fn);

	priv->lpm_wq = alloc_ordered_workqueue("cdsp_lpm_wq", 0);
	if (!priv->lpm_wq) {
		ret = dev_err_probe(&pdev->dev, -ENOMEM, "failed to allocate lpm workqueue\n");
		goto free_mbox;
	}

	ret = devm_request_threaded_irq(&pdev->dev, priv->lpm_irq, NULL,
					cdsp_lpm_irq_handler, IRQF_ONESHOT,
					"cdsp-lpm", priv);
	if (ret) {
		dev_err_probe(&pdev->dev, ret, "failed to request lpm irq\n");
		goto free_lpm_wq;
	}

	ret = qcom_q6v5_init(&priv->q6v5, pdev, rproc, QCOM_SMEM_HOST_ANY,
			     CDSP_CRASH_REASON_SMEM, CDSP_SSR_NAME, cdsp_pas_handover);
	if (ret)
		goto free_lpm_wq;

	qcom_add_glink_subdev(rproc, &priv->glink_subdev, CDSP_SSR_NAME);
	priv->sysmon = qcom_add_sysmon_subdev(rproc, CDSP_SSR_NAME, CDSP_SSCTL_ID);
	qcom_add_smd_subdev(rproc, &priv->smd_subdev);
	qcom_add_ssr_subdev(rproc, &priv->ssr_subdev, CDSP_SSR_NAME);

	ret = cdsp_smem_init(priv);
	if (ret)
		goto remove_subdevs;

	ret = rproc_add(rproc);
	if (ret)
		goto remove_subdevs;

	return 0;

remove_subdevs:
	qcom_remove_glink_subdev(rproc, &priv->glink_subdev);
	qcom_remove_sysmon_subdev(priv->sysmon);
	qcom_remove_smd_subdev(rproc, &priv->smd_subdev);
	qcom_remove_ssr_subdev(rproc, &priv->ssr_subdev);
	qcom_q6v5_deinit(&priv->q6v5);
free_lpm_wq:
	destroy_workqueue(priv->lpm_wq);
free_mbox:
	mbox_free_channel(priv->dcvs_mbox_chan);
free_rproc:
	device_init_wakeup(&pdev->dev, false);
	rproc_free(rproc);
	return ret;
}

static void cdsp_pas_remove(struct platform_device *pdev)
{
	struct cdsp_pas_priv *priv = platform_get_drvdata(pdev);
	struct rproc *rproc = priv->rproc;

	disable_irq(priv->dcvs_irq);
	disable_irq(priv->lpm_irq);

	cancel_work_sync(&priv->dcvs_work);
	cancel_work_sync(&priv->lpm_work);

	destroy_workqueue(priv->lpm_wq);

	rproc_del(rproc);

	qcom_q6v5_deinit(&priv->q6v5);

	qcom_remove_glink_subdev(rproc, &priv->glink_subdev);
	qcom_remove_sysmon_subdev(priv->sysmon);
	qcom_remove_smd_subdev(rproc, &priv->smd_subdev);
	qcom_remove_ssr_subdev(rproc, &priv->ssr_subdev);

	mbox_free_channel(priv->dcvs_mbox_chan);

	/* Belt-and-braces: the normal path already dropped both votes via cdsp_pas_stop() */
	mutex_lock(&priv->lock);
	if (priv->mx && (priv->pas_active || priv->lpm_active_mx))
		regulator_disable(priv->mx);
	if (priv->pas_active || priv->lpm_active_cx)
		regulator_disable(priv->cx);
	mutex_unlock(&priv->lock);

	device_init_wakeup(&pdev->dev, false);

	rproc_free(rproc);
}

static const struct of_device_id cdsp_pas_of_match[] = {
	{ .compatible = "qcom,ipq9650-cdsp-pas" },
	{ }
};
MODULE_DEVICE_TABLE(of, cdsp_pas_of_match);

static struct platform_driver cdsp_pas_driver = {
	.probe = cdsp_pas_probe,
	.remove_new = cdsp_pas_remove,
	.driver = {
		.name = "qcom-q6v5-ipq-cdsp",
		.of_match_table = cdsp_pas_of_match,
	},
};

module_platform_driver(cdsp_pas_driver);

MODULE_DESCRIPTION("QTI IPQ9650 CDSP PAS/Power remoteproc driver");
MODULE_LICENSE("GPL v2");
