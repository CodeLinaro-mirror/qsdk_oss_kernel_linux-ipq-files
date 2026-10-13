/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * QCE2204 PPE exported APIs for qca-ssdk integration.
 *
 * These functions are implemented in the QCE2204 DSA driver and exported
 * via EXPORT_SYMBOL_GPL so that the qca-ssdk HTTPPE adapter layer can call
 * them to implement FAL APIs (fal_port_mcast_priority_class_set,
 * fal_ucast_priority_class_set, fal_qm_threshold_reset,
 * fal_port_scheduler_resource_get, fal_port_scheduler_cfg_reset) for the
 * QCE2204 (HTT) chip.
 *
 * Each function takes a DSA user-port net_device; the QCE2204 DSA driver
 * resolves its own private data from it via dsa_port_from_netdev(). The
 * port-scoped functions use the net_device's own port index. The
 * switch-scoped functions (no per-port meaning) accept any
 * already-registered net_device belonging to the same switch, since the
 * private data is switch-wide, not port-specific.
 */

#ifndef __LINUX_DSA_QCE2204_API_H
#define __LINUX_DSA_QCE2204_API_H

struct net_device;

/**
 * struct qce2204_ppe_scheduler_resource - PPE port scheduler resource info
 * @ucastq_start: First unicast queue ID allocated to the port
 * @ucastq_end:   Last unicast queue ID allocated to the port
 * @mcastq_start: First multicast queue ID allocated to the port
 * @mcastq_end:   Last multicast queue ID allocated to the port
 * @flow_id_start: First L1 flow (SP) node ID allocated to the port
 * @flow_id_end:   Last L1 flow (SP) node ID allocated to the port
 * @l0node_start: First L0 queue-level scheduler node ID for the port
 * @l0node_end:   Last L0 queue-level scheduler node ID for the port
 * @l1node_start: First L1 flow-level scheduler node ID for the port
 * @l1node_end:   Last L1 flow-level scheduler node ID for the port
 */
struct qce2204_ppe_scheduler_resource {
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
 * qce2204_ppe_port_mcast_priority_class_set - Set multicast priority class
 * @netdev:      DSA user-port net_device
 * @priority:    Internal priority value (0-15)
 * @queue_class: Queue class offset to assign
 *
 * Supports fal_port_mcast_priority_class_set() for the QCE2204 chip.
 * Returns 0 on success, negative errno on failure.
 */
int qce2204_ppe_port_mcast_priority_class_set(struct net_device *netdev,
					      int priority, int queue_class);

/**
 * qce2204_ppe_ucast_priority_class_set - Set unicast priority class
 * @netdev:       Any already-registered DSA user-port net_device on the switch
 * @profile_id:   Queue profile ID
 * @priority:     Internal priority value (0-15)
 * @queue_offset: Queue offset to assign for this priority
 *
 * Supports fal_ucast_priority_class_set() for the QCE2204 chip.
 * Returns 0 on success, negative errno on failure.
 */
int qce2204_ppe_ucast_priority_class_set(struct net_device *netdev,
					 int profile_id, int priority,
					 int queue_offset);

/**
 * qce2204_ppe_qm_threshold_reset - Reset QM threshold for a queue
 * @netdev:   Any already-registered DSA user-port net_device on the switch
 * @queue_id: PPE queue ID (0-291)
 *
 * Resets the admission-control threshold of the specified queue to the
 * driver default values.  Supports fal_qm_threshold_reset() for the
 * QCE2204 chip.
 * Returns 0 on success, negative errno on failure.
 */
int qce2204_ppe_qm_threshold_reset(struct net_device *netdev, int queue_id);

/**
 * qce2204_ppe_port_scheduler_resource_get - Get port scheduler resources
 * @netdev: DSA user-port net_device
 * @res:    Output: filled with the scheduler resource allocation for @netdev's port
 *
 * Supports fal_port_scheduler_resource_get() for the QCE2204 chip.
 * Returns 0 on success, negative errno on failure.
 */
int qce2204_ppe_port_scheduler_resource_get(struct net_device *netdev,
					    struct qce2204_ppe_scheduler_resource *res);

/**
 * qce2204_ppe_port_scheduler_cfg_reset - Reset port scheduler to bootup defaults
 * @netdev: DSA user-port net_device
 *
 * Re-applies the bootup scheduler configuration stored in the QCE2204 DSA
 * driver for the given port.  Supports fal_port_scheduler_cfg_reset() for
 * the QCE2204 chip.
 * Returns 0 on success, negative errno on failure.
 */
int qce2204_ppe_port_scheduler_cfg_reset(struct net_device *netdev);

#endif /* __LINUX_DSA_QCE2204_API_H */
