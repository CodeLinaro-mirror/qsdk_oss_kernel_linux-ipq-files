/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef __QCE2204_PPE_H__
#define __QCE2204_PPE_H__

#include <linux/types.h>
#include <linux/bitmap.h>

/* Forward declaration */
struct qce2204_priv;

/* PPE supports 300 queues, each bit presents as one queue */
#define QCE2204_PPE_RING_TO_QUEUE_BITMAP_WORD_CNT	10

/* The service code is used by EDMA port to transmit packet to PPE */
#define QCE2204_PPE_EDMA_SC_BYPASS_ID			1

/* PPE queue scheduler priority number */
#define QCE2204_PPE_QUEUE_SCH_PRI_NUM			8

/* PPE CPU code num */
#define QCE2204_PPE_CPU_CODE_NUM			256

/* PPE internal priority and hash numbers */
#define QCE2204_PPE_QUEUE_INTER_PRI_NUM			16
#define QCE2204_PPE_QUEUE_HASH_NUM			256

/* PPE RSS hash configuration */
#define QCE2204_PPE_RSS_HASH_MODE_IPV4			BIT(0)
#define QCE2204_PPE_RSS_HASH_MODE_IPV6			BIT(1)
#define QCE2204_PPE_RSS_HASH_IP_LENGTH			4
#define QCE2204_PPE_RSS_HASH_TUPLES			5

/* PPE VSI table entries */
#define QCE2204_PPE_VSI_TBL_ENTRIES_NUM			64

/* Queue base index ranges */
#define QCE2204_PPE_QUEUE_BASE_DEST_PORT		0
#define QCE2204_PPE_QUEUE_BASE_CPU_CODE			1024
#define QCE2204_PPE_QUEUE_BASE_SERVICE_CODE		2048

/* Dest info encap */
#define QCE2204_PPE_DEST_INFO(type, value) (((type) << 12) | (value))

/**
 * enum qce2204_ppe_scheduler_frame_mode - PPE scheduler frame mode
 * @QCE2204_PPE_SCH_WITH_IPG_PREAMBLE_FRAME_CRC: Frame with IPG, preamble, and CRC
 * @QCE2204_PPE_SCH_WITH_FRAME_CRC: Frame with CRC only
 * @QCE2204_PPE_SCH_WITH_L3_PAYLOAD: L3 payload only
 */
enum qce2204_ppe_scheduler_frame_mode {
	QCE2204_PPE_SCH_WITH_IPG_PREAMBLE_FRAME_CRC = 0,
	QCE2204_PPE_SCH_WITH_FRAME_CRC = 1,
	QCE2204_PPE_SCH_WITH_L3_PAYLOAD = 2,
};

/**
 * enum qce2204_ppe_direction - PPE direction (ingress/egress)
 * @QCE2204_PPE_INGRESS: Ingress direction
 * @QCE2204_PPE_EGRESS: Egress direction
 *
 * This enum is used throughout the driver for ingress/egress configuration
 */
enum qce2204_ppe_direction {
	QCE2204_PPE_INGRESS = 0,
	QCE2204_PPE_EGRESS = 1,
};

/**
 * enum qce2204_ppe_resource_type - PPE resource type
 * @QCE2204_PPE_RES_UCAST: Unicast queue resource
 * @QCE2204_PPE_RES_MCAST: Multicast queue resource
 * @QCE2204_PPE_RES_L0_NODE: Level 0 queue-based node resource
 * @QCE2204_PPE_RES_L1_NODE: Level 1 flow-based node resource
 * @QCE2204_PPE_RES_FLOW_ID: Flow-based node resource
 */
enum qce2204_ppe_resource_type {
	QCE2204_PPE_RES_UCAST,
	QCE2204_PPE_RES_MCAST,
	QCE2204_PPE_RES_L0_NODE,
	QCE2204_PPE_RES_L1_NODE,
	QCE2204_PPE_RES_FLOW_ID,
};

