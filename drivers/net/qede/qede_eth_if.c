/*
 * Copyright (c) 2016 QLogic Corporation.
 * All rights reserved.
 * www.qlogic.com
 *
 * See LICENSE.qede_pmd for copyright and licensing details.
 */

#include "qede_ethdev.h"

static int
qed_start_vport(struct ecore_dev *edev, struct qed_start_vport_params *p_params)
{
	int rc, i;

	for_each_hwfn(edev, i) {
		struct ecore_hwfn *p_hwfn = &edev->hwfns[i];
		u8 tx_switching = 0;
		struct ecore_sp_vport_start_params start = { 0 };

		start.tpa_mode = p_params->gro_enable ? ECORE_TPA_MODE_GRO :
		    ECORE_TPA_MODE_NONE;
		start.remove_inner_vlan = p_params->remove_inner_vlan;
		start.tx_switching = tx_switching;
		start.only_untagged = false;	/* untagged only */
		start.drop_ttl0 = p_params->drop_ttl0;
		start.concrete_fid = p_hwfn->hw_info.concrete_fid;
		start.opaque_fid = p_hwfn->hw_info.opaque_fid;
		start.concrete_fid = p_hwfn->hw_info.concrete_fid;
		start.handle_ptp_pkts = p_params->handle_ptp_pkts;
		start.vport_id = p_params->vport_id;
		start.max_buffers_per_cqe = 16;	/* TODO-is this right */
		start.mtu = p_params->mtu;
		/* @DPDK - Disable FW placement */
		start.zero_placement_offset = 1;

		rc = ecore_sp_vport_start(p_hwfn, &start);
		if (rc) {
			DP_ERR(edev, "Failed to start VPORT\n");
			return rc;
		}

		DP_VERBOSE(edev, ECORE_MSG_SPQ,
			   "Started V-PORT %d with MTU %d\n",
			   p_params->vport_id, p_params->mtu);
	}

	ecore_reset_vport_stats(edev);

	return 0;
}

static int qed_stop_vport(struct ecore_dev *edev, uint8_t vport_id)
{
	int rc, i;

	for_each_hwfn(edev, i) {
		struct ecore_hwfn *p_hwfn = &edev->hwfns[i];
		rc = ecore_sp_vport_stop(p_hwfn,
					 p_hwfn->hw_info.opaque_fid, vport_id);

		if (rc) {
			DP_ERR(edev, "Failed to stop VPORT\n");
			return rc;
		}
	}

	return 0;
}

bool qed_update_rss_parm_cmt(struct ecore_dev *edev, uint16_t *p_tbl)
{
	uint16_t max = 0, k;
	bool rss_mode = 0; /* disable */
	int divisor;

	/* Find largest entry, since it's possible RSS needs to
	 * be disabled [in case only 1 queue per-hwfn]
	 */
	for (k = 0; k < ECORE_RSS_IND_TABLE_SIZE; k++)
		max = (max > p_tbl[k]) ?  max : p_tbl[k];

	/* Either fix RSS values or disable RSS */
	if (edev->num_hwfns < max + 1) {
		divisor = (max + edev->num_hwfns - 1) / edev->num_hwfns;
		DP_VERBOSE(edev, ECORE_MSG_SPQ,
			   "CMT - fixing RSS values (modulo %02x)\n",
			   divisor);
		for (k = 0; k < ECORE_RSS_IND_TABLE_SIZE; k++)
			p_tbl[k] = p_tbl[k] % divisor;

		rss_mode = 1;
	}

	return rss_mode;
}

static int
qed_update_vport(struct ecore_dev *edev, struct qed_update_vport_params *params)
{
	struct ecore_sp_vport_update_params sp_params;
	struct ecore_rss_params sp_rss_params;
	int rc, i;

	memset(&sp_params, 0, sizeof(sp_params));
	memset(&sp_rss_params, 0, sizeof(sp_rss_params));

	/* Translate protocol params into sp params */
	sp_params.vport_id = params->vport_id;
	sp_params.update_vport_active_rx_flg = params->update_vport_active_flg;
	sp_params.update_vport_active_tx_flg = params->update_vport_active_flg;
	sp_params.vport_active_rx_flg = params->vport_active_flg;
	sp_params.vport_active_tx_flg = params->vport_active_flg;
	sp_params.update_inner_vlan_removal_flg =
	    params->update_inner_vlan_removal_flg;
	sp_params.inner_vlan_removal_flg = params->inner_vlan_removal_flg;
	sp_params.update_tx_switching_flg = params->update_tx_switching_flg;
	sp_params.tx_switching_flg = params->tx_switching_flg;
	sp_params.accept_any_vlan = params->accept_any_vlan;
	sp_params.update_accept_any_vlan_flg =
	    params->update_accept_any_vlan_flg;
	sp_params.mtu = params->mtu;

	for_each_hwfn(edev, i) {
		struct ecore_hwfn *p_hwfn = &edev->hwfns[i];

		sp_params.opaque_fid = p_hwfn->hw_info.opaque_fid;
		rc = ecore_sp_vport_update(p_hwfn, &sp_params,
					   ECORE_SPQ_MODE_EBLOCK, NULL);
		if (rc) {
			DP_ERR(edev, "Failed to update VPORT\n");
			return rc;
		}

		DP_VERBOSE(edev, ECORE_MSG_SPQ,
			   "Updated V-PORT %d: active_flag %d [update %d]\n",
			   params->vport_id, params->vport_active_flg,
			   params->update_vport_active_flg);
	}

	return 0;
}