/**
 * enum qce2204_ppe_action_type - PPE action type
 * @QCE2204_PPE_ACTION_FORWARD: Forward packet
 * @QCE2204_PPE_ACTION_DROP: Drop packet
 * @QCE2204_PPE_ACTION_COPY_TO_CPU: Copy to CPU
 * @QCE2204_PPE_ACTION_REDIRECT_TO_CPU: Redirect to CPU
 */
enum qce2204_ppe_action_type {
	QCE2204_PPE_ACTION_FORWARD = 0,
	QCE2204_PPE_ACTION_DROP = 1,
	QCE2204_PPE_ACTION_COPY_TO_CPU = 2,
	QCE2204_PPE_ACTION_REDIRECT_TO_CPU = 3,
};

/**
 * enum qce2204_ppe_dest_info_type - PPE dest type
 * @QCE2204_PPE_DEST_INFO_PORT_ID: Port ID
 * @QCE2204_PPE_DEST_INFO_PORT_BMP: Port BitMap
 */
enum qce2204_ppe_dest_info_type {
	QCE2204_PPE_DEST_INFO_PORT_ID = 2,
	QCE2204_PPE_DEST_INFO_PORT_BMP = 3,
};

/**
 * struct qce2204_ppe_bm_port_config - PPE BM port configuration
 * @port_id_start: First BM port ID to configure
 * @port_id_end: Last BM port ID to configure
 * @pre_alloc: BM port dedicated buffer number
 * @in_fly_buf: Buffer number for receiving packet after pause frame sent
 * @ceil: Ceil to generate back pressure
 * @weight: Weight value
 * @resume_offset: Resume offset from threshold value
 * @resume_ceil: Ceil to resume from back pressure state
 * @dynamic: Dynamic threshold used or not
 */
struct qce2204_ppe_bm_port_config {
	unsigned int port_id_start;
	unsigned int port_id_end;
	unsigned int pre_alloc;
	unsigned int in_fly_buf;
	unsigned int ceil;
	unsigned int weight;
	unsigned int resume_offset;
	unsigned int resume_ceil;
	bool dynamic;
};

/**
 * struct qce2204_ppe_qm_queue_config - PPE queue config
 * @queue_start: PPE start of queue ID
 * @queue_end: PPE end of queue ID
 * @prealloc_buf: Queue dedicated buffer number
 * @ceil: Ceil to start drop packet from queue
 * @weight: Weight value
 * @resume_offset: Resume offset from threshold
 * @dynamic: Threshold value decided dynamically or statically
 */
struct qce2204_ppe_qm_queue_config {
	unsigned int queue_start;
	unsigned int queue_end;
	unsigned int prealloc_buf;
	unsigned int ceil;
	unsigned int weight;
	unsigned int resume_offset;
	bool dynamic;
};

/**
 * struct qce2204_ppe_scheduler_bm_config - PPE arbitration for buffer config
 * @valid: Arbitration entry valid or not
 * @dir: Arbitration entry for egress or ingress
 * @port: Port ID to use arbitration entry
 * @backup_port_valid: Backup port valid or not
 * @backup_port: Backup port ID to use
 */
struct qce2204_ppe_scheduler_bm_config {
	bool valid;
	enum qce2204_ppe_direction dir;
	unsigned int port;
	bool backup_port_valid;
	unsigned int backup_port;
};

/**
 * struct qce2204_ppe_scheduler_qm_config - PPE arbitration for scheduler config
 * @ensch_port_bmp: Port bit map for enqueue scheduler
 * @ensch_port: Port ID to enqueue scheduler
 * @desch_port: Port ID to dequeue scheduler
 * @desch_backup_port_valid: Dequeue for backup port valid or not
 * @desch_backup_port: Backup port ID to dequeue scheduler
 */