static int
qed_start_rxq(struct ecore_dev *edev,
	      uint8_t rss_num,
	      struct ecore_queue_start_common_params *p_params,
	      uint16_t bd_max_bytes,
	      dma_addr_t bd_chain_phys_addr,
	      dma_addr_t cqe_pbl_addr,
	      uint16_t cqe_pbl_size, void OSAL_IOMEM * *pp_prod)
{
	struct ecore_hwfn *p_hwfn;
	int rc, hwfn_index;

	hwfn_index = rss_num % edev->num_hwfns;
	p_hwfn = &edev->hwfns[hwfn_index];

	p_params->queue_id = p_params->queue_id / edev->num_hwfns;
	p_params->stats_id = p_params->vport_id;

	rc = ecore_sp_eth_rx_queue_start(p_hwfn,
					 p_hwfn->hw_info.opaque_fid,
					 p_params,
					 bd_max_bytes,
					 bd_chain_phys_addr,
					 cqe_pbl_addr, cqe_pbl_size, pp_prod);

	if (rc) {
		DP_ERR(edev, "Failed to start RXQ#%d\n", p_params->queue_id);
		return rc;
	}

	DP_VERBOSE(edev, ECORE_MSG_SPQ,
		   "Started RX-Q %d [rss_num %d] on V-PORT %d and SB %d\n",
		   p_params->queue_id, rss_num, p_params->vport_id,
		   p_params->sb);

	return 0;
}

static int
qed_stop_rxq(struct ecore_dev *edev, struct qed_stop_rxq_params *params)
{
	int rc, hwfn_index;
	struct ecore_hwfn *p_hwfn;

	hwfn_index = params->rss_id % edev->num_hwfns;
	p_hwfn = &edev->hwfns[hwfn_index];

	rc = ecore_sp_eth_rx_queue_stop(p_hwfn,
					params->rx_queue_id / edev->num_hwfns,
					params->eq_completion_only, false);
	if (rc) {
		DP_ERR(edev, "Failed to stop RXQ#%d\n", params->rx_queue_id);
		return rc;
	}

	return 0;
}

static int
qed_start_txq(struct ecore_dev *edev,
	      uint8_t rss_num,
	      struct ecore_queue_start_common_params *p_params,
	      dma_addr_t pbl_addr,
	      uint16_t pbl_size, void OSAL_IOMEM * *pp_doorbell)
{
	struct ecore_hwfn *p_hwfn;
	int rc, hwfn_index;

	hwfn_index = rss_num % edev->num_hwfns;
	p_hwfn = &edev->hwfns[hwfn_index];

	p_params->queue_id = p_params->queue_id / edev->num_hwfns;
	p_params->qzone_id = p_params->queue_id;
	p_params->stats_id = p_params->vport_id;

	rc = ecore_sp_eth_tx_queue_start(p_hwfn,
					 p_hwfn->hw_info.opaque_fid,
					 p_params,
					 0 /* tc */,
					 pbl_addr, pbl_size, pp_doorbell);

	if (rc) {
		DP_ERR(edev, "Failed to start TXQ#%d\n", p_params->queue_id);
		return rc;
	}

	DP_VERBOSE(edev, ECORE_MSG_SPQ,
		   "Started TX-Q %d [rss_num %d] on V-PORT %d and SB %d\n",
		   p_params->queue_id, rss_num, p_params->vport_id,
		   p_params->sb);

	return 0;
}