struct qce2204_ppe_scheduler_qm_config {
	unsigned int ensch_port_bmp;
	unsigned int ensch_port;
	unsigned int desch_port;
	bool desch_backup_port_valid;
	unsigned int desch_backup_port;
};

/**
 * struct qce2204_ppe_scheduler_port_config - PPE port scheduler config
 * @port: Port ID to be scheduled
 * @flow_level: Scheduler flow level or not
 * @node_id: Node ID, for level 0, queue ID is used
 * @loop_num: Loop number of scheduler config
 * @pri_max: Max priority configured
 * @flow_id: Strict priority ID
 * @drr_node_id: Node ID for scheduler
 */
struct qce2204_ppe_scheduler_port_config {
	unsigned int port;
	bool flow_level;
	unsigned int node_id;
	unsigned int loop_num;
	unsigned int pri_max;
	unsigned int flow_id;
	unsigned int drr_node_id;
};

/**
 * struct qce2204_ppe_port_schedule_resource - PPE port scheduler resource
 * @ucastq_start: Unicast queue start ID
 * @ucastq_end: Unicast queue end ID
 * @mcastq_start: Multicast queue start ID
 * @mcastq_end: Multicast queue end ID
 * @flow_id_start: Flow start ID
 * @flow_id_end: Flow end ID
 * @l0node_start: Scheduler node start ID for queue level
 * @l0node_end: Scheduler node end ID for queue level
 * @l1node_start: Scheduler node start ID for flow level
 * @l1node_end: Scheduler node end ID for flow level
 */
struct qce2204_ppe_port_schedule_resource {
	unsigned int ucastq_start;
	unsigned int ucastq_end;
	unsigned int mcastq_start;
	unsigned int mcastq_end;
	unsigned int flow_id_start;
	unsigned int flow_id_end;
	unsigned int l0node_start;
	unsigned int l0node_end;
	unsigned int l1node_start;
	unsigned int l1node_end;
};

/**
 * struct qce2204_ppe_scheduler_cfg - PPE scheduler configuration
 * @flow_id: PPE flow ID
 * @pri: Scheduler priority
 * @drr_node_id: Node ID for scheduled traffic
 * @drr_node_wt: Weight for scheduled traffic
 * @unit_is_packet: Packet based or byte based unit
 * @frame_mode: Packet mode to be scheduled
 */
struct qce2204_ppe_scheduler_cfg {
	int flow_id;
	int pri;
	int drr_node_id;
	int drr_node_wt;
	bool unit_is_packet;
	enum qce2204_ppe_scheduler_frame_mode frame_mode;
};

/**
 * struct qce2204_ppe_queue_ucast_dest - PPE unicast queue destination
 * @src_profile: Source profile
 * @service_code_en: Enable service code to map queue base ID
 * @service_code: Service code
 * @cpu_code_en: Enable CPU code to map queue base ID
 * @cpu_code: CPU code
 * @dest_port: Destination port
 */
struct qce2204_ppe_queue_ucast_dest {
	int src_profile;
	bool service_code_en;
	int service_code;
	bool cpu_code_en;
	int cpu_code;
	int dest_port;
};

/* Hardware bitmaps for bypassing features */
enum qce2204_ppe_sc_ingress_type {
	QCE2204_PPE_SC_BYPASS_INGRESS_FAKE_MAC_HEADER = 8,
	QCE2204_PPE_SC_BYPASS_INGRESS_SERVICE_CODE = 9,
	QCE2204_PPE_SC_BYPASS_INGRESS_FAKE_L2_PROTO = 16,
	QCE2204_PPE_SC_BYPASS_INGRESS_SIZE = 29,
};

enum qce2204_ppe_sc_egress_type {
	QCE2204_PPE_SC_BYPASS_EGRESS_ACL_POST_ROUTING_CHECK = 14,
	QCE2204_PPE_SC_BYPASS_EGRESS_SIZE = 24,
};

enum qce2204_ppe_sc_counter_type {
	QCE2204_PPE_SC_BYPASS_COUNTER_RX_VLAN = 0,
	QCE2204_PPE_SC_BYPASS_COUNTER_RX = 1,
	QCE2204_PPE_SC_BYPASS_COUNTER_TX_VLAN = 2,
	QCE2204_PPE_SC_BYPASS_COUNTER_TX = 3,
	QCE2204_PPE_SC_BYPASS_COUNTER_SIZE = 4,
};

enum qce2204_ppe_sc_tunnel_type {
	QCE2204_PPE_SC_BYPASS_TUNNEL_SIZE = 21,
};

/**
 * struct qce2204_ppe_sc_bypass - PPE service bypass bitmaps
 * @ingress: Bitmap of features bypassed on ingress
 * @egress: Bitmap of features bypassed on egress
 * @counter: Bitmap of features bypassed on counter
 * @tunnel: Bitmap of features bypassed on tunnel
 */
struct qce2204_ppe_sc_bypass {
	DECLARE_BITMAP(ingress, QCE2204_PPE_SC_BYPASS_INGRESS_SIZE);
	DECLARE_BITMAP(egress, QCE2204_PPE_SC_BYPASS_EGRESS_SIZE);
	DECLARE_BITMAP(counter, QCE2204_PPE_SC_BYPASS_COUNTER_SIZE);
	DECLARE_BITMAP(tunnel, QCE2204_PPE_SC_BYPASS_TUNNEL_SIZE);
};

/**
 * struct qce2204_ppe_sc_cfg - PPE service code configuration
 * @dest_port_valid: Generate destination port or not
 * @dest_port: Destination port ID
 * @bitmaps: Bitmap of bypass features
 * @is_src: Destination port acts as source port
 * @next_service_code: New service code generated
 * @eip_field_update_bitmap: Fields updated for EIP
 * @eip_hw_service: Selected hardware functions for EIP
 * @eip_offset_sel: Packet offset selection for EIP
 */
struct qce2204_ppe_sc_cfg {
	bool dest_port_valid;
	int dest_port;
	struct qce2204_ppe_sc_bypass bitmaps;
	bool is_src;
	int next_service_code;
	int eip_field_update_bitmap;
	int eip_hw_service;
	int eip_offset_sel;
};

/**
 * struct qce2204_ppe_rss_hash_cfg - PPE RSS hash configuration
 * @hash_mask: Mask of generated hash value
 * @hash_fragment_mode: Hash generation mode for first fragment
 * @hash_seed: Seed to generate RSS hash
 * @hash_sip_mix: Source IP selection
 * @hash_dip_mix: Destination IP selection
 * @hash_protocol_mix: Protocol selection
 * @hash_sport_mix: Source L4 port selection
 * @hash_dport_mix: Destination L4 port selection
 * @hash_fin_inner: RSS hash value first selection
 * @hash_fin_outer: RSS hash value second selection
 */
struct qce2204_ppe_rss_hash_cfg {
	u32 hash_mask;
	bool hash_fragment_mode;
	u32 hash_seed;
	u8 hash_sip_mix[QCE2204_PPE_RSS_HASH_IP_LENGTH];
	u8 hash_dip_mix[QCE2204_PPE_RSS_HASH_IP_LENGTH];
	u8 hash_protocol_mix;
	u8 hash_sport_mix;
	u8 hash_dport_mix;
	u8 hash_fin_inner[QCE2204_PPE_RSS_HASH_TUPLES];
	u8 hash_fin_outer[QCE2204_PPE_RSS_HASH_TUPLES];
};

/**
 * struct qce2204_ppe_vsi_member_cfg - VSI member port configuration
 * @member_port_bitmap: Member port bitmap
 * @uuc_bitmap: Unknown unicast bitmap
 * @umc_bitmap: Unknown multicast bitmap
 * @bc_bitmap: Broadcast bitmap
 */