static int
qed_stop_txq(struct ecore_dev *edev, struct qed_stop_txq_params *params)
{
	struct ecore_hwfn *p_hwfn;
	int rc, hwfn_index;

	hwfn_index = params->rss_id % edev->num_hwfns;
	p_hwfn = &edev->hwfns[hwfn_index];

	rc = ecore_sp_eth_tx_queue_stop(p_hwfn,
					params->tx_queue_id / edev->num_hwfns);
	if (rc) {
		DP_ERR(edev, "Failed to stop TXQ#%d\n", params->tx_queue_id);
		return rc;
	}

	return 0;
}

static int
qed_fp_cqe_completion(struct ecore_dev *edev,
		      uint8_t rss_id, struct eth_slow_path_rx_cqe *cqe)
{
	return ecore_eth_cqe_completion(&edev->hwfns[rss_id % edev->num_hwfns],
					cqe);
}

static int qed_fastpath_stop(struct ecore_dev *edev)
{
	ecore_hw_stop_fastpath(edev);

	return 0;
}

static void qed_fastpath_start(struct ecore_dev *edev)
{
	struct ecore_hwfn *p_hwfn;
	int i;

	for_each_hwfn(edev, i) {
		p_hwfn = &edev->hwfns[i];
		ecore_hw_start_fastpath(p_hwfn);
	}
}

static void
qed_get_vport_stats(struct ecore_dev *edev, struct ecore_eth_stats *stats)
{
	ecore_get_vport_stats(edev, stats);
}

int qed_configure_filter_rx_mode(struct rte_eth_dev *eth_dev,
				 enum qed_filter_rx_mode_type type)
{
	struct qede_dev *qdev = QEDE_INIT_QDEV(eth_dev);
	struct ecore_dev *edev = QEDE_INIT_EDEV(qdev);
	struct ecore_filter_accept_flags flags;

	memset(&flags, 0, sizeof(flags));

	flags.update_rx_mode_config = 1;
	flags.update_tx_mode_config = 1;
	flags.rx_accept_filter = ECORE_ACCEPT_UCAST_MATCHED |
					ECORE_ACCEPT_MCAST_MATCHED |
					ECORE_ACCEPT_BCAST;

	flags.tx_accept_filter = ECORE_ACCEPT_UCAST_MATCHED |
				 ECORE_ACCEPT_MCAST_MATCHED |
				 ECORE_ACCEPT_BCAST;

	if (type == QED_FILTER_RX_MODE_TYPE_PROMISC) {
		flags.rx_accept_filter |= ECORE_ACCEPT_UCAST_UNMATCHED;
		if (IS_VF(edev)) {
			flags.tx_accept_filter |= ECORE_ACCEPT_UCAST_UNMATCHED;
			DP_INFO(edev, "Enabling Tx unmatched flag for VF\n");
		}
	} else if (type == QED_FILTER_RX_MODE_TYPE_MULTI_PROMISC) {
		flags.rx_accept_filter |= ECORE_ACCEPT_MCAST_UNMATCHED;
	} else if (type == (QED_FILTER_RX_MODE_TYPE_MULTI_PROMISC |
			    QED_FILTER_RX_MODE_TYPE_PROMISC)) {
		flags.rx_accept_filter |= ECORE_ACCEPT_UCAST_UNMATCHED |
					  ECORE_ACCEPT_MCAST_UNMATCHED;
	}

	return ecore_filter_accept_cmd(edev, 0, flags, false, false,
				       ECORE_SPQ_MODE_CB, NULL);
}

static const struct qed_eth_ops qed_eth_ops_pass = {
	INIT_STRUCT_FIELD(common, &qed_common_ops_pass),
	INIT_STRUCT_FIELD(fill_dev_info, &qed_fill_eth_dev_info),
	INIT_STRUCT_FIELD(vport_start, &qed_start_vport),
	INIT_STRUCT_FIELD(vport_stop, &qed_stop_vport),
	INIT_STRUCT_FIELD(vport_update, &qed_update_vport),
	INIT_STRUCT_FIELD(q_rx_start, &qed_start_rxq),
	INIT_STRUCT_FIELD(q_tx_start, &qed_start_txq),
	INIT_STRUCT_FIELD(q_rx_stop, &qed_stop_rxq),
	INIT_STRUCT_FIELD(q_tx_stop, &qed_stop_txq),
	INIT_STRUCT_FIELD(eth_cqe_completion, &qed_fp_cqe_completion),
	INIT_STRUCT_FIELD(fastpath_stop, &qed_fastpath_stop),
	INIT_STRUCT_FIELD(fastpath_start, &qed_fastpath_start),
	INIT_STRUCT_FIELD(get_vport_stats, &qed_get_vport_stats),
};

const struct qed_eth_ops *qed_get_eth_ops(void)
{
	return &qed_eth_ops_pass;
}