struct qce2204_ppe_vsi_member_cfg {
	u8 member_port_bitmap;
	u8 uuc_bitmap;
	u8 umc_bitmap;
	u8 bc_bitmap;
};

/**
 * struct qce2204_ppe_port_athtag_rx_cfg - Port Atheros tag RX configuration
 * @athtag_type: Atheros header type
 * @athtag_en: Enable Atheros header parsing
 * @version: Atheros header version
 */
struct qce2204_ppe_port_athtag_rx_cfg {
	u16 athtag_type;
	bool athtag_en;
	bool version;
};

/**
 * struct qce2204_ppe_athtag_dst_port_mapping_cfg - Atheros tag destination port mapping configuration
 * @dest_info_valid: Destination info valid flag
 * @dest_info: Destination info value
 */
struct qce2204_ppe_athtag_dst_port_mapping_cfg {
	bool dest_info_valid;
	u16 dest_info;
};

/**
 * struct qce2204_ppe_eg_vp_athtag_cfg - Egress VP Atheros tag configuration
 * @athtag_en: Enable Atheros header insertion
 * @version: Atheros header version
 * @action_type: Atheros header default type
 * @dest_info: Atheros port bitmap
 * @from_cpu: Atheros header from CPU flag
 */
struct qce2204_ppe_eg_vp_athtag_cfg {
	bool athtag_en;
	u8 version;
	u8 action_type;
	u8 dest_info;
	bool from_cpu;
};

/**
 * struct qce2204_ppe_port_mtu_cfg - Port MTU configuration
 * @mtu: Maximum transmission unit
 * @mtu_cmd: MTU action command
 */
struct qce2204_ppe_port_mtu_cfg {
	u16 mtu;
	u8 mtu_cmd;
};

/**
 * struct qce2204_ppe_port_mru_cfg - Port MRU configuration
 * @mru: Maximum receive unit
 * @mru_cmd: MRU action command
 */
struct qce2204_ppe_port_mru_cfg {
	u16 mru;
	u8 mru_cmd;
};

/**
 * struct qce2204_ppe_eg_gen_ctrl_cfg - Egress general control configuration
 * @ath_type: Atheros header type for egress packets
 */
struct qce2204_ppe_eg_gen_ctrl_cfg {
	u16 ath_type;
};

/**
 * struct qce2204_ppe_in_vlan_xlt_cfg - Ingress VLAN translation configuration
 * @port_id: Port ID or port bitmap
 * @svid_fmt: S-VID format
 * @svid_inc: S-VID include flag
 * @svid: S-VID value
 * @cvid_fmt: C-VID format
 * @cvid_inc: C-VID include flag
 * @cvid: C-VID value
 * @svid_xlt_cmd: S-VID translation command
 * @svid_xlt: S-VID translation value
 * @cvid_xlt_cmd: C-VID translation command
 * @cvid_xlt: C-VID translation value
 * @cpcp_xlt_cmd: C-PCP translation command
 * @tags_rmv: Tags to remove
 * @cnt_en: Counter enable
 * @cnt_id: Counter ID
 * @vsi_cmd: VSI command
 * @vsi: VSI value
 * @src_valid: Source info valid
 * @src_type: Source info type
 * @src_info: Source info value
 * @fwd_cmd: Forward command
 * @dest_valid: Destination info valid
 * @dest_info: Destination info value
 */
struct qce2204_ppe_in_vlan_xlt_cfg {
	u16 port_id;
	u8 svid_fmt;
	bool svid_inc;
	u16 svid;
	u8 cvid_fmt;
	bool cvid_inc;
	u16 cvid;
	u8 svid_xlt_cmd;
	u16 svid_xlt;
	u8 cvid_xlt_cmd;
	u16 cvid_xlt;
	u8 cpcp_xlt_cmd;
	u8 tags_rmv;
	bool cnt_en;
	u8 cnt_id;
	bool vsi_cmd;
	u8 vsi;
	bool src_valid;
	bool src_type;
	u8 src_info;
	u8 fwd_cmd;
	bool dest_valid;
	u16 dest_info;
};

/**
 * struct qce2204_ppe_vlan_tpid_cfg - VLAN TPID configuration
 * @ctpid: Customer VLAN TPID value
 * @stpid: Service VLAN TPID value
 * @ctpid_ext: Customer VLAN TPID extension value
 * @stpid_ext: Service VLAN TPID extension value
 * @ctpid_map: Customer VLAN TPID mapping
 * @stpid_map: Service VLAN TPID mapping
 */
struct qce2204_ppe_vlan_tpid_cfg {
	u16 ctpid;
	u16 stpid;
	u16 ctpid_ext;
	u16 stpid_ext;
	u8 ctpid_map;
	u8 stpid_map;
};

/**
 * struct qce2204_ppe_eg_vlan_xlt_cfg - Egress VLAN translation configuration
 * @port_id: Port ID or port bitmap
 * @vsi_inc: VSI include flag
 * @vsi_valid: VSI valid flag
 * @vsi: VSI value
 * @svid_fmt: S-VID format
 * @svid_inc: S-VID include flag
 * @svid: S-VID value
 * @cvid_fmt: C-VID format
 * @cvid_inc: C-VID include flag
 * @cvid: C-VID value
 * @svid_xlt_cmd: S-VID translation command
 * @svid_xlt: S-VID translation value
 * @cvid_xlt_cmd: C-VID translation command
 * @cvid_xlt: C-VID translation value
 * @tags_rmv: Tags to remove
 * @cnt_en: Counter enable
 * @cnt_id: Counter ID
 * @fwd_cmd: Forward command
 */
struct qce2204_ppe_eg_vlan_xlt_cfg {
	u16 port_id;
	bool vsi_inc;
	bool vsi_valid;
	u8 vsi;
	u8 svid_fmt;
	bool svid_inc;
	u16 svid;
	u8 cvid_fmt;
	bool cvid_inc;
	u16 cvid;
	u8 svid_xlt_cmd;
	u16 svid_xlt;
	u8 cvid_xlt_cmd;
	u16 cvid_xlt;
	u8 tags_rmv;
	bool cnt_en;
	u8 cnt_id;
	u8 fwd_cmd;
};

/* Function prototypes */
int qce2204_ppe_vsi_member_set(struct qce2204_priv *priv,
				u32 vsi_id,
				struct qce2204_ppe_vsi_member_cfg *cfg);
int qce2204_ppe_port_athtag_rx_set(struct qce2204_priv *priv,
				    u32 port_id,
				    struct qce2204_ppe_port_athtag_rx_cfg *cfg);
int qce2204_ppe_athtag_dst_port_mapping_set(struct qce2204_priv *priv,
					     u32 port_id,
					     struct qce2204_ppe_athtag_dst_port_mapping_cfg *cfg);
int qce2204_ppe_eg_vp_athtag_set(struct qce2204_priv *priv,
				  u32 port_id,
				  struct qce2204_ppe_eg_vp_athtag_cfg *cfg);
int qce2204_ppe_port_mtu_set(struct qce2204_priv *priv,
			      u32 port_id,
			      struct qce2204_ppe_port_mtu_cfg *cfg);
int qce2204_ppe_port_mru_set(struct qce2204_priv *priv,
			      u32 port_id,
			      struct qce2204_ppe_port_mru_cfg *cfg);
int qce2204_ppe_eg_gen_ctrl_set(struct qce2204_priv *priv,
				 struct qce2204_ppe_eg_gen_ctrl_cfg *cfg);
int qce2204_setup_cpu_port_athtag(struct qce2204_priv *priv);
int qce2204_teardown_cpu_port_athtag(struct qce2204_priv *priv);
int qce2204_ppe_hw_init(struct qce2204_priv *priv);
int qce2204_ppe_queue_scheduler_set(struct qce2204_priv *priv,
				    int node_id, bool flow_level, int port,
				    struct qce2204_ppe_scheduler_cfg scheduler_cfg);
int qce2204_ppe_queue_ucast_base_set(struct qce2204_priv *priv,
				     struct qce2204_ppe_queue_ucast_dest queue_dst,
				     int queue_base, int profile_id);
int qce2204_ppe_queue_ucast_offset_pri_set(struct qce2204_priv *priv,
					   int profile_id, int priority,
					   int queue_offset);
int qce2204_ppe_queue_ucast_offset_hash_set(struct qce2204_priv *priv,
					    int profile_id, int rss_hash,
					    int queue_offset);
int qce2204_ppe_port_resource_get(struct qce2204_priv *priv, int port,
				  enum qce2204_ppe_resource_type type,
				  int *res_start, int *res_end);
int qce2204_ppe_sc_config_set(struct qce2204_priv *priv, int sc,
			      struct qce2204_ppe_sc_cfg cfg);
int qce2204_ppe_counter_enable_set(struct qce2204_priv *priv, int port);
int qce2204_ppe_rss_hash_config_set(struct qce2204_priv *priv, int mode,
				    struct qce2204_ppe_rss_hash_cfg cfg);

/* VLAN Translation Functions */
int qce2204_ppe_vlan_in_vlan_xlt_set(struct qce2204_priv *priv,
				     u32 index,
				     struct qce2204_ppe_in_vlan_xlt_cfg *cfg);
int qce2204_ppe_vlan_eg_vlan_xlt_set(struct qce2204_priv *priv,
				     u32 index,
				     struct qce2204_ppe_eg_vlan_xlt_cfg *cfg);
int qce2204_ppe_vlan_tpid_set(struct qce2204_priv *priv,
			       enum qce2204_ppe_direction dir,
			       struct qce2204_ppe_vlan_tpid_cfg *cfg);

/* Port VLAN role configuration */
struct qce2204_ppe_port_vlan_role_cfg {
	bool port_role;  /* Port role: 0=edge port, 1=core port */
};

int qce2204_ppe_port_vlan_role_set(struct qce2204_priv *priv,
				    u32 port_id,
				    enum qce2204_ppe_direction direction,
				    struct qce2204_ppe_port_vlan_role_cfg *cfg);

/* STP State Configuration */
struct qce2204_ppe_stp_state_cfg {
	u8 stp_state;  /* STP state: 0=Disabled, 1=Blocking, 2=Learning, 3=Forwarding */
};

int qce2204_ppe_stp_state_set(struct qce2204_priv *priv,
			       u32 port_id,
			       struct qce2204_ppe_stp_state_cfg *cfg);

/* Port VSI Configuration */
struct qce2204_ppe_port_vsi_cfg {
	bool vsi_valid;
	u8 vsi;
};

int qce2204_ppe_port_vsi_set(struct qce2204_priv *priv,
			      u32 port_id,
			      struct qce2204_ppe_port_vsi_cfg *cfg);

/* DSA None Tag Support Functions */
int qce2204_setup_none_tag_vsi(struct qce2204_priv *priv);
int qce2204_teardown_none_tag_vsi(struct qce2204_priv *priv);

/* DSA 8021Q Support Functions */
int qce2204_setup_8021q_global(struct qce2204_priv *priv);
int qce2204_teardown_8021q_global(struct qce2204_priv *priv);
int qce2204_setup_8021q_tagging(struct qce2204_priv *priv, int port);
int qce2204_teardown_8021q_tagging(struct qce2204_priv *priv, int port);
int qce2204_port_alloc_ppe_resources(struct qce2204_priv *priv, int port);
void qce2204_port_free_ppe_resources(struct qce2204_priv *priv, int port);

#endif /* __QCE2204_PPE_H__ */
