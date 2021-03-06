/*
 * Copyright (c) 2016 Hisilicon Limited.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <linux/platform_device.h>
#include <linux/etherdevice.h>
#include <rdma/ib_umem.h>
#include "hns_roce_common.h"
#include "hns_roce_device.h"
#include "hns_roce_cmd.h"
#include "hns_roce_hem.h"
#include "hns_roce_hw_v1.h"

#define BITS28_CMP_CHECK(a, b) (int)((((a) - (b)) & 0xFFFFFFF) << 4)
#define V1_SUPPORT_QP_TYPE(qp_type) (((qp_type) == IB_QPT_RC) || \
					((qp_type) == IB_QPT_UD))

#define DB_WAIT_TIMEOUT_VALUE	20
#define MR_FREE_TIMEOUT_MSECS	50000
#define SET_MAC_TIMEOUT_MSECS	10000
#define MR_FREE_WAIT_VALUE	5
#define SET_MAC_WAIT_VALUE	20

static int hns_roce_v1_query_qpc(struct hns_roce_dev *hr_dev,
				 struct hns_roce_qp *hr_qp,
				 struct hns_roce_qp_context *hr_context);
static int hns_roce_v1_set_mac_fh(struct hns_roce_dev *hr_dev);

static void hns_roce_v1_release_lp_qp(struct hns_roce_dev *hr_dev);
static struct hns_roce_qp *hns_roce_v1_create_lp_qp(struct hns_roce_dev *hr_dev,
						    struct ib_pd *pd);

static void set_data_seg(struct hns_roce_wqe_data_seg *dseg, struct ib_sge *sg)
{
	dseg->lkey = cpu_to_le32(sg->lkey);
	dseg->addr = cpu_to_le64(sg->addr);
	dseg->len  = cpu_to_le32(sg->length);
}

static void set_raddr_seg(struct hns_roce_wqe_raddr_seg *rseg, u64 remote_addr,
			  u32 rkey)
{
	rseg->raddr = cpu_to_le64(remote_addr);
	rseg->rkey  = cpu_to_le32(rkey);
}

static int set_wqe_data_seg(struct ib_qp *ibqp,
			struct hns_roce_wqe_ctrl_seg *ctrl,
			struct ib_send_wr *wr, void *wqe,
			struct ib_send_wr **bad_wr)
{
	struct hns_roce_dev *hr_dev = to_hr_dev(ibqp->device);
	struct hns_roce_wqe_data_seg *dseg = wqe;
	struct device *dev = &hr_dev->pdev->dev;
	int i = 0;

	if (wr->send_flags & IB_SEND_INLINE && wr->num_sge) {
		if (ctrl->msg_length > hr_dev->caps.max_sq_inline) {
			*bad_wr = wr;
			dev_err(dev, "Inline data len(1-32) = %d, illegal!\n",
				ctrl->msg_length);
			return -EINVAL;
		}

		for (i = 0; i < wr->num_sge; i++) {
			memcpy(wqe, ((void *)(uintptr_t)wr->sg_list[i].addr),
				wr->sg_list[i].length);
			wqe +=  wr->sg_list[i].length;
		}

		ctrl->flag |= HNS_ROCE_WQE_INLINE;
	} else {
		/*sqe num is two */
		for (i = 0; i < wr->num_sge; i++)
			set_data_seg(dseg + i, wr->sg_list + i);
		ctrl->flag |= cpu_to_le32((u32)wr->num_sge
				<< HNS_ROCE_WQE_SGE_NUM_BIT);
	}

	return 0;
}

static void set_wqe_remote_addr(struct hns_roce_wqe_ctrl_seg *ctrl,
		struct ib_send_wr *wr, struct hns_roce_wqe_raddr_seg *wqe)
{
	int ps_opcode = 0;

	switch (wr->opcode) {
	case IB_WR_RDMA_READ:
		ps_opcode = HNS_ROCE_WQE_OPCODE_RDMA_READ;
		set_raddr_seg(wqe, wr->wr.rdma.remote_addr,
			wr->wr.rdma.rkey);
		break;

	case IB_WR_RDMA_WRITE:
	case IB_WR_RDMA_WRITE_WITH_IMM:
		ps_opcode = HNS_ROCE_WQE_OPCODE_RDMA_WRITE;
		set_raddr_seg(wqe, wr->wr.rdma.remote_addr,
			wr->wr.rdma.rkey);
		break;

	case IB_WR_SEND:
	case IB_WR_SEND_WITH_INV:
	case IB_WR_SEND_WITH_IMM:
		ps_opcode = HNS_ROCE_WQE_OPCODE_SEND;
		break;
	case IB_WR_LOCAL_INV:
		break;
	case IB_WR_FAST_REG_MR:
		break;
	case IB_WR_ATOMIC_CMP_AND_SWP:
	case IB_WR_ATOMIC_FETCH_AND_ADD:
	case IB_WR_LSO:
	default:
		ps_opcode = HNS_ROCE_WQE_OPCODE_MASK;
		break;
	}

	ctrl->flag |= cpu_to_le32(ps_opcode);
}

static void set_wqe_ctrl_length(struct hns_roce_wqe_ctrl_seg *ctrl,
		struct ib_send_wr *wr)
{
	int i = 0;

	for (i = 0; i < wr->num_sge; i++)
		ctrl->msg_length += wr->sg_list[i].length;
}

#define HNS_ROCE_MAX_PSN_WINDOW (1 << 23)
#define HNS_ROCE_MAX_PSN (1 << 24)


static inline u32 __get_cur_total_psn(u32 head, u32 tail)
{
	u32 total;

	if (head >= tail)
		total = head - tail;
	else
		total = head + HNS_ROCE_MAX_PSN - tail;
	return total;
}

static inline u32 __get_cur_total_wqe(u32 head, u32 tail, u32 depth)
{
	u32 total;

	if (head >= tail)
		total = head - tail;
	else
		total = head + depth - tail;
	return total;
}

static int hns_roce_v1_check_psn(struct hns_roce_dev *hr_dev,
	struct hns_roce_qp *qp, u32 length)
{
	u32 pkt_num;
	u32 total_psn;
	u32 tail_offset;
	u32 sq_tail;
	u32 psn_tail;
	struct hns_roce_cq *hr_cq;
	struct hns_roce_qp_context *context;
	void *sq_wqe;
	struct hns_roce_wqe_raddr_seg *seg;
	unsigned int total_unpoll;
	unsigned int total_last;

	if (!qp->mtu) {
		dev_err(&hr_dev->pdev->dev, "qp mtu is invalid.\n");
		return -EINVAL;
	}

	total_psn = __get_cur_total_psn(qp->psn_head, qp->psn_tail);

	if (length)
		pkt_num = (length + qp->mtu - 1) / qp->mtu;
	else
		pkt_num = 1;

	if (total_psn + pkt_num > HNS_ROCE_MAX_PSN_WINDOW) {
		hr_cq = to_hr_cq(qp->ibqp.send_cq);
		spin_lock(&hr_cq->lock);
		/* sq empty */
		if (qp->sq.head == qp->sq.tail &&
			qp->psn_tail == qp->psn_head) {
			spin_unlock(&hr_cq->lock);
			goto out;
		}

		/* cqe has been polled after last update */
		total_unpoll = __get_cur_total_wqe(qp->sq.head, qp->sq.tail,
					qp->sq.wqe_cnt);
		total_last = __get_cur_total_wqe(qp->sq.head, qp->sq_last_wqe,
					qp->sq.wqe_cnt);
		if (total_unpoll < total_last) {
			/* get the tail sqe */
			sq_tail = (qp->sq.tail - 1) & (qp->sq.wqe_cnt - 1);
			sq_wqe = hns_roce_buf_offset(&qp->hr_buf,
				qp->sq.offset + (sq_tail << qp->sq.wqe_shift));
			seg = sq_wqe + sizeof(struct hns_roce_wqe_ctrl_seg);

			total_psn = __get_cur_total_psn(qp->psn_head, seg->psn);
			if (total_psn + pkt_num <= HNS_ROCE_MAX_PSN_WINDOW) {
				qp->psn_tail = seg->psn;
				qp->sq_last_wqe = qp->sq.tail;
				spin_unlock(&hr_cq->lock);
				goto out;
			}
		}
		spin_unlock(&hr_cq->lock);

		/* query qpc to get the tail point in chip */
		context = (struct hns_roce_qp_context *)(qp->context);
		if (hns_roce_v1_query_qpc(hr_dev, qp, context))
			return -EAGAIN;

		psn_tail = roce_get_field(context->qpc_bytes_140,
			QP_CONTEXT_QPC_BYTES_140_RETRY_MSG_FPKT_PSN_H_M,
			QP_CONTEXT_QPC_BYTES_140_RETRY_MSG_FPKT_PSN_H_S) << 8;
		psn_tail += roce_get_field(context->qpc_bytes_136,
			QP_CONTEXT_QPC_BYTES_136_RETRY_MSG_FPKT_PSN_L_M,
			QP_CONTEXT_QPC_BYTES_136_RETRY_MSG_FPKT_PSN_L_S);

		tail_offset = 0;
		while (tail_offset < (u32)qp->sq.wqe_cnt) {
			sq_tail = (qp->sq.tail - 1 + tail_offset) &
				(qp->sq.wqe_cnt - 1);
			sq_wqe = hns_roce_buf_offset(&qp->hr_buf,
				qp->sq.offset + (sq_tail << qp->sq.wqe_shift));
			seg = sq_wqe + sizeof(struct hns_roce_wqe_ctrl_seg);
			if (seg->psn == qp->psn_tail) {
				qp->sq_last_wqe = qp->sq.tail + tail_offset;
				break;
			}
			tail_offset++;
		}
		if (tail_offset >= (u32)qp->sq.wqe_cnt) {
			dev_err(&hr_dev->pdev->dev, "The psn of sq is wrong, please reset qp.\n");
			return -EFAULT;
		}
		qp->psn_tail = psn_tail;

		/* if total pkt > 8M, post send failed */
		total_psn = __get_cur_total_psn(qp->psn_head, qp->psn_tail);
		if (total_psn + pkt_num > HNS_ROCE_MAX_PSN_WINDOW) {
			dev_err(&hr_dev->pdev->dev, "The send window is full, please poll or wait.\n");
			return -EAGAIN;
		}
	}
out:
	qp->psn_head = (qp->psn_head + pkt_num) & (HNS_ROCE_MAX_PSN - 1);
	return 0;
}

int hns_roce_v1_post_send(struct ib_qp *ibqp, struct ib_send_wr *wr,
			  struct ib_send_wr **bad_wr)
{
	struct hns_roce_dev *hr_dev = to_hr_dev(ibqp->device);
	struct hns_roce_ah *ah = to_hr_ah(wr->wr.ud.ah);
	struct hns_roce_ud_send_wqe *ud_sq_wqe = NULL;
	struct hns_roce_wqe_ctrl_seg *ctrl = NULL;
	struct hns_roce_wqe_raddr_seg *seg;
	struct hns_roce_qp *qp = to_hr_qp(ibqp);
	struct device *dev = &hr_dev->pdev->dev;
	struct hns_roce_sq_db sq_db;
	unsigned long flags = 0;
	void *wqe = NULL;
	u32 doorbell[2];
	int nreq = 0;
	u32 ind = 0;
	int ret = 0;
	u8 *smac;
	int loopback;

	spin_lock_irqsave(&qp->sq.lock, flags);

	ind = qp->sq_next_wqe;
	for (nreq = 0; wr; ++nreq, wr = wr->next) {
		if (hns_roce_wq_overflow(&qp->sq, nreq, qp->ibqp.send_cq)) {
			dev_err(dev, "Post send of qp 0x%x overflow.Cur is 0x%x, nreq is 0x%x.\n",
				ibqp->qp_num, qp->sq.head - qp->sq.tail, nreq);
			*bad_wr = wr;
			ret = -EOVERFLOW;
			goto out;
		}

		if (unlikely(wr->num_sge > qp->sq.max_gs)) {
			dev_err(dev, "Sge num(%d) of post send error!\n",
				wr->num_sge);
			ret = -EINVAL;
			*bad_wr = wr;
			goto out;
		}

		wqe = get_send_wqe(qp, ind & (qp->sq.wqe_cnt - 1));
		if (!wqe) {
			dev_err(dev, "get send wqe failed\n");
			ret = -EINVAL;
			*bad_wr = wr;
			goto out;
		}
		qp->sq.wrid[(qp->sq.head + nreq) & (qp->sq.wqe_cnt - 1)] =
								      wr->wr_id;

		/* Corresponding to the RC and RD type wqe process separately */
		if (ibqp->qp_type == IB_QPT_GSI) {
			ud_sq_wqe = wqe;
			roce_set_field(ud_sq_wqe->dmac_h,
				       UD_SEND_WQE_U32_4_DMAC_0_M,
				       UD_SEND_WQE_U32_4_DMAC_0_S,
				       ah->av.mac[0]);
			roce_set_field(ud_sq_wqe->dmac_h,
				       UD_SEND_WQE_U32_4_DMAC_1_M,
				       UD_SEND_WQE_U32_4_DMAC_1_S,
				       ah->av.mac[1]);
			roce_set_field(ud_sq_wqe->dmac_h,
				       UD_SEND_WQE_U32_4_DMAC_2_M,
				       UD_SEND_WQE_U32_4_DMAC_2_S,
				       ah->av.mac[2]);
			roce_set_field(ud_sq_wqe->dmac_h,
				       UD_SEND_WQE_U32_4_DMAC_3_M,
				       UD_SEND_WQE_U32_4_DMAC_3_S,
				       ah->av.mac[3]);

			roce_set_field(ud_sq_wqe->u32_8,
				       UD_SEND_WQE_U32_8_DMAC_4_M,
				       UD_SEND_WQE_U32_8_DMAC_4_S,
				       ah->av.mac[4]);
			roce_set_field(ud_sq_wqe->u32_8,
				       UD_SEND_WQE_U32_8_DMAC_5_M,
				       UD_SEND_WQE_U32_8_DMAC_5_S,
				       ah->av.mac[5]);

			smac = (u8 *)hr_dev->dev_addr[qp->port];

			/* when dmac equals smac , it should loopback*/
			loopback = ether_addr_equal_unaligned(ah->av.mac,
								smac) ? 1 : 0;
			roce_set_bit(ud_sq_wqe->u32_8,
					UD_SEND_WQE_U32_8_LOOPBACK_INDICATOR_S,
					loopback);

			roce_set_field(ud_sq_wqe->u32_8,
				       UD_SEND_WQE_U32_8_OPERATION_TYPE_M,
				       UD_SEND_WQE_U32_8_OPERATION_TYPE_S,
				       HNS_ROCE_WQE_OPCODE_SEND);
			roce_set_field(ud_sq_wqe->u32_8,
				       UD_SEND_WQE_U32_8_NUMBER_OF_DATA_SEG_M,
				       UD_SEND_WQE_U32_8_NUMBER_OF_DATA_SEG_S,
				       2);
			roce_set_bit(ud_sq_wqe->u32_8,
				UD_SEND_WQE_U32_8_SEND_GL_ROUTING_HDR_FLAG_S,
				1);

			ud_sq_wqe->u32_8 |= (wr->send_flags & IB_SEND_SIGNALED ?
				cpu_to_le32(HNS_ROCE_WQE_CQ_NOTIFY) : 0) |
				(wr->send_flags & IB_SEND_SOLICITED ?
				cpu_to_le32(HNS_ROCE_WQE_SE) : 0) |
				((wr->opcode == IB_WR_SEND_WITH_IMM) ?
				cpu_to_le32(HNS_ROCE_WQE_IMM) : 0);

			roce_set_field(ud_sq_wqe->u32_16,
				       UD_SEND_WQE_U32_16_DEST_QP_M,
				       UD_SEND_WQE_U32_16_DEST_QP_S,
				wr->wr.ud.remote_qpn);
			roce_set_field(ud_sq_wqe->u32_16,
				       UD_SEND_WQE_U32_16_MAX_STATIC_RATE_M,
				       UD_SEND_WQE_U32_16_MAX_STATIC_RATE_S,
				       ah->av.stat_rate);

			roce_set_field(ud_sq_wqe->u32_36,
				       UD_SEND_WQE_U32_36_FLOW_LABEL_M,
				       UD_SEND_WQE_U32_36_FLOW_LABEL_S, 0);
			roce_set_field(ud_sq_wqe->u32_36,
				       UD_SEND_WQE_U32_36_PRIORITY_M,
				       UD_SEND_WQE_U32_36_PRIORITY_S,
				       ah->av.sl_tclass_flowlabel >>
				       HNS_ROCE_SL_SHIFT);
			roce_set_field(ud_sq_wqe->u32_36,
				       UD_SEND_WQE_U32_36_SGID_INDEX_M,
				       UD_SEND_WQE_U32_36_SGID_INDEX_S,
				       hns_get_gid_index(hr_dev, qp->port,
							 ah->av.gid_index));

			roce_set_field(ud_sq_wqe->u32_40,
				       UD_SEND_WQE_U32_40_HOP_LIMIT_M,
				       UD_SEND_WQE_U32_40_HOP_LIMIT_S,
				       ah->av.hop_limit);
			roce_set_field(ud_sq_wqe->u32_40,
				       UD_SEND_WQE_U32_40_TRAFFIC_CLASS_M,
				       UD_SEND_WQE_U32_40_TRAFFIC_CLASS_S, 0);

			memcpy(&ud_sq_wqe->dgid[0], &ah->av.dgid[0], GID_LEN);

			ud_sq_wqe->va0_l = (u32)wr->sg_list[0].addr;
			ud_sq_wqe->va0_h = (wr->sg_list[0].addr) >> 32;
			ud_sq_wqe->l_key0 = wr->sg_list[0].lkey;

			ud_sq_wqe->va1_l = (u32)wr->sg_list[1].addr;
			ud_sq_wqe->va1_h = (wr->sg_list[1].addr) >> 32;
			ud_sq_wqe->l_key1 = wr->sg_list[1].lkey;
			ind++;
		} else if (ibqp->qp_type == IB_QPT_RC) {
			ctrl = wqe;
			memset(ctrl, 0, sizeof(struct hns_roce_wqe_ctrl_seg));
			set_wqe_ctrl_length(ctrl, wr);
			ctrl->sgl_pa_h = 0;
			ctrl->flag = 0;
			ctrl->imm_data = send_ieth(wr);

			/*Ctrl field, ctrl set type: sig, solic, imm, fence */
			/* SO wait for conforming application scenarios */
			ctrl->flag |= (wr->send_flags & IB_SEND_SIGNALED ?
				      cpu_to_le32(HNS_ROCE_WQE_CQ_NOTIFY) : 0) |
				      (wr->send_flags & IB_SEND_SOLICITED ?
				      cpu_to_le32(HNS_ROCE_WQE_SE) : 0) |
				      ((wr->opcode == IB_WR_SEND_WITH_IMM ||
				      wr->opcode == IB_WR_RDMA_WRITE_WITH_IMM) ?
				      cpu_to_le32(HNS_ROCE_WQE_IMM) : 0) |
				      (wr->send_flags & IB_SEND_FENCE ?
				      (cpu_to_le32(HNS_ROCE_WQE_FENCE)) : 0);

			wqe += sizeof(struct hns_roce_wqe_ctrl_seg);

			set_wqe_remote_addr(ctrl, wr, wqe);
			seg = wqe;

			wqe += sizeof(struct hns_roce_wqe_raddr_seg);
			ret = set_wqe_data_seg(ibqp, ctrl, wr, wqe, bad_wr);
			if (ret)
				goto out;

			ret = hns_roce_v1_check_psn(hr_dev, qp,
				ctrl->msg_length);
			if (ret) {
				*bad_wr = wr;
				goto out;
			}
			seg->psn = qp->psn_head;

			ind++;
		} else {
			dev_err(dev, "unSupported QP type(%d)!\n",
				ibqp->qp_type);
			ret = -EOPNOTSUPP;
			*bad_wr = wr;
			goto out;
		}
	}

out:
	/* Set DB return */
	if (likely(nreq)) {
		qp->sq.head += nreq;
		/* Memory barrier */
		wmb();

		sq_db.u32_4 = 0;
		sq_db.u32_8 = 0;
		roce_set_field(sq_db.u32_4, SQ_DOORBELL_U32_4_SQ_HEAD_M,
			       SQ_DOORBELL_U32_4_SQ_HEAD_S,
			      (qp->sq.head & ((qp->sq.wqe_cnt << 1) - 1)));
		roce_set_field(sq_db.u32_4, SQ_DOORBELL_U32_4_SL_M,
			       SQ_DOORBELL_U32_4_SL_S, qp->sl);
		roce_set_field(sq_db.u32_4, SQ_DOORBELL_U32_4_PORT_M,
			       SQ_DOORBELL_U32_4_PORT_S, qp->port);
		roce_set_field(sq_db.u32_8, SQ_DOORBELL_U32_8_QPN_M,
			       SQ_DOORBELL_U32_8_QPN_S, qp->doorbell_qpn);
		roce_set_bit(sq_db.u32_8, SQ_DOORBELL_HW_SYNC_S, 1);

		doorbell[0] = sq_db.u32_4;
		doorbell[1] = sq_db.u32_8;

		hns_roce_write64_k(doorbell, qp->sq.db_reg_l);
		qp->sq_next_wqe = ind;
	}

	spin_unlock_irqrestore(&qp->sq.lock, flags);

	return ret;
}

int hns_roce_v1_post_recv(struct ib_qp *ibqp, struct ib_recv_wr *wr,
			  struct ib_recv_wr **bad_wr)
{
	int ret = 0;
	int nreq = 0;
	int ind = 0;
	int i = 0;
	u32 reg_val = 0;
	unsigned long flags = 0;
	struct hns_roce_rq_wqe_ctrl *ctrl = NULL;
	struct hns_roce_wqe_data_seg *scat = NULL;
	struct hns_roce_qp *hr_qp = to_hr_qp(ibqp);
	struct hns_roce_dev *hr_dev = to_hr_dev(ibqp->device);
	struct device *dev = &hr_dev->pdev->dev;
	struct hns_roce_rq_db rq_db;
	uint32_t doorbell[2] = {0};

	spin_lock_irqsave(&hr_qp->rq.lock, flags);
	ind = hr_qp->rq.head & (hr_qp->rq.wqe_cnt - 1);

	for (nreq = 0; wr; ++nreq, wr = wr->next) {
		if (hns_roce_wq_overflow(&hr_qp->rq, nreq,
			hr_qp->ibqp.recv_cq)) {
			dev_err(dev, "Post recv of qp 0x%x overflow. Cur is 0x%x, nreq is 0x%x.\n",
				ibqp->qp_num,
				hr_qp->rq.head - hr_qp->rq.tail, nreq);
			ret = -EOVERFLOW;
			*bad_wr = wr;
			goto out;
		}

		if (unlikely(wr->num_sge > hr_qp->rq.max_gs)) {
			dev_err(dev, "Sge num(%d) of post recv error!\n",
				wr->num_sge);
			ret = -EINVAL;
			*bad_wr = wr;
			goto out;
		}

		ctrl = get_recv_wqe(hr_qp, ind);
		if (!ctrl) {
			dev_err(dev, "Get recv wqe failed!\n");
			ret = -EINVAL;
			*bad_wr = wr;
			goto out;
		}

		roce_set_field(ctrl->rwqe_byte_12,
			       RQ_WQE_CTRL_RWQE_BYTE_12_RWQE_SGE_NUM_M,
			       RQ_WQE_CTRL_RWQE_BYTE_12_RWQE_SGE_NUM_S,
			       wr->num_sge);

		scat = (struct hns_roce_wqe_data_seg *)(ctrl + 1);

		for (i = 0; i < wr->num_sge; i++)
			set_data_seg(scat + i, wr->sg_list + i);

		hr_qp->rq.wrid[ind] = wr->wr_id;

		ind = (ind + 1) & (hr_qp->rq.wqe_cnt - 1);
	}

out:
	if (likely(nreq)) {
		hr_qp->rq.head += nreq;
		/* Memory barrier */
		wmb();

		if (ibqp->qp_type == IB_QPT_GSI) {
			/* SW update GSI rq header */
			reg_val = roce_read(to_hr_dev(ibqp->device),
					    ROCEE_QP1C_CFG3_0_REG +
					    QP1C_CFGN_OFFSET * hr_qp->port);
			roce_set_field(reg_val,
				       ROCEE_QP1C_CFG3_0_ROCEE_QP1C_RQ_HEAD_M,
				       ROCEE_QP1C_CFG3_0_ROCEE_QP1C_RQ_HEAD_S,
				       hr_qp->rq.head);
			roce_write(to_hr_dev(ibqp->device),
				   ROCEE_QP1C_CFG3_0_REG +
				   QP1C_CFGN_OFFSET * hr_qp->port, reg_val);
		} else {
			rq_db.u32_4 = 0;
			rq_db.u32_8 = 0;

			roce_set_field(rq_db.u32_4, RQ_DOORBELL_U32_4_RQ_HEAD_M,
				       RQ_DOORBELL_U32_4_RQ_HEAD_S,
				       hr_qp->rq.head);
			roce_set_field(rq_db.u32_8, RQ_DOORBELL_U32_8_QPN_M,
				       RQ_DOORBELL_U32_8_QPN_S, hr_qp->qpn);
			roce_set_field(rq_db.u32_8, RQ_DOORBELL_U32_8_CMD_M,
				       RQ_DOORBELL_U32_8_CMD_S, 1);
			roce_set_bit(rq_db.u32_8, RQ_DOORBELL_U32_8_HW_SYNC_S,
				     1);

			doorbell[0] = rq_db.u32_4;
			doorbell[1] = rq_db.u32_8;

			hns_roce_write64_k(doorbell, hr_qp->rq.db_reg_l);
		}
	}
	spin_unlock_irqrestore(&hr_qp->rq.lock, flags);

	return ret;
}

static void hns_roce_set_db_event_mode(struct hns_roce_dev *hr_dev,
				       int sdb_mode, int odb_mode)
{
	u32 val;

	val = roce_read(hr_dev, ROCEE_GLB_CFG_REG);
	roce_set_bit(val, ROCEE_GLB_CFG_ROCEE_DB_SQ_MODE_S, sdb_mode);
	roce_set_bit(val, ROCEE_GLB_CFG_ROCEE_DB_OTH_MODE_S, odb_mode);
	roce_write(hr_dev, ROCEE_GLB_CFG_REG, val);
}

static void hns_roce_set_db_ext_mode(struct hns_roce_dev *hr_dev, u32 sdb_mode,
				     u32 odb_mode)
{
	u32 val;

	/* Configure SDB/ODB extend mode */
	val = roce_read(hr_dev, ROCEE_GLB_CFG_REG);
	roce_set_bit(val, ROCEE_GLB_CFG_SQ_EXT_DB_MODE_S, sdb_mode);
	roce_set_bit(val, ROCEE_GLB_CFG_OTH_EXT_DB_MODE_S, odb_mode);
	roce_write(hr_dev, ROCEE_GLB_CFG_REG, val);
}

static void hns_roce_set_sdb(struct hns_roce_dev *hr_dev, u32 sdb_alept,
			     u32 sdb_alful)
{
	u32 val;

	/* Configure SDB */
	val = roce_read(hr_dev, ROCEE_DB_SQ_WL_REG);
	roce_set_field(val, ROCEE_DB_SQ_WL_ROCEE_DB_SQ_WL_M,
		       ROCEE_DB_SQ_WL_ROCEE_DB_SQ_WL_S, sdb_alful);
	roce_set_field(val, ROCEE_DB_SQ_WL_ROCEE_DB_SQ_WL_EMPTY_M,
		       ROCEE_DB_SQ_WL_ROCEE_DB_SQ_WL_EMPTY_S, sdb_alept);
	roce_write(hr_dev, ROCEE_DB_SQ_WL_REG, val);
}

static void hns_roce_set_odb(struct hns_roce_dev *hr_dev, u32 odb_alept,
			     u32 odb_alful)
{
	u32 val;

	/* Configure ODB */
	val = roce_read(hr_dev, ROCEE_DB_OTHERS_WL_REG);
	roce_set_field(val, ROCEE_DB_OTHERS_WL_ROCEE_DB_OTH_WL_M,
		       ROCEE_DB_OTHERS_WL_ROCEE_DB_OTH_WL_S, odb_alful);
	roce_set_field(val, ROCEE_DB_OTHERS_WL_ROCEE_DB_OTH_WL_EMPTY_M,
		       ROCEE_DB_OTHERS_WL_ROCEE_DB_OTH_WL_EMPTY_S, odb_alept);
	roce_write(hr_dev, ROCEE_DB_OTHERS_WL_REG, val);
}

static void hns_roce_set_sdb_ext(struct hns_roce_dev *hr_dev, u32 ext_sdb_alept,
				 u32 ext_sdb_alful)
{
	struct device *dev = &hr_dev->pdev->dev;
	struct hns_roce_v1_priv *priv;
	struct hns_roce_db_table *db;
	dma_addr_t sdb_dma_addr;
	u32 val;

	priv = (struct hns_roce_v1_priv *)hr_dev->hw->priv;
	db = &priv->db_table;

	/* Configure extend SDB threshold */
	roce_write(hr_dev, ROCEE_EXT_DB_SQ_WL_EMPTY_REG, ext_sdb_alept);
	roce_write(hr_dev, ROCEE_EXT_DB_SQ_WL_REG, ext_sdb_alful);

	/* Configure extend SDB base addr */
	sdb_dma_addr = db->ext_db->sdb_buf_list->map;
	roce_write(hr_dev, ROCEE_EXT_DB_SQ_REG, (u32)(sdb_dma_addr >> 12));

	/* Configure extend SDB depth */
	val = roce_read(hr_dev, ROCEE_EXT_DB_SQ_H_REG);
	roce_set_field(val, ROCEE_EXT_DB_SQ_H_EXT_DB_SQ_SHIFT_M,
		       ROCEE_EXT_DB_SQ_H_EXT_DB_SQ_SHIFT_S,
		       db->ext_db->esdb_dep);
	/*
	 * 44 = 32 + 12, When evaluating addr to hardware, shift 12 because of
	 * using 4K page, and shift more 32 because of
	 * caculating the high 32 bit value evaluated to hardware.
	 */
	roce_set_field(val, ROCEE_EXT_DB_SQ_H_EXT_DB_SQ_BA_H_M,
		       ROCEE_EXT_DB_SQ_H_EXT_DB_SQ_BA_H_S, sdb_dma_addr >> 44);
	roce_write(hr_dev, ROCEE_EXT_DB_SQ_H_REG, val);

	dev_dbg(dev, "ext SDB depth: 0x%x\n", db->ext_db->esdb_dep);
	dev_dbg(dev, "ext SDB threshold: epmty: 0x%x, ful: 0x%x\n",
		ext_sdb_alept, ext_sdb_alful);
}

static void hns_roce_set_odb_ext(struct hns_roce_dev *hr_dev, u32 ext_odb_alept,
				 u32 ext_odb_alful)
{
	struct device *dev = &hr_dev->pdev->dev;
	struct hns_roce_v1_priv *priv;
	struct hns_roce_db_table *db;
	dma_addr_t odb_dma_addr;
	u32 val;

	priv = (struct hns_roce_v1_priv *)hr_dev->hw->priv;
	db = &priv->db_table;

	/* Configure extend ODB threshold */
	roce_write(hr_dev, ROCEE_EXT_DB_OTHERS_WL_EMPTY_REG, ext_odb_alept);
	roce_write(hr_dev, ROCEE_EXT_DB_OTHERS_WL_REG, ext_odb_alful);

	/* Configure extend ODB base addr */
	odb_dma_addr = db->ext_db->odb_buf_list->map;
	roce_write(hr_dev, ROCEE_EXT_DB_OTH_REG, (u32)(odb_dma_addr >> 12));

	/* Configure extend ODB depth */
	val = roce_read(hr_dev, ROCEE_EXT_DB_OTH_H_REG);
	roce_set_field(val, ROCEE_EXT_DB_OTH_H_EXT_DB_OTH_SHIFT_M,
		       ROCEE_EXT_DB_OTH_H_EXT_DB_OTH_SHIFT_S,
		       db->ext_db->eodb_dep);
	roce_set_field(val, ROCEE_EXT_DB_SQ_H_EXT_DB_OTH_BA_H_M,
		       ROCEE_EXT_DB_SQ_H_EXT_DB_OTH_BA_H_S,
		       db->ext_db->eodb_dep);
	roce_write(hr_dev, ROCEE_EXT_DB_OTH_H_REG, val);

	dev_dbg(dev, "ext ODB depth: 0x%x\n", db->ext_db->eodb_dep);
	dev_dbg(dev, "ext ODB threshold: empty: 0x%x, ful: 0x%x\n",
		ext_odb_alept, ext_odb_alful);
}

static int hns_roce_db_ext_init(struct hns_roce_dev *hr_dev, u32 sdb_ext_mod,
				u32 odb_ext_mod)
{
	struct device *dev = &hr_dev->pdev->dev;
	struct hns_roce_v1_priv *priv;
	struct hns_roce_db_table *db;
	dma_addr_t sdb_dma_addr;
	dma_addr_t odb_dma_addr;
	int ret = 0;

	priv = (struct hns_roce_v1_priv *)hr_dev->hw->priv;
	db = &priv->db_table;

	db->ext_db = kzalloc(sizeof(*db->ext_db), GFP_KERNEL);
	if (!db->ext_db)
		return -ENOMEM;

	if (sdb_ext_mod) {
		db->ext_db->sdb_buf_list = kzalloc(
				sizeof(*db->ext_db->sdb_buf_list), GFP_KERNEL);
		if (!db->ext_db->sdb_buf_list) {
			ret = -ENOMEM;
			goto ext_sdb_buf_fail_out;
		}

		db->ext_db->sdb_buf_list->buf = dma_alloc_coherent(dev,
						     HNS_ROCE_V1_EXT_SDB_SIZE,
						     &sdb_dma_addr, GFP_KERNEL);
		if (!db->ext_db->sdb_buf_list->buf) {
			ret = -ENOMEM;
			goto alloc_sq_db_buf_fail;
		}
		db->ext_db->sdb_buf_list->map = sdb_dma_addr;

		db->ext_db->esdb_dep = ilog2(HNS_ROCE_V1_EXT_SDB_DEPTH);
		hns_roce_set_sdb_ext(hr_dev, HNS_ROCE_V1_EXT_SDB_ALEPT,
				     HNS_ROCE_V1_EXT_SDB_ALFUL);
	} else
		hns_roce_set_sdb(hr_dev, HNS_ROCE_V1_SDB_ALEPT,
				 HNS_ROCE_V1_SDB_ALFUL);

	if (odb_ext_mod) {
		db->ext_db->odb_buf_list = kzalloc(
				sizeof(*db->ext_db->odb_buf_list), GFP_KERNEL);
		if (!db->ext_db->odb_buf_list) {
			ret = -ENOMEM;
			goto ext_odb_buf_fail_out;
		}

		db->ext_db->odb_buf_list->buf = dma_alloc_coherent(dev,
						     HNS_ROCE_V1_EXT_ODB_SIZE,
						     &odb_dma_addr, GFP_KERNEL);
		if (!db->ext_db->odb_buf_list->buf) {
			ret = -ENOMEM;
			goto alloc_otr_db_buf_fail;
		}
		db->ext_db->odb_buf_list->map = odb_dma_addr;

		db->ext_db->eodb_dep = ilog2(HNS_ROCE_V1_EXT_ODB_DEPTH);
		hns_roce_set_odb_ext(hr_dev, HNS_ROCE_V1_EXT_ODB_ALEPT,
				     HNS_ROCE_V1_EXT_ODB_ALFUL);
	} else
		hns_roce_set_odb(hr_dev, HNS_ROCE_V1_ODB_ALEPT,
				 HNS_ROCE_V1_ODB_ALFUL);

	hns_roce_set_db_ext_mode(hr_dev, sdb_ext_mod, odb_ext_mod);

	return 0;

alloc_otr_db_buf_fail:
	kfree(db->ext_db->odb_buf_list);

ext_odb_buf_fail_out:
	if (sdb_ext_mod) {
		dma_free_coherent(dev, HNS_ROCE_V1_EXT_SDB_SIZE,
				  db->ext_db->sdb_buf_list->buf,
				  db->ext_db->sdb_buf_list->map);
	}

alloc_sq_db_buf_fail:
	if (sdb_ext_mod)
		kfree(db->ext_db->sdb_buf_list);

ext_sdb_buf_fail_out:
	kfree(db->ext_db);
	return ret;
}

static int hns_roce_db_init(struct hns_roce_dev *hr_dev)
{
	struct device *dev = &hr_dev->pdev->dev;
	struct hns_roce_v1_priv *priv;
	struct hns_roce_db_table *db;
	u32 sdb_ext_mod;
	u32 odb_ext_mod;
	u32 sdb_evt_mod;
	u32 odb_evt_mod;
	int ret = 0;

	priv = (struct hns_roce_v1_priv *)hr_dev->hw->priv;
	db = &priv->db_table;

	memset(db, 0, sizeof(*db));

	/* Default DB mode */
	sdb_ext_mod = HNS_ROCE_SDB_EXTEND_MODE;
	odb_ext_mod = HNS_ROCE_ODB_EXTEND_MODE;
	sdb_evt_mod = HNS_ROCE_SDB_NORMAL_MODE;
	odb_evt_mod = HNS_ROCE_ODB_POLL_MODE;

	db->sdb_ext_mod = sdb_ext_mod;
	db->odb_ext_mod = odb_ext_mod;

	/* Init extend DB */
	ret = hns_roce_db_ext_init(hr_dev, sdb_ext_mod, odb_ext_mod);
	if (ret) {
		dev_err(dev, "Failed(%d) in extend DB configuration.\n", ret);
		return ret;
	}

	hns_roce_set_db_event_mode(hr_dev, sdb_evt_mod, odb_evt_mod);

	return 0;
}

static void hns_roce_db_free(struct hns_roce_dev *hr_dev)
{
	struct device *dev = &hr_dev->pdev->dev;
	struct hns_roce_v1_priv *priv;
	struct hns_roce_db_table *db;

	priv = (struct hns_roce_v1_priv *)hr_dev->hw->priv;
	db = &priv->db_table;

	if (db->sdb_ext_mod) {
		dma_free_coherent(dev, HNS_ROCE_V1_EXT_SDB_SIZE,
				  db->ext_db->sdb_buf_list->buf,
				  db->ext_db->sdb_buf_list->map);
		kfree(db->ext_db->sdb_buf_list);
	}

	if (db->odb_ext_mod) {
		dma_free_coherent(dev, HNS_ROCE_V1_EXT_ODB_SIZE,
				  db->ext_db->odb_buf_list->buf,
				  db->ext_db->odb_buf_list->map);
		kfree(db->ext_db->odb_buf_list);
	}

	kfree(db->ext_db);
}

static int hns_roce_raq_init(struct hns_roce_dev *hr_dev)
{
	int ret;
	int raq_shift = 0;
	dma_addr_t addr;
	u32 val;
	struct hns_roce_v1_priv *priv;
	struct hns_roce_raq_table *raq;
	struct device *dev = &hr_dev->pdev->dev;

	priv = (struct hns_roce_v1_priv *)hr_dev->hw->priv;
	raq = &priv->raq_table;

	raq->e_raq_buf = kzalloc(sizeof(*(raq->e_raq_buf)), GFP_KERNEL);
	if (!raq->e_raq_buf)
		return -ENOMEM;

	raq->e_raq_buf->buf = dma_alloc_coherent(dev, HNS_ROCE_V1_RAQ_SIZE,
						 &addr, GFP_KERNEL);
	if (!raq->e_raq_buf->buf) {
		ret = -ENOMEM;
		goto err_dma_alloc_raq;
	}
	raq->e_raq_buf->map = addr;

	/* Configure raq extended address. 48bit 4K align*/
	roce_write(hr_dev, ROCEE_EXT_RAQ_REG, raq->e_raq_buf->map >> 12);

	/* Configure raq_shift */
	raq_shift = ilog2(HNS_ROCE_V1_RAQ_SIZE / HNS_ROCE_V1_RAQ_ENTRY);
	val = roce_read(hr_dev, ROCEE_EXT_RAQ_H_REG);
	roce_set_field(val, ROCEE_EXT_RAQ_H_EXT_RAQ_SHIFT_M,
		       ROCEE_EXT_RAQ_H_EXT_RAQ_SHIFT_S, raq_shift);
	/*
	 * 44 = 32 + 12, When evaluating addr to hardware, shift 12 because of
	 * using 4K page, and shift more 32 because of
	 * caculating the high 32 bit value evaluated to hardware.
	 */
	roce_set_field(val, ROCEE_EXT_RAQ_H_EXT_RAQ_BA_H_M,
		       ROCEE_EXT_RAQ_H_EXT_RAQ_BA_H_S,
		       raq->e_raq_buf->map >> 44);
	roce_write(hr_dev, ROCEE_EXT_RAQ_H_REG, val);
	dev_dbg(dev, "Configure raq_shift 0x%x.\n", val);

	/* Configure raq threshold */
	val = roce_read(hr_dev, ROCEE_RAQ_WL_REG);
	roce_set_field(val, ROCEE_RAQ_WL_ROCEE_RAQ_WL_M,
		       ROCEE_RAQ_WL_ROCEE_RAQ_WL_S,
		       HNS_ROCE_V1_EXT_RAQ_WF);
	roce_write(hr_dev, ROCEE_RAQ_WL_REG, val);
	dev_dbg(dev, "Configure raq_wl 0x%x.\n", val);

	/* Enable extend raq */
	val = roce_read(hr_dev, ROCEE_WRMS_POL_TIME_INTERVAL_REG);
	roce_set_field(val,
		       ROCEE_WRMS_POL_TIME_INTERVAL_WRMS_POL_TIME_INTERVAL_M,
		       ROCEE_WRMS_POL_TIME_INTERVAL_WRMS_POL_TIME_INTERVAL_S,
		       POL_TIME_INTERVAL_VAL);
	roce_set_bit(val, ROCEE_WRMS_POL_TIME_INTERVAL_WRMS_EXT_RAQ_MODE, 1);
	roce_set_field(val,
		       ROCEE_WRMS_POL_TIME_INTERVAL_WRMS_RAQ_TIMEOUT_CHK_CFG_M,
		       ROCEE_WRMS_POL_TIME_INTERVAL_WRMS_RAQ_TIMEOUT_CHK_CFG_S,
		       2);
	roce_set_bit(val,
		     ROCEE_WRMS_POL_TIME_INTERVAL_WRMS_RAQ_TIMEOUT_CHK_EN_S, 1);
	roce_write(hr_dev, ROCEE_WRMS_POL_TIME_INTERVAL_REG, val);
	dev_dbg(dev, "Configure WrmsPolTimeInterval 0x%x.\n", val);

	/* Enable raq drop */
	val = roce_read(hr_dev, ROCEE_GLB_CFG_REG);
	roce_set_bit(val, ROCEE_GLB_CFG_TRP_RAQ_DROP_EN_S, 1);
	roce_write(hr_dev, ROCEE_GLB_CFG_REG, val);
	dev_dbg(dev, "Configure GlbCfg = 0x%x.\n", val);

	return 0;

err_dma_alloc_raq:
	kfree(raq->e_raq_buf);
	return ret;
}

static void hns_roce_raq_free(struct hns_roce_dev *hr_dev)
{
	struct device *dev = &hr_dev->pdev->dev;
	struct hns_roce_v1_priv *priv;
	struct hns_roce_raq_table *raq;

	priv = (struct hns_roce_v1_priv *)hr_dev->hw->priv;
	raq = &priv->raq_table;

	dma_free_coherent(dev, HNS_ROCE_V1_RAQ_SIZE, raq->e_raq_buf->buf,
			  raq->e_raq_buf->map);
	kfree(raq->e_raq_buf);
}

static void hns_roce_port_enable(struct hns_roce_dev *hr_dev, int enable_flag)
{
	u32 val;

	if (enable_flag) {
		val = roce_read(hr_dev, ROCEE_GLB_CFG_REG);
		 /* Open all ports */
		roce_set_field(val, ROCEE_GLB_CFG_ROCEE_PORT_ST_M,
			       ROCEE_GLB_CFG_ROCEE_PORT_ST_S,
			       ALL_PORT_VAL_OPEN);
		roce_write(hr_dev, ROCEE_GLB_CFG_REG, val);
	} else {
		val = roce_read(hr_dev, ROCEE_GLB_CFG_REG);
		/* Close all ports */
		roce_set_field(val, ROCEE_GLB_CFG_ROCEE_PORT_ST_M,
			       ROCEE_GLB_CFG_ROCEE_PORT_ST_S, 0x0);
		roce_write(hr_dev, ROCEE_GLB_CFG_REG, val);
	}
}

/*
 * hns_roce_v1_reset - reset roce
 * @hr_dev: roce device struct pointer
 * @enable: true -- drop reset, false -- reset
 * return 0 - success , negative --fail
 */
int hns_roce_v1_reset(struct hns_roce_dev *hr_dev, bool enable)
{
	struct device_node *dsaf_node;
	struct device *dev = &hr_dev->pdev->dev;
	struct device_node *np = dev->of_node;
	struct hns_roce_caps *caps = &hr_dev->caps;
	u32 channel_mode[HNS_ROCE_MAX_PORTS + 1] = {
		ROCE_PORT_MODE_INVAL,
		ROCE_PORT_MODE_INVAL,
		ROCE_2PORT_MODE,
		ROCE_PORT_MODE_INVAL,
		ROCE_4PORT_MODE,
		ROCE_PORT_MODE_INVAL,
		ROCE_6PORT_MODE
	};
	int ret;

	if (channel_mode[caps->num_ports] >= ROCE_PORT_MODE_INVAL) {
		dev_err(dev, "Port config illegal! port num now is %d\n",
				caps->num_ports);
		return -EINVAL;
	}

	dsaf_node = of_parse_phandle(np, "dsaf-handle", 0);
	if (!dsaf_node) {
		dev_err(dev, "Unable to get dsaf node by dsaf-handle!\n");
		return -EINVAL;
	}

	ret = hns_dsaf_roce_reset(&dsaf_node->fwnode,
				  channel_mode[caps->num_ports], 0);
	if (ret)
		return ret;

	if (enable) {
		msleep(SLEEP_TIME_INTERVAL);
		return hns_dsaf_roce_reset(&dsaf_node->fwnode,
					   channel_mode[caps->num_ports], 1);
	}

	return 0;
}

void hns_roce_v1_profile(struct hns_roce_dev *hr_dev)
{
	int i = 0;
	struct hns_roce_caps *caps = &hr_dev->caps;

	hr_dev->vendor_id = le32_to_cpu(roce_read(hr_dev, ROCEE_VENDOR_ID_REG));
	hr_dev->vendor_part_id = le32_to_cpu(roce_read(hr_dev,
					     ROCEE_VENDOR_PART_ID_REG));
	hr_dev->hw_rev = le32_to_cpu(roce_read(hr_dev, ROCEE_HW_VERSION_REG));

	hr_dev->sys_image_guid = le32_to_cpu(roce_read(hr_dev,
					     ROCEE_SYS_IMAGE_GUID_L_REG)) |
				((u64)le32_to_cpu(roce_read(hr_dev,
					    ROCEE_SYS_IMAGE_GUID_H_REG)) << 32);

	caps->num_qps		= HNS_ROCE_V1_MAX_QP_NUM;
	caps->max_wqes		= HNS_ROCE_V1_MAX_WQE_NUM;
	caps->num_cqs		= HNS_ROCE_V1_MAX_CQ_NUM;
	caps->max_cqes		= HNS_ROCE_V1_MAX_CQE_NUM;
	caps->max_sq_sg		= HNS_ROCE_V1_SG_NUM;
	caps->max_rq_sg		= HNS_ROCE_V1_SG_NUM;
	caps->max_sq_inline	= HNS_ROCE_V1_INLINE_SIZE;
	caps->num_uars		= HNS_ROCE_V1_UAR_NUM;
	caps->phy_num_uars	= HNS_ROCE_V1_PHY_UAR_NUM;
	caps->num_aeq_vectors	= HNS_ROCE_AEQE_VEC_NUM;
	caps->num_comp_vectors	= HNS_ROCE_COMP_VEC_NUM;
	caps->num_other_vectors	= HNS_ROCE_AEQE_OF_VEC_NUM;
	caps->num_mtpts		= HNS_ROCE_V1_MAX_MTPT_NUM;
	caps->num_mtt_segs	= HNS_ROCE_V1_MAX_MTT_SEGS;
	caps->num_pds		= HNS_ROCE_V1_MAX_PD_NUM;
	caps->max_qp_init_rdma	= HNS_ROCE_V1_MAX_QP_INIT_RDMA;
	caps->max_qp_dest_rdma	= HNS_ROCE_V1_MAX_QP_DEST_RDMA;
	caps->max_sq_desc_sz	= HNS_ROCE_V1_MAX_SQ_DESC_SZ;
	caps->max_rq_desc_sz	= HNS_ROCE_V1_MAX_RQ_DESC_SZ;
	caps->qpc_entry_sz	= HNS_ROCE_V1_QPC_ENTRY_SIZE;
	caps->irrl_entry_sz	= HNS_ROCE_V1_IRRL_ENTRY_SIZE;
	caps->cqc_entry_sz	= HNS_ROCE_V1_CQC_ENTRY_SIZE;
	caps->mtpt_entry_sz	= HNS_ROCE_V1_MTPT_ENTRY_SIZE;
	caps->mtt_entry_sz	= HNS_ROCE_V1_MTT_ENTRY_SIZE;
	caps->cq_entry_sz	= HNS_ROCE_V1_CQE_ENTRY_SIZE;
	caps->page_size_cap	= HNS_ROCE_V1_PAGE_SIZE_SUPPORT;
	caps->sqp_start		= 0;
	caps->reserved_lkey	= 0;
	caps->reserved_pds	= 0;
	caps->reserved_mrws	= 1;
	caps->reserved_uars	= 0;
	caps->reserved_cqs	= 0;

	for (i = 1; i < caps->num_ports + 1; i++)
		caps->pkey_table_len[i] = 1;

	for (i = 0; i < caps->num_ports; i++) {
		/* Six ports shared 16 GID in v1 engine */
		if (i >= (HNS_ROCE_V1_GID_NUM % caps->num_ports))
			caps->gid_table_len[i] = HNS_ROCE_V1_GID_NUM /
						 caps->num_ports;
		else
			caps->gid_table_len[i] = HNS_ROCE_V1_GID_NUM /
						 caps->num_ports + 1;
	}

	for (i = 0; i < caps->num_comp_vectors; i++)
		caps->ceqe_depth[i] = HNS_ROCE_V1_NUM_COMP_EQE;

	caps->aeqe_depth = HNS_ROCE_V1_NUM_ASYNC_EQE;
	caps->local_ca_ack_delay = le32_to_cpu(roce_read(hr_dev,
							 ROCEE_ACK_DELAY_REG));
	caps->max_mtu = IB_MTU_2048;
}

void hns_roce_v1_set_gid(struct hns_roce_dev *hr_dev, u8 port, int gid_index,
			 union ib_gid *gid)
{
	u32 *p = NULL;
	u8 gid_idx = 0;

	gid_idx = hns_get_gid_index(hr_dev, port, gid_index);

	p = (u32 *)&gid->raw[0];
	roce_raw_write(*p, hr_dev->reg_base + ROCEE_PORT_GID_L_0_REG +
		       (HNS_ROCE_V1_GID_NUM * gid_idx));

	p = (u32 *)&gid->raw[4];
	roce_raw_write(*p, hr_dev->reg_base + ROCEE_PORT_GID_ML_0_REG +
		       (HNS_ROCE_V1_GID_NUM * gid_idx));

	p = (u32 *)&gid->raw[8];
	roce_raw_write(*p, hr_dev->reg_base + ROCEE_PORT_GID_MH_0_REG +
		       (HNS_ROCE_V1_GID_NUM * gid_idx));

	p = (u32 *)&gid->raw[0xc];
	roce_raw_write(*p, hr_dev->reg_base + ROCEE_PORT_GID_H_0_REG +
		       (HNS_ROCE_V1_GID_NUM * gid_idx));
}

void hns_roce_v1_set_mac(struct hns_roce_dev *hr_dev, u8 phy_port, u8 *addr)
{
	u32 reg_smac_l;
	u16 reg_smac_h;
	u16 *p_h;
	u32 *p;
	u32 val;
	int ret;

	/*When mac changed ,here we release and create reserved qp again*/
	ret = hns_roce_v1_set_mac_fh(hr_dev);
	if (ret)
		dev_err(&hr_dev->pdev->dev, "Set mac bh failed(%d)!\n", ret);

	p = (u32 *)(&addr[0]);
	reg_smac_l = *p;
	roce_raw_write(reg_smac_l, hr_dev->reg_base + ROCEE_SMAC_L_0_REG +
		       PHY_PORT_OFFSET * phy_port);

	val = roce_read(hr_dev,
			ROCEE_SMAC_H_0_REG + phy_port * PHY_PORT_OFFSET);
	p_h = (u16 *)(&addr[4]);
	reg_smac_h  = *p_h;
	roce_set_field(val, ROCEE_SMAC_H_ROCEE_SMAC_H_M,
		       ROCEE_SMAC_H_ROCEE_SMAC_H_S, reg_smac_h);
	roce_write(hr_dev, ROCEE_SMAC_H_0_REG + phy_port * PHY_PORT_OFFSET,
		   val);
}

void hns_roce_v1_set_mtu(struct hns_roce_dev  *hr_dev, u8 phy_port,
			 enum ib_mtu mtu)
{
	u32 val;

	val = roce_read(hr_dev,
			ROCEE_SMAC_H_0_REG + phy_port * PHY_PORT_OFFSET);
	roce_set_field(val, ROCEE_SMAC_H_ROCEE_PORT_MTU_M,
		       ROCEE_SMAC_H_ROCEE_PORT_MTU_S, mtu);
	roce_write(hr_dev, ROCEE_SMAC_H_0_REG + phy_port * PHY_PORT_OFFSET,
		   val);
}

int hns_roce_v1_write_mtpt(void *mb_buf, struct hns_roce_mr *mr,
			   unsigned long mtpt_idx)
{
	struct hns_roce_v1_mpt_entry *mpt_entry;
	struct scatterlist *sg;
	u64 *pages;
	int entry;
	int i;

	/* MPT filled into mailbox buf */
	mpt_entry = (struct hns_roce_v1_mpt_entry *)mb_buf;
	memset(mpt_entry, 0, sizeof(*mpt_entry));

	roce_set_field(mpt_entry->mpt_byte_4, MPT_BYTE_4_KEY_STATE_M,
		       MPT_BYTE_4_KEY_STATE_S, KEY_VALID);
	roce_set_field(mpt_entry->mpt_byte_4, MPT_BYTE_4_KEY_M,
		       MPT_BYTE_4_KEY_S, mr->key);
	roce_set_field(mpt_entry->mpt_byte_4, MPT_BYTE_4_PAGE_SIZE_M,
		       MPT_BYTE_4_PAGE_SIZE_S, MR_SIZE_4K);
	roce_set_bit(mpt_entry->mpt_byte_4, MPT_BYTE_4_MW_TYPE_S, 0);
	roce_set_bit(mpt_entry->mpt_byte_4, MPT_BYTE_4_MW_BIND_ENABLE_S,
		     (mr->access & IB_ACCESS_MW_BIND ? 1 : 0));
	roce_set_bit(mpt_entry->mpt_byte_4, MPT_BYTE_4_OWN_S, 0);
	roce_set_field(mpt_entry->mpt_byte_4, MPT_BYTE_4_MEMORY_LOCATION_TYPE_M,
		       MPT_BYTE_4_MEMORY_LOCATION_TYPE_S, mr->type);
	roce_set_bit(mpt_entry->mpt_byte_4, MPT_BYTE_4_REMOTE_ATOMIC_S, 0);
	roce_set_bit(mpt_entry->mpt_byte_4, MPT_BYTE_4_LOCAL_WRITE_S,
		     (mr->access & IB_ACCESS_LOCAL_WRITE ? 1 : 0));
	roce_set_bit(mpt_entry->mpt_byte_4, MPT_BYTE_4_REMOTE_WRITE_S,
		     (mr->access & IB_ACCESS_REMOTE_WRITE ? 1 : 0));
	roce_set_bit(mpt_entry->mpt_byte_4, MPT_BYTE_4_REMOTE_READ_S,
		     (mr->access & IB_ACCESS_REMOTE_READ ? 1 : 0));
	roce_set_bit(mpt_entry->mpt_byte_4, MPT_BYTE_4_REMOTE_INVAL_ENABLE_S,
		     0);
	roce_set_bit(mpt_entry->mpt_byte_4, MPT_BYTE_4_ADDRESS_TYPE_S, 0);

	roce_set_field(mpt_entry->mpt_byte_12, MPT_BYTE_12_PBL_ADDR_H_M,
		       MPT_BYTE_12_PBL_ADDR_H_S, 0);
	roce_set_field(mpt_entry->mpt_byte_12, MPT_BYTE_12_MW_BIND_COUNTER_M,
		       MPT_BYTE_12_MW_BIND_COUNTER_S, 0);

	mpt_entry->virt_addr_l = (u32)mr->iova;
	mpt_entry->virt_addr_h = (u32)(mr->iova >> 32);
	mpt_entry->length = (u32)mr->size;

	roce_set_field(mpt_entry->mpt_byte_28, MPT_BYTE_28_PD_M,
		       MPT_BYTE_28_PD_S, mr->pd);
	roce_set_field(mpt_entry->mpt_byte_28, MPT_BYTE_28_L_KEY_IDX_L_M,
		       MPT_BYTE_28_L_KEY_IDX_L_S, mtpt_idx);
	roce_set_field(mpt_entry->mpt_byte_64, MPT_BYTE_64_L_KEY_IDX_H_M,
		       MPT_BYTE_64_L_KEY_IDX_H_S, mtpt_idx >> MTPT_IDX_SHIFT);

	/* DMA momery regsiter */
	if (mr->type == MR_TYPE_DMA)
		return 0;

	pages = (u64 *) __get_free_page(GFP_KERNEL);
	if (!pages)
		return -ENOMEM;

	i = 0;
	for_each_sg(mr->umem->sg_head.sgl, sg, mr->umem->nmap, entry) {
		pages[i] = ((u64)sg_dma_address(sg)) >> 12;

		/* Directly record to MTPT table firstly 7 entry */
		if (i >= HNS_ROCE_MAX_INNER_MTPT_NUM)
			break;
		i++;
	}

	/* Register user mr */
	for (i = 0; i < HNS_ROCE_MAX_INNER_MTPT_NUM; i++) {
		switch (i) {
		case 0:
			mpt_entry->pa0_l = cpu_to_le32((u32)(pages[i]));
			roce_set_field(mpt_entry->mpt_byte_36,
				MPT_BYTE_36_PA0_H_M,
				MPT_BYTE_36_PA0_H_S,
				cpu_to_le32((u32)(pages[i] >> PAGES_SHIFT_32)));
			break;
		case 1:
			roce_set_field(mpt_entry->mpt_byte_36,
				       MPT_BYTE_36_PA1_L_M,
				       MPT_BYTE_36_PA1_L_S,
				       cpu_to_le32((u32)(pages[i])));
			roce_set_field(mpt_entry->mpt_byte_40,
				MPT_BYTE_40_PA1_H_M,
				MPT_BYTE_40_PA1_H_S,
				cpu_to_le32((u32)(pages[i] >> PAGES_SHIFT_24)));
			break;
		case 2:
			roce_set_field(mpt_entry->mpt_byte_40,
				       MPT_BYTE_40_PA2_L_M,
				       MPT_BYTE_40_PA2_L_S,
				       cpu_to_le32((u32)(pages[i])));
			roce_set_field(mpt_entry->mpt_byte_44,
				MPT_BYTE_44_PA2_H_M,
				MPT_BYTE_44_PA2_H_S,
				cpu_to_le32((u32)(pages[i] >> PAGES_SHIFT_16)));
			break;
		case 3:
			roce_set_field(mpt_entry->mpt_byte_44,
				       MPT_BYTE_44_PA3_L_M,
				       MPT_BYTE_44_PA3_L_S,
				       cpu_to_le32((u32)(pages[i])));
			roce_set_field(mpt_entry->mpt_byte_48,
				MPT_BYTE_48_PA3_H_M,
				MPT_BYTE_48_PA3_H_S,
				cpu_to_le32((u32)(pages[i] >> PAGES_SHIFT_8)));
			break;
		case 4:
			mpt_entry->pa4_l = cpu_to_le32((u32)(pages[i]));
			roce_set_field(mpt_entry->mpt_byte_56,
				MPT_BYTE_56_PA4_H_M,
				MPT_BYTE_56_PA4_H_S,
				cpu_to_le32((u32)(pages[i] >> PAGES_SHIFT_32)));
			break;
		case 5:
			roce_set_field(mpt_entry->mpt_byte_56,
				       MPT_BYTE_56_PA5_L_M,
				       MPT_BYTE_56_PA5_L_S,
				       cpu_to_le32((u32)(pages[i])));
			roce_set_field(mpt_entry->mpt_byte_60,
				MPT_BYTE_60_PA5_H_M,
				MPT_BYTE_60_PA5_H_S,
				cpu_to_le32((u32)(pages[i] >> PAGES_SHIFT_24)));
			break;
		case 6:
			roce_set_field(mpt_entry->mpt_byte_60,
				       MPT_BYTE_60_PA6_L_M,
				       MPT_BYTE_60_PA6_L_S,
				       cpu_to_le32((u32)(pages[i])));
			roce_set_field(mpt_entry->mpt_byte_64,
				MPT_BYTE_64_PA6_H_M,
				MPT_BYTE_64_PA6_H_S,
				cpu_to_le32((u32)(pages[i] >> PAGES_SHIFT_16)));
			break;
		default:
			break;
		}
	}

	free_page((unsigned long) pages);

	mpt_entry->pbl_addr_l = (u32)(mr->pbl_dma_addr);

	roce_set_field(mpt_entry->mpt_byte_12, MPT_BYTE_12_PBL_ADDR_H_M,
		       MPT_BYTE_12_PBL_ADDR_H_S,
		       ((u32)(mr->pbl_dma_addr >> 32)));

	return 0;
}

static void *get_cqe(struct hns_roce_cq *hr_cq, int n)
{
	return hns_roce_buf_offset(&hr_cq->hr_buf.hr_buf,
				   n * HNS_ROCE_V1_CQE_ENTRY_SIZE);
}

static void *get_sw_cqe(struct hns_roce_cq *hr_cq, int n)
{
	struct hns_roce_cqe *hr_cqe = get_cqe(hr_cq, n & hr_cq->ib_cq.cqe);

	/* Get cqe when Owner bit is Conversely with the MSB of cons_idx */
	return (roce_get_bit(hr_cqe->cqe_byte_4, CQE_BYTE_4_OWNER_S) ^
		!!(n & (hr_cq->ib_cq.cqe + 1))) ? hr_cqe : NULL;
}

static struct hns_roce_cqe *next_cqe_sw(struct hns_roce_cq *hr_cq)
{
	return get_sw_cqe(hr_cq, hr_cq->cons_index);
}

void hns_roce_v1_cq_set_ci(struct hns_roce_cq *hr_cq, u32 cons_index)

{
	u32 doorbell[2];

	doorbell[0] = cons_index & ((hr_cq->cq_depth << 1) - 1);
	roce_set_bit(doorbell[1], ROCEE_DB_OTHERS_H_ROCEE_DB_OTH_HW_SYNS_S, 1);
	roce_set_field(doorbell[1], ROCEE_DB_OTHERS_H_ROCEE_DB_OTH_CMD_M,
		       ROCEE_DB_OTHERS_H_ROCEE_DB_OTH_CMD_S, 3);
	roce_set_field(doorbell[1], ROCEE_DB_OTHERS_H_ROCEE_DB_OTH_CMD_MDF_M,
		       ROCEE_DB_OTHERS_H_ROCEE_DB_OTH_CMD_MDF_S, 0);
	roce_set_field(doorbell[1], ROCEE_DB_OTHERS_H_ROCEE_DB_OTH_INP_H_M,
		       ROCEE_DB_OTHERS_H_ROCEE_DB_OTH_INP_H_S, hr_cq->cqn);

	hns_roce_write64_k(doorbell, hr_cq->cq_db_l);
}

static void hns_roce_v1_k_clean_cq(struct hns_roce_cq *hr_cq, u32 qpn,
				 struct hns_roce_srq *srq)
{
	struct hns_roce_cqe *cqe, *dest;
	u32 prod_index;
	int nfreed = 0;
	u8 owner_bit;

	for (prod_index = hr_cq->cons_index; get_sw_cqe(hr_cq, prod_index);
		++prod_index) {
		if (prod_index == hr_cq->cons_index + hr_cq->ib_cq.cqe)
			break;
	}

	/*
	* Now backwards through the CQ, removing CQ entries
	* that match our QP by overwriting them with next entries.
	*/
	while ((int) --prod_index - (int) hr_cq->cons_index >= 0) {
		cqe = get_cqe(hr_cq, prod_index & hr_cq->ib_cq.cqe);
		if ((roce_get_field(cqe->cqe_byte_16, CQE_BYTE_16_LOCAL_QPN_M,
					CQE_BYTE_16_LOCAL_QPN_S) &
					HNS_ROCE_CQE_QPN_MASK) == qpn) {
			/* In v1 engine, not support SRQ */
			++nfreed;
		} else if (nfreed) {
			dest = get_cqe(hr_cq, (prod_index + nfreed) &
					hr_cq->ib_cq.cqe);
			owner_bit = roce_get_bit(dest->cqe_byte_4,
					CQE_BYTE_4_OWNER_S);
			memcpy(dest, cqe, sizeof(*cqe));
			roce_set_bit(dest->cqe_byte_4, CQE_BYTE_4_OWNER_S,
					owner_bit);
		}
	}

	if (nfreed) {
		hr_cq->cons_index += nfreed;
		/*
		* Make sure update of buffer contents is done before
		* updating consumer index.
		*/
		wmb();

		hns_roce_v1_cq_set_ci(hr_cq, hr_cq->cons_index);
	}
}

static void hns_roce_v1_clean_cq(struct hns_roce_cq *send_cq,
		struct hns_roce_cq *recv_cq,
		struct hns_roce_qp *hr_qp, int is_user)
{
	if (!is_user) {
		hns_roce_v1_k_clean_cq(recv_cq, hr_qp->doorbell_qpn,
			hr_qp->ibqp.srq ? to_hr_srq(hr_qp->ibqp.srq) : NULL);
		if (send_cq != recv_cq)
			hns_roce_v1_k_clean_cq(send_cq, hr_qp->doorbell_qpn,
				NULL);
	}
}

static void hns_roce_v1_cq_clean(struct hns_roce_cq *hr_cq, u32 qpn,
				 struct hns_roce_srq *srq)
{
	spin_lock_irq(&hr_cq->lock);
	hns_roce_v1_k_clean_cq(hr_cq, qpn, srq);
	spin_unlock_irq(&hr_cq->lock);
}

void hns_roce_v1_write_cqc(struct hns_roce_dev *hr_dev,
			   struct hns_roce_cq *hr_cq, void *mb_buf, u64 *mtts,
			   dma_addr_t dma_handle, int nent, u32 vector)
{
	struct hns_roce_cq_context *cq_context = NULL;

	cq_context = mb_buf;
	memset(cq_context, 0, sizeof(*cq_context));

	/* Register cq_context members */
	roce_set_field(cq_context->cqc_byte_4,
		       CQ_CONTEXT_CQC_BYTE_4_CQC_STATE_M,
		       CQ_CONTEXT_CQC_BYTE_4_CQC_STATE_S, CQ_STATE_VALID);
	roce_set_field(cq_context->cqc_byte_4, CQ_CONTEXT_CQC_BYTE_4_CQN_M,
		       CQ_CONTEXT_CQC_BYTE_4_CQN_S, hr_cq->cqn);
	cq_context->cqc_byte_4 = cpu_to_le32(cq_context->cqc_byte_4);

	cq_context->cq_bt_l = (u32)dma_handle;
	cq_context->cq_bt_l = cpu_to_le32(cq_context->cq_bt_l);

	roce_set_field(cq_context->cqc_byte_12,
		       CQ_CONTEXT_CQC_BYTE_12_CQ_BT_H_M,
		       CQ_CONTEXT_CQC_BYTE_12_CQ_BT_H_S,
		       ((u64)dma_handle >> 32));
	roce_set_field(cq_context->cqc_byte_12,
		       CQ_CONTEXT_CQC_BYTE_12_CQ_CQE_SHIFT_M,
		       CQ_CONTEXT_CQC_BYTE_12_CQ_CQE_SHIFT_S,
		       ilog2((unsigned int)nent));
	roce_set_field(cq_context->cqc_byte_12, CQ_CONTEXT_CQC_BYTE_12_CEQN_M,
		       CQ_CONTEXT_CQC_BYTE_12_CEQN_S, vector);
	cq_context->cqc_byte_12 = cpu_to_le32(cq_context->cqc_byte_12);

	cq_context->cur_cqe_ba0_l = (u32)(mtts[0]);
	cq_context->cur_cqe_ba0_l = cpu_to_le32(cq_context->cur_cqe_ba0_l);

	roce_set_field(cq_context->cqc_byte_20,
		       CQ_CONTEXT_CQC_BYTE_20_CUR_CQE_BA0_H_M,
		       CQ_CONTEXT_CQC_BYTE_20_CUR_CQE_BA0_H_S,
		       cpu_to_le32((mtts[0]) >> 32));
	/* Dedicated hardware, directly set 0 */
	roce_set_field(cq_context->cqc_byte_20,
		       CQ_CONTEXT_CQC_BYTE_20_CQ_CUR_INDEX_M,
		       CQ_CONTEXT_CQC_BYTE_20_CQ_CUR_INDEX_S, 0);
	/**
	 * 44 = 32 + 12, When evaluating addr to hardware, shift 12 because of
	 * using 4K page, and shift more 32 because of
	 * caculating the high 32 bit value evaluated to hardware.
	 */
	roce_set_field(cq_context->cqc_byte_20,
		       CQ_CONTEXT_CQC_BYTE_20_CQE_TPTR_ADDR_H_M,
		       CQ_CONTEXT_CQC_BYTE_20_CQE_TPTR_ADDR_H_S,
		       (u64)(hr_cq->db.dma) >> 44);
	cq_context->cqc_byte_20 = cpu_to_le32(cq_context->cqc_byte_20);

	cq_context->cqe_tptr_addr_l = (u32)((u64)(hr_cq->db.dma) >> 12);

	roce_set_field(cq_context->cqc_byte_32,
		       CQ_CONTEXT_CQC_BYTE_32_CUR_CQE_BA1_H_M,
		       CQ_CONTEXT_CQC_BYTE_32_CUR_CQE_BA1_H_S, 0);
	roce_set_bit(cq_context->cqc_byte_32,
		     CQ_CONTEXT_CQC_BYTE_32_SE_FLAG_S, 0);
	roce_set_bit(cq_context->cqc_byte_32,
		     CQ_CONTEXT_CQC_BYTE_32_CE_FLAG_S, 0);
	roce_set_bit(cq_context->cqc_byte_32,
		     CQ_CONTEXT_CQC_BYTE_32_NOTIFICATION_FLAG_S, 0);
	roce_set_bit(cq_context->cqc_byte_32,
		     CQ_CQNTEXT_CQC_BYTE_32_TYPE_OF_COMPLETION_NOTIFICATION_S,
		     0);
	/*The initial value of cq's ci is 0 */
	roce_set_field(cq_context->cqc_byte_32,
		       CQ_CONTEXT_CQC_BYTE_32_CQ_CONS_IDX_M,
		       CQ_CONTEXT_CQC_BYTE_32_CQ_CONS_IDX_S, 0);
	cq_context->cqc_byte_32 = cpu_to_le32(cq_context->cqc_byte_32);
}

int hns_roce_v1_clear_hem(struct hns_roce_dev *hr_dev,
		struct hns_roce_hem_table *table, int obj)
{
	struct device *dev = &hr_dev->pdev->dev;
	struct hns_roce_v1_priv *priv;
	unsigned long end = 0;
	u32 bt_cmd_h_val = 0;
	u64 bt_ba = 0;

	priv = (struct hns_roce_v1_priv *)hr_dev->hw->priv;

	switch (table->type) {
	case HEM_TYPE_QPC:
		dev_dbg(dev, "UNMAP QPC BT:\n");
		roce_set_field(bt_cmd_h_val, ROCEE_BT_CMD_H_ROCEE_BT_CMD_MDF_M,
			ROCEE_BT_CMD_H_ROCEE_BT_CMD_MDF_S, HEM_TYPE_QPC);
		bt_ba = priv->bt_rsv_buf_dma_qpc >> 12;
		break;
	case HEM_TYPE_MTPT:
		dev_dbg(dev, "UNMAP MTPT BT:\n");
		roce_set_field(bt_cmd_h_val, ROCEE_BT_CMD_H_ROCEE_BT_CMD_MDF_M,
			ROCEE_BT_CMD_H_ROCEE_BT_CMD_MDF_S, HEM_TYPE_MTPT);
		bt_ba = priv->bt_rsv_buf_dma_mtpt >> 12;
		break;
	case HEM_TYPE_CQC:
		dev_dbg(dev, "UNMAP CQC BT:\n");
		roce_set_field(bt_cmd_h_val, ROCEE_BT_CMD_H_ROCEE_BT_CMD_MDF_M,
			ROCEE_BT_CMD_H_ROCEE_BT_CMD_MDF_S, HEM_TYPE_CQC);
		bt_ba = priv->bt_rsv_buf_dma_cqc >> 12;
		break;
	case HEM_TYPE_SRQC:
		dev_dbg(dev, "HEM_TYPE_SRQC not support.\n");
		return -EINVAL;
	default:
		dev_dbg(dev, "HEM table type(%d) unnecessary to unmap!\n",
			table->type);
		return 0;
	}
	roce_set_field(bt_cmd_h_val, ROCEE_BT_CMD_H_ROCEE_BT_CMD_IN_MDF_M,
		ROCEE_BT_CMD_H_ROCEE_BT_CMD_IN_MDF_S, obj);
	roce_set_bit(bt_cmd_h_val, ROCEE_BT_CMD_H_ROCEE_BT_CMD_S, 0);
	roce_set_bit(bt_cmd_h_val, ROCEE_BT_CMD_H_ROCEE_BT_CMD_HW_SYNS_S, 1);

	mutex_lock(&hr_dev->bt_cmd_lock);

	end = msecs_to_jiffies(HW_SYNC_TIMEOUT_MSECS) + jiffies;
	while (1) {
		if (roce_read(hr_dev, ROCEE_BT_CMD_H_REG) >> 31) {
			if (!(time_before(jiffies, end))) {
				dev_err(dev, "Unmap hem Write bt_cmd err. Hw_sync is not zero!\n");
				mutex_unlock(&hr_dev->bt_cmd_lock);
				return -EBUSY;
			}
		} else {
			break;
		}
		msleep(HW_SYNC_SLEEP_TIME_INTERVAL);
	}

	roce_set_field(bt_cmd_h_val, ROCEE_BT_CMD_H_ROCEE_BT_CMD_BA_H_M,
		ROCEE_BT_CMD_H_ROCEE_BT_CMD_BA_H_S, bt_ba >> 32);

	dev_dbg(dev, "(0x%x, 0x%x)\n", (u32)bt_ba, bt_cmd_h_val);

	roce_write(hr_dev, ROCEE_BT_CMD_L_REG, (u32)bt_ba);
	roce_write(hr_dev, ROCEE_BT_CMD_H_REG, bt_cmd_h_val);

	mutex_unlock(&hr_dev->bt_cmd_lock);

	return 0;
}

int hns_roce_v1_req_notify_cq(struct ib_cq *ibcq, enum ib_cq_notify_flags flags)
{
	struct hns_roce_cq *hr_cq = to_hr_cq(ibcq);
	u32 notification_flag;
	u32 doorbell[2];
	int ret = 0;

	notification_flag = (flags & IB_CQ_SOLICITED_MASK) ==
			    IB_CQ_SOLICITED ? CQ_DB_REQ_NOT : CQ_DB_REQ_NOT_SOL;
	/*
	* flags = 0; Notification Flag = 1, next
	* flags = 1; Notification Flag = 0, solocited
	*/
	doorbell[0] = hr_cq->cons_index & ((hr_cq->cq_depth << 1) - 1);
	roce_set_bit(doorbell[1], ROCEE_DB_OTHERS_H_ROCEE_DB_OTH_HW_SYNS_S, 1);
	roce_set_field(doorbell[1], ROCEE_DB_OTHERS_H_ROCEE_DB_OTH_CMD_M,
		       ROCEE_DB_OTHERS_H_ROCEE_DB_OTH_CMD_S, 3);
	roce_set_field(doorbell[1], ROCEE_DB_OTHERS_H_ROCEE_DB_OTH_CMD_MDF_M,
		       ROCEE_DB_OTHERS_H_ROCEE_DB_OTH_CMD_MDF_S, 1);
	roce_set_field(doorbell[1], ROCEE_DB_OTHERS_H_ROCEE_DB_OTH_INP_H_M,
		       ROCEE_DB_OTHERS_H_ROCEE_DB_OTH_INP_H_S,
		       hr_cq->cqn | notification_flag);

	hns_roce_write64_k(doorbell, hr_cq->cq_db_l);

	return ret;
}

static void set_wc_status(struct hns_roce_cqe *cqe, struct ib_wc *wc)
{
	u32 status = roce_get_field(cqe->cqe_byte_4,
				CQE_BYTE_4_STATUS_OF_THE_OPERATION_M,
				CQE_BYTE_4_STATUS_OF_THE_OPERATION_S) &
				HNS_ROCE_CQE_STATUS_MASK;

	switch (status) {
	case HNS_ROCE_CQE_SUCCESS:
		wc->status = IB_WC_SUCCESS;
		break;
	case HNS_ROCE_CQE_SYNDROME_LOCAL_LENGTH_ERR:
		wc->status = IB_WC_LOC_LEN_ERR;
		break;
	case HNS_ROCE_CQE_SYNDROME_LOCAL_QP_OP_ERR:
		wc->status = IB_WC_LOC_QP_OP_ERR;
		break;
	case HNS_ROCE_CQE_SYNDROME_LOCAL_PROT_ERR:
		wc->status = IB_WC_LOC_PROT_ERR;
		break;
	case HNS_ROCE_CQE_SYNDROME_WR_FLUSH_ERR:
		wc->status = IB_WC_WR_FLUSH_ERR;
		break;
	case HNS_ROCE_CQE_SYNDROME_MEM_MANAGE_OPERATE_ERR:
		wc->status = IB_WC_MW_BIND_ERR;
		break;
	case HNS_ROCE_CQE_SYNDROME_BAD_RESP_ERR:
		wc->status = IB_WC_BAD_RESP_ERR;
		break;
	case HNS_ROCE_CQE_SYNDROME_LOCAL_ACCESS_ERR:
		wc->status = IB_WC_LOC_ACCESS_ERR;
		break;
	case HNS_ROCE_CQE_SYNDROME_REMOTE_INVAL_REQ_ERR:
		wc->status = IB_WC_REM_INV_REQ_ERR;
		break;
	case HNS_ROCE_CQE_SYNDROME_REMOTE_ACCESS_ERR:
		wc->status = IB_WC_REM_ACCESS_ERR;
		break;
	case HNS_ROCE_CQE_SYNDROME_REMOTE_OP_ERR:
		wc->status = IB_WC_REM_OP_ERR;
		break;
	case HNS_ROCE_CQE_SYNDROME_TRANSPORT_RETRY_EXC_ERR:
		wc->status = IB_WC_RETRY_EXC_ERR;
		break;
	case HNS_ROCE_CQE_SYNDROME_RNR_RETRY_EXC_ERR:
		wc->status = IB_WC_RNR_RETRY_EXC_ERR;
		break;
	default:
		wc->status = IB_WC_GENERAL_ERR;
		break;
	}
}

static void set_wc_options(struct hns_roce_wqe_ctrl_seg *sq_wqe,
	struct ib_wc *wc, struct hns_roce_cqe *cqe)
{
	switch (sq_wqe->flag & HNS_ROCE_WQE_OPCODE_MASK) {
	case HNS_ROCE_WQE_OPCODE_SEND:
		wc->opcode = IB_WC_SEND;
		break;
	case HNS_ROCE_WQE_OPCODE_RDMA_READ:
		wc->opcode = IB_WC_RDMA_READ;
		wc->byte_len = le32_to_cpu(cqe->byte_cnt);
		break;
	case HNS_ROCE_WQE_OPCODE_RDMA_WRITE:
		wc->opcode = IB_WC_RDMA_WRITE;
		break;
	case HNS_ROCE_WQE_OPCODE_LOCAL_INV:
		wc->opcode = IB_WC_LOCAL_INV;
		break;
	case HNS_ROCE_WQE_OPCODE_UD_SEND:
		wc->opcode = IB_WC_SEND;
		break;
	default:
		wc->status = IB_WC_GENERAL_ERR;
		break;
	}
	wc->wc_flags = (sq_wqe->flag & HNS_ROCE_WQE_IMM ?
		IB_WC_WITH_IMM : 0);
}

static int hns_roce_v1_poll_one(struct hns_roce_cq *hr_cq,
				struct hns_roce_qp **cur_qp, struct ib_wc *wc)
{
	int qpn;
	int is_send;
	u16 wqe_ctr;
	u32 opcode, local_qpn, port;
	struct hns_roce_cqe *cqe;
	struct hns_roce_qp *hr_qp;
	struct hns_roce_wq *wq;
	struct hns_roce_wqe_ctrl_seg *sq_wqe;
	struct hns_roce_dev *hr_dev = to_hr_dev(hr_cq->ib_cq.device);
	struct device *dev = &hr_dev->pdev->dev;

	/* Find cqe according consumer index */
	cqe = next_cqe_sw(hr_cq);
	if (!cqe)
		return -EAGAIN;

	++hr_cq->cons_index;
	/* Memory barrier */
	rmb();
	/* 0->SQ, 1->RQ */
	is_send  = !(roce_get_bit(cqe->cqe_byte_4, CQE_BYTE_4_SQ_RQ_FLAG_S));

	/* Local_qpn in UD cqe is always 1, so it needs to compute new qpn */
	local_qpn = roce_get_field(cqe->cqe_byte_16, CQE_BYTE_16_LOCAL_QPN_M,
					CQE_BYTE_16_LOCAL_QPN_S);
	port = roce_get_field(cqe->cqe_byte_20, CQE_BYTE_20_PORT_NUM_M,
					CQE_BYTE_20_PORT_NUM_S);
	qpn = hns_get_real_qpn(local_qpn, hr_dev->caps.num_ports, port);

	if (!*cur_qp || (qpn & HNS_ROCE_CQE_QPN_MASK) != (*cur_qp)->qpn) {
		hr_qp = __hns_roce_qp_lookup(hr_dev, qpn);
		if (unlikely(!hr_qp)) {
			dev_err(dev, "CQ %06lx with entry for unknown QPN %06x\n",
				hr_cq->cqn, (qpn & HNS_ROCE_CQE_QPN_MASK));
			return -EINVAL;
		}

		*cur_qp = hr_qp;
	}

	wc->qp = &(*cur_qp)->ibqp;
	wc->vendor_err = 0;

	set_wc_status(cqe, wc);

	/* CQE status error, directly return */
	if (wc->status != IB_WC_SUCCESS)
		return 0;

	if (is_send) {
		/* SQ conrespond to CQE */
		sq_wqe = get_send_wqe(*cur_qp, roce_get_field(cqe->cqe_byte_4,
						CQE_BYTE_4_WQE_INDEX_M,
						CQE_BYTE_4_WQE_INDEX_S));
		if (!sq_wqe) {
			dev_err(dev, "Get send wqe failed!\n");
			return -EFAULT;
		}
		set_wc_options(sq_wqe, wc, cqe);

		wq = &(*cur_qp)->sq;
		if ((*cur_qp)->sq_signal_bits) {
			/*
			* If sg_signal_bit is 1,
			* firstly tail pointer updated to wqe
			* which current cqe correspond to
			*/
			wqe_ctr = (u16)roce_get_field(cqe->cqe_byte_4,
						      CQE_BYTE_4_WQE_INDEX_M,
						      CQE_BYTE_4_WQE_INDEX_S);
			wq->tail += (wqe_ctr - (u16)wq->tail) &
				    (wq->wqe_cnt - 1);
		}
		wc->wr_id = wq->wrid[wq->tail & (wq->wqe_cnt - 1)];
		++wq->tail;
	} else {
		/* RQ conrespond to CQE */
		wc->byte_len = le32_to_cpu(cqe->byte_cnt);
		opcode = roce_get_field(cqe->cqe_byte_4,
					CQE_BYTE_4_OPERATION_TYPE_M,
					CQE_BYTE_4_OPERATION_TYPE_S) &
					HNS_ROCE_CQE_OPCODE_MASK;
		switch (opcode) {
		case HNS_ROCE_OPCODE_RDMA_WITH_IMM_RECEIVE:
			wc->opcode = IB_WC_RECV_RDMA_WITH_IMM;
			wc->wc_flags = IB_WC_WITH_IMM;
			wc->ex.imm_data = le32_to_cpu(cqe->immediate_data);
			break;
		case HNS_ROCE_OPCODE_SEND_DATA_RECEIVE:
			if (roce_get_bit(cqe->cqe_byte_4,
					 CQE_BYTE_4_IMM_INDICATOR_S)) {
				wc->opcode = IB_WC_RECV;
				wc->wc_flags = IB_WC_WITH_IMM;
				wc->ex.imm_data = le32_to_cpu(
						  cqe->immediate_data);
			} else {
				wc->opcode = IB_WC_RECV;
				wc->wc_flags = 0;
			}
			break;
		default:
			wc->status = IB_WC_GENERAL_ERR;
			break;
		}

		/* Update tail pointer, record wr_id */
		wq = &(*cur_qp)->rq;
		wc->wr_id = wq->wrid[wq->tail & (wq->wqe_cnt - 1)];
		++wq->tail;
		wc->sl = (u8)roce_get_field(cqe->cqe_byte_20, CQE_BYTE_20_SL_M,
					    CQE_BYTE_20_SL_S);
		wc->src_qp = (u8)roce_get_field(cqe->cqe_byte_20,
						CQE_BYTE_20_REMOTE_QPN_M,
						CQE_BYTE_20_REMOTE_QPN_S);
		wc->wc_flags |= (roce_get_bit(cqe->cqe_byte_20,
					      CQE_BYTE_20_GRH_PRESENT_S) ?
					      IB_WC_GRH : 0);
		wc->pkey_index = (u16)roce_get_field(cqe->cqe_byte_28,
						     CQE_BYTE_28_P_KEY_IDX_M,
						     CQE_BYTE_28_P_KEY_IDX_S);
	}

	return 0;
}

int hns_roce_v1_poll_cq(struct ib_cq *ibcq, int num_entries, struct ib_wc *wc)
{
	struct hns_roce_cq *hr_cq = to_hr_cq(ibcq);
	struct hns_roce_qp *cur_qp = NULL;
	unsigned long flags;
	int npolled;
	int ret = 0;

	spin_lock_irqsave(&hr_cq->lock, flags);

	for (npolled = 0; npolled < num_entries; ++npolled) {
		ret = hns_roce_v1_poll_one(hr_cq, &cur_qp, wc + npolled);
		if (ret)
			break;
	}

	if (npolled) {
		*hr_cq->db.db = hr_cq->cons_index &
			((hr_cq->cq_depth << 1) - 1);

		/* The value must be written in memory after poll cq */
		wmb();
		hns_roce_v1_cq_set_ci(hr_cq, hr_cq->cons_index);
	}

	spin_unlock_irqrestore(&hr_cq->lock, flags);

	if (ret == 0 || ret == -EAGAIN)
		return npolled;
	else
		return ret;
}

static int hns_roce_v1_qp_modify(struct hns_roce_dev *hr_dev,
				 struct hns_roce_mtt *mtt,
				 enum hns_roce_qp_state cur_state,
				 enum hns_roce_qp_state new_state,
				 struct hns_roce_qp_context *context,
				 struct hns_roce_qp *hr_qp)
{
	static const u16
	op[HNS_ROCE_QP_NUM_STATE][HNS_ROCE_QP_NUM_STATE] = {
		[HNS_ROCE_QP_STATE_RST] = {
		[HNS_ROCE_QP_STATE_RST] = HNS_ROCE_CMD_2RST_QP,
		[HNS_ROCE_QP_STATE_ERR] = HNS_ROCE_CMD_2ERR_QP,
		[HNS_ROCE_QP_STATE_INIT] = HNS_ROCE_CMD_RST2INIT_QP,
		},
		[HNS_ROCE_QP_STATE_INIT] = {
		[HNS_ROCE_QP_STATE_RST] = HNS_ROCE_CMD_2RST_QP,
		[HNS_ROCE_QP_STATE_ERR] = HNS_ROCE_CMD_2ERR_QP,
		/* Note: In v1 engine, HW doesn't support RST2INIT.
		 * We use RST2INIT cmd instead of INIT2INIT.
		 */
		[HNS_ROCE_QP_STATE_INIT] = HNS_ROCE_CMD_RST2INIT_QP,
		[HNS_ROCE_QP_STATE_RTR] = HNS_ROCE_CMD_INIT2RTR_QP,
		},
		[HNS_ROCE_QP_STATE_RTR] = {
		[HNS_ROCE_QP_STATE_RST] = HNS_ROCE_CMD_2RST_QP,
		[HNS_ROCE_QP_STATE_ERR] = HNS_ROCE_CMD_2ERR_QP,
		[HNS_ROCE_QP_STATE_RTS] = HNS_ROCE_CMD_RTR2RTS_QP,
		},
		[HNS_ROCE_QP_STATE_RTS] = {
		[HNS_ROCE_QP_STATE_RST] = HNS_ROCE_CMD_2RST_QP,
		[HNS_ROCE_QP_STATE_ERR] = HNS_ROCE_CMD_2ERR_QP,
		[HNS_ROCE_QP_STATE_RTS] = HNS_ROCE_CMD_RTS2RTS_QP,
		[HNS_ROCE_QP_STATE_SQD] = HNS_ROCE_CMD_RTS2SQD_QP,
		},
		[HNS_ROCE_QP_STATE_SQD] = {
		[HNS_ROCE_QP_STATE_RST] = HNS_ROCE_CMD_2RST_QP,
		[HNS_ROCE_QP_STATE_ERR] = HNS_ROCE_CMD_2ERR_QP,
		[HNS_ROCE_QP_STATE_RTS] = HNS_ROCE_CMD_SQD2RTS_QP,
		[HNS_ROCE_QP_STATE_SQD] = HNS_ROCE_CMD_SQD2SQD_QP,
		},
		[HNS_ROCE_QP_STATE_ERR] = {
		[HNS_ROCE_QP_STATE_RST] = HNS_ROCE_CMD_2RST_QP,
		[HNS_ROCE_QP_STATE_ERR] = HNS_ROCE_CMD_2ERR_QP,
		}
	};

	struct hns_roce_cmd_mailbox *mailbox;
	struct device *dev = &hr_dev->pdev->dev;
	int ret = 0;

	if (cur_state >= HNS_ROCE_QP_NUM_STATE ||
	    new_state >= HNS_ROCE_QP_NUM_STATE ||
	    !op[cur_state][new_state]) {
		dev_err(dev, "[modify_qp]Don't support state %d to %d\n",
			cur_state, new_state);
		return -EINVAL;
	}

	if (op[cur_state][new_state] == HNS_ROCE_CMD_2RST_QP)
		return hns_roce_cmd_mbox(hr_dev, 0, 0, hr_qp->qpn, 2,
					 HNS_ROCE_CMD_2RST_QP,
					 HNS_ROCE_CMD_TIME_CLASS_A);

	if (op[cur_state][new_state] == HNS_ROCE_CMD_2ERR_QP)
		return hns_roce_cmd_mbox(hr_dev, 0, 0, hr_qp->qpn, 2,
					 HNS_ROCE_CMD_2ERR_QP,
					 HNS_ROCE_CMD_TIME_CLASS_A);

	mailbox = hns_roce_alloc_cmd_mailbox(hr_dev);
	if (IS_ERR(mailbox)) {
		dev_err(dev, "[modify_qp]Mailboc alloc failed!\n");
		return PTR_ERR(mailbox);
	}

	memcpy(mailbox->buf, context, sizeof(*context));

	ret = hns_roce_cmd_mbox(hr_dev, mailbox->dma, 0, hr_qp->qpn, 0,
				op[cur_state][new_state],
				HNS_ROCE_CMD_TIME_CLASS_A);

	hns_roce_free_cmd_mailbox(hr_dev, mailbox);
	return ret;
}

static int hns_roce_v1_m_sqp(struct ib_qp *ibqp, const struct ib_qp_attr *attr,
			     int attr_mask, enum ib_qp_state cur_state,
			     enum ib_qp_state new_state)
{
	struct hns_roce_dev *hr_dev = to_hr_dev(ibqp->device);
	struct hns_roce_qp *hr_qp = to_hr_qp(ibqp);
	struct hns_roce_sqp_context *context;
	struct device *dev = &hr_dev->pdev->dev;
	dma_addr_t dma_handle = 0;
	int rq_pa_start;
	u32 reg_val;
	u64 *mtts;
	u32 *addr;

	context = kzalloc(sizeof(*context), GFP_KERNEL);
	if (!context)
		return -ENOMEM;

	/* Search QP buf's MTTs */
	mtts = hns_roce_table_find(&hr_dev->mr_table.mtt_table,
				   hr_qp->mtt.first_seg, &dma_handle);
	if (!mtts) {
		dev_err(dev, "Qp buf pa find failed\n");
		goto out;
	}

	if (cur_state == IB_QPS_RESET && new_state == IB_QPS_INIT) {
		roce_set_field(context->qp1c_bytes_4,
			       QP1C_BYTES_4_SQ_WQE_SHIFT_M,
			       QP1C_BYTES_4_SQ_WQE_SHIFT_S,
			       ilog2((unsigned int)hr_qp->sq.wqe_cnt));
		roce_set_field(context->qp1c_bytes_4,
			       QP1C_BYTES_4_RQ_WQE_SHIFT_M,
			       QP1C_BYTES_4_RQ_WQE_SHIFT_S,
			       ilog2((unsigned int)hr_qp->rq.wqe_cnt));
		roce_set_field(context->qp1c_bytes_4, QP1C_BYTES_4_PD_M,
			       QP1C_BYTES_4_PD_S, to_hr_pd(ibqp->pd)->pdn);

		context->sq_rq_bt_l = (u32)(dma_handle);
		roce_set_field(context->qp1c_bytes_12,
			       QP1C_BYTES_12_SQ_RQ_BT_H_M,
			       QP1C_BYTES_12_SQ_RQ_BT_H_S,
			       ((u32)(dma_handle >> 32)));

		roce_set_field(context->qp1c_bytes_16, QP1C_BYTES_16_RQ_HEAD_M,
			       QP1C_BYTES_16_RQ_HEAD_S, hr_qp->rq.head);
		roce_set_field(context->qp1c_bytes_16, QP1C_BYTES_16_PORT_NUM_M,
			       QP1C_BYTES_16_PORT_NUM_S, hr_qp->port);
		roce_set_bit(context->qp1c_bytes_16,
			     QP1C_BYTES_16_SIGNALING_TYPE_S,
			     hr_qp->sq_signal_bits);
		roce_set_bit(context->qp1c_bytes_16, QP1C_BYTES_16_RQ_BA_FLG_S,
			     1);
		roce_set_bit(context->qp1c_bytes_16, QP1C_BYTES_16_SQ_BA_FLG_S,
			     1);
		roce_set_bit(context->qp1c_bytes_16, QP1C_BYTES_16_QP1_ERR_S,
			     0);

		roce_set_field(context->qp1c_bytes_20, QP1C_BYTES_20_SQ_HEAD_M,
			       QP1C_BYTES_20_SQ_HEAD_S, hr_qp->sq.head);
		roce_set_field(context->qp1c_bytes_20, QP1C_BYTES_20_PKEY_IDX_M,
			       QP1C_BYTES_20_PKEY_IDX_S, attr->pkey_index);

		rq_pa_start = (u32)hr_qp->rq.offset / PAGE_SIZE;
		context->cur_rq_wqe_ba_l = (u32)(mtts[rq_pa_start]);

		roce_set_field(context->qp1c_bytes_28,
			       QP1C_BYTES_28_CUR_RQ_WQE_BA_H_M,
			       QP1C_BYTES_28_CUR_RQ_WQE_BA_H_S,
			       (mtts[rq_pa_start]) >> 32);
		roce_set_field(context->qp1c_bytes_28,
			       QP1C_BYTES_28_RQ_CUR_IDX_M,
			       QP1C_BYTES_28_RQ_CUR_IDX_S, 0);

		roce_set_field(context->qp1c_bytes_32,
			       QP1C_BYTES_32_RX_CQ_NUM_M,
			       QP1C_BYTES_32_RX_CQ_NUM_S,
			       to_hr_cq(ibqp->recv_cq)->cqn);
		roce_set_field(context->qp1c_bytes_32,
			       QP1C_BYTES_32_TX_CQ_NUM_M,
			       QP1C_BYTES_32_TX_CQ_NUM_S,
			       to_hr_cq(ibqp->send_cq)->cqn);

		context->cur_sq_wqe_ba_l  = (u32)mtts[0];

		roce_set_field(context->qp1c_bytes_40,
			       QP1C_BYTES_40_CUR_SQ_WQE_BA_H_M,
			       QP1C_BYTES_40_CUR_SQ_WQE_BA_H_S,
			       (mtts[0]) >> 32);
		roce_set_field(context->qp1c_bytes_40,
			       QP1C_BYTES_40_SQ_CUR_IDX_M,
			       QP1C_BYTES_40_SQ_CUR_IDX_S, 0);

		/* Copy context to QP1C register */
		addr = (u32 *)(hr_dev->reg_base + ROCEE_QP1C_CFG0_0_REG +
			hr_qp->port * sizeof(*context));

		writel(context->qp1c_bytes_4, addr);
		writel(context->sq_rq_bt_l, addr + 1);
		writel(context->qp1c_bytes_12, addr + 2);
		writel(context->qp1c_bytes_16, addr + 3);
		writel(context->qp1c_bytes_20, addr + 4);
		writel(context->cur_rq_wqe_ba_l, addr + 5);
		writel(context->qp1c_bytes_28, addr + 6);
		writel(context->qp1c_bytes_32, addr + 7);
		writel(context->cur_sq_wqe_ba_l, addr + 8);
	}

	/* Modify QP1C status */
	reg_val = roce_read(hr_dev, ROCEE_QP1C_CFG0_0_REG +
			    hr_qp->port * sizeof(*context));
	roce_set_field(reg_val, ROCEE_QP1C_CFG0_0_ROCEE_QP1C_QP_ST_M,
		       ROCEE_QP1C_CFG0_0_ROCEE_QP1C_QP_ST_S, new_state);
	roce_write(hr_dev, ROCEE_QP1C_CFG0_0_REG +
		    hr_qp->port * sizeof(*context), reg_val);

	hr_qp->state = new_state;
	if (new_state == IB_QPS_RESET) {
		hns_roce_v1_cq_clean(to_hr_cq(ibqp->recv_cq), hr_qp->qpn,
				     ibqp->srq ? to_hr_srq(ibqp->srq) : NULL);
		if (ibqp->send_cq != ibqp->recv_cq)
			hns_roce_v1_cq_clean(to_hr_cq(ibqp->send_cq),
					     hr_qp->qpn, NULL);

		hr_qp->rq.head = 0;
		hr_qp->rq.tail = 0;
		hr_qp->sq.head = 0;
		hr_qp->sq.tail = 0;
		hr_qp->sq_next_wqe = 0;
	}

	kfree(context);
	return 0;

out:
	kfree(context);
	return -EINVAL;
}

static void modify_qp_rst_to_init(struct ib_qp *ibqp,
		const struct ib_qp_attr *attr,
		struct hns_roce_qp_context *context)
{
	struct hns_roce_qp *hr_qp = to_hr_qp(ibqp);

	roce_set_field(context->qpc_bytes_4,
		QP_CONTEXT_QPC_BYTES_4_TRANSPORT_SERVICE_TYPE_M,
		QP_CONTEXT_QPC_BYTES_4_TRANSPORT_SERVICE_TYPE_S,
		to_hr_qp_type(hr_qp->ibqp.qp_type));

	roce_set_bit(context->qpc_bytes_4,
		QP_CONTEXT_QPC_BYTE_4_ENABLE_FPMR_S, 0);
	roce_set_bit(context->qpc_bytes_4,
		QP_CONTEXT_QPC_BYTE_4_RDMA_READ_ENABLE_S,
		!!(attr->qp_access_flags & IB_ACCESS_REMOTE_READ));
	roce_set_bit(context->qpc_bytes_4,
		QP_CONTEXT_QPC_BYTE_4_RDMA_WRITE_ENABLE_S,
		!!(attr->qp_access_flags & IB_ACCESS_REMOTE_WRITE));
	roce_set_bit(context->qpc_bytes_4,
		QP_CONTEXT_QPC_BYTE_4_ATOMIC_OPERATION_ENABLE_S,
		!!(attr->qp_access_flags & IB_ACCESS_REMOTE_ATOMIC));
	roce_set_bit(context->qpc_bytes_4,
		QP_CONTEXT_QPC_BYTE_4_RDMAR_USE_S, 1);
	roce_set_field(context->qpc_bytes_4,
		QP_CONTEXT_QPC_BYTES_4_SQ_WQE_SHIFT_M,
		QP_CONTEXT_QPC_BYTES_4_SQ_WQE_SHIFT_S,
			       ilog2((unsigned int)hr_qp->sq.wqe_cnt));
	roce_set_field(context->qpc_bytes_4,
		QP_CONTEXT_QPC_BYTES_4_RQ_WQE_SHIFT_M,
		QP_CONTEXT_QPC_BYTES_4_RQ_WQE_SHIFT_S,
			       ilog2((unsigned int)hr_qp->rq.wqe_cnt));
	roce_set_field(context->qpc_bytes_4,
		QP_CONTEXT_QPC_BYTES_4_PD_M,
		QP_CONTEXT_QPC_BYTES_4_PD_S,
		to_hr_pd(ibqp->pd)->pdn);
	hr_qp->access_flags = attr->qp_access_flags;
	roce_set_field(context->qpc_bytes_8,
		QP_CONTEXT_QPC_BYTES_8_TX_COMPLETION_M,
		QP_CONTEXT_QPC_BYTES_8_TX_COMPLETION_S,
		to_hr_cq(ibqp->send_cq)->cqn);
	roce_set_field(context->qpc_bytes_8,
		QP_CONTEXT_QPC_BYTES_8_RX_COMPLETION_M,
		QP_CONTEXT_QPC_BYTES_8_RX_COMPLETION_S,
		to_hr_cq(ibqp->recv_cq)->cqn);

	if (ibqp->srq)
		roce_set_field(context->qpc_bytes_12,
			QP_CONTEXT_QPC_BYTES_12_SRQ_NUMBER_M,
			QP_CONTEXT_QPC_BYTES_12_SRQ_NUMBER_S,
			to_hr_srq(ibqp->srq)->srqn);

	roce_set_field(context->qpc_bytes_12,
		QP_CONTEXT_QPC_BYTES_12_P_KEY_INDEX_M,
		QP_CONTEXT_QPC_BYTES_12_P_KEY_INDEX_S,
		attr->pkey_index);
	hr_qp->pkey_index = attr->pkey_index;
	roce_set_field(context->qpc_bytes_16,
		QP_CONTEXT_QPC_BYTES_16_QP_NUM_M,
		QP_CONTEXT_QPC_BYTES_16_QP_NUM_S, hr_qp->qpn);

}

static void modify_qp_init_to_init(struct ib_qp *ibqp,
		const struct ib_qp_attr *attr,
		int attr_mask,
		struct hns_roce_qp_context *context)
{
	struct hns_roce_qp *hr_qp = to_hr_qp(ibqp);

	roce_set_field(context->qpc_bytes_4,
		QP_CONTEXT_QPC_BYTES_4_TRANSPORT_SERVICE_TYPE_M,
		QP_CONTEXT_QPC_BYTES_4_TRANSPORT_SERVICE_TYPE_S,
		to_hr_qp_type(hr_qp->ibqp.qp_type));
	roce_set_bit(context->qpc_bytes_4,
		QP_CONTEXT_QPC_BYTE_4_ENABLE_FPMR_S, 0);
	if (attr_mask & IB_QP_ACCESS_FLAGS) {
		roce_set_bit(context->qpc_bytes_4,
			QP_CONTEXT_QPC_BYTE_4_RDMA_READ_ENABLE_S,
			!!(attr->qp_access_flags &
			IB_ACCESS_REMOTE_READ));
		roce_set_bit(context->qpc_bytes_4,
			QP_CONTEXT_QPC_BYTE_4_RDMA_WRITE_ENABLE_S,
			!!(attr->qp_access_flags &
			IB_ACCESS_REMOTE_WRITE));
	} else {
		roce_set_bit(context->qpc_bytes_4,
			QP_CONTEXT_QPC_BYTE_4_RDMA_READ_ENABLE_S,
			!!(hr_qp->access_flags &
			IB_ACCESS_REMOTE_READ));
		roce_set_bit(context->qpc_bytes_4,
			QP_CONTEXT_QPC_BYTE_4_RDMA_WRITE_ENABLE_S,
			!!(hr_qp->access_flags &
			IB_ACCESS_REMOTE_WRITE));
	}

	roce_set_bit(context->qpc_bytes_4,
		QP_CONTEXT_QPC_BYTE_4_RDMAR_USE_S, 1);
	roce_set_field(context->qpc_bytes_4,
		QP_CONTEXT_QPC_BYTES_4_SQ_WQE_SHIFT_M,
		QP_CONTEXT_QPC_BYTES_4_SQ_WQE_SHIFT_S,
			       ilog2((unsigned int)hr_qp->sq.wqe_cnt));
	roce_set_field(context->qpc_bytes_4,
		QP_CONTEXT_QPC_BYTES_4_RQ_WQE_SHIFT_M,
		QP_CONTEXT_QPC_BYTES_4_RQ_WQE_SHIFT_S,
		ilog2((unsigned int)hr_qp->rq.wqe_cnt));
	roce_set_field(context->qpc_bytes_4,
		QP_CONTEXT_QPC_BYTES_4_PD_M,
		QP_CONTEXT_QPC_BYTES_4_PD_S,
		to_hr_pd(ibqp->pd)->pdn);

	roce_set_field(context->qpc_bytes_8,
		QP_CONTEXT_QPC_BYTES_8_TX_COMPLETION_M,
		QP_CONTEXT_QPC_BYTES_8_TX_COMPLETION_S,
		to_hr_cq(ibqp->send_cq)->cqn);
	roce_set_field(context->qpc_bytes_8,
		QP_CONTEXT_QPC_BYTES_8_RX_COMPLETION_M,
		QP_CONTEXT_QPC_BYTES_8_RX_COMPLETION_S,
		to_hr_cq(ibqp->recv_cq)->cqn);

	if (ibqp->srq)
		roce_set_field(context->qpc_bytes_12,
			QP_CONTEXT_QPC_BYTES_12_SRQ_NUMBER_M,
			QP_CONTEXT_QPC_BYTES_12_SRQ_NUMBER_S,
			to_hr_srq(ibqp->srq)->srqn);
	if (attr_mask & IB_QP_PKEY_INDEX)
		roce_set_field(context->qpc_bytes_12,
			QP_CONTEXT_QPC_BYTES_12_P_KEY_INDEX_M,
			QP_CONTEXT_QPC_BYTES_12_P_KEY_INDEX_S,
			attr->pkey_index);
	else
		roce_set_field(context->qpc_bytes_12,
			QP_CONTEXT_QPC_BYTES_12_P_KEY_INDEX_M,
			QP_CONTEXT_QPC_BYTES_12_P_KEY_INDEX_S,
			hr_qp->pkey_index);

	roce_set_field(context->qpc_bytes_16,
			QP_CONTEXT_QPC_BYTES_16_QP_NUM_M,
			QP_CONTEXT_QPC_BYTES_16_QP_NUM_S, hr_qp->qpn);
}

int modify_qp_init_to_rtr(struct ib_qp *ibqp, const struct ib_qp_attr *attr,
		int attr_mask, struct hns_roce_qp_context *context)
{
	struct hns_roce_dev *hr_dev = to_hr_dev(ibqp->device);
	struct hns_roce_qp *hr_qp = to_hr_qp(ibqp);
	struct device *dev = &hr_dev->pdev->dev;
	u64 *mtts = NULL;
	u64 *mtts_2 = NULL;
	dma_addr_t dma_handle = 0;
	dma_addr_t dma_handle_2 = 0;
	int port;
	u8 *dmac;
	u8 *smac;
	int rq_pa_start = 0;

	/* search qp buf's mtts */
	mtts = hns_roce_table_find(&hr_dev->mr_table.mtt_table,
				hr_qp->mtt.first_seg, &dma_handle);
	if (mtts == NULL) {
		dev_err(dev, "Qp buf pa find failed!\n");
		return -EINVAL;
	}

	/* search IRRL's mtts */
	mtts_2 = hns_roce_table_find(&hr_dev->qp_table.irrl_table,
				hr_qp->qpn, &dma_handle_2);
	if (mtts_2 == NULL) {
		dev_err(dev, "Qp irrl_table find failed!\n");
		return -EINVAL;
	}

	if (INIT_2_RTR_ATTR_MASK_VALID(attr_mask)) {
		dev_err(dev, "INIT2RTR attr_mask(0x%x) error!\n", attr_mask);
		return -EINVAL;
	}

	dmac = (u8 *)attr->ah_attr.dmac;

	context->sq_rq_bt_l = (u32)(dma_handle);

	roce_set_field(context->qpc_bytes_24,
		       QP_CONTEXT_QPC_BYTES_24_SQ_RQ_BT_H_M,
		       QP_CONTEXT_QPC_BYTES_24_SQ_RQ_BT_H_S,
			   ((u32)(dma_handle >> 32)));
	roce_set_bit(context->qpc_bytes_24,
		     QP_CONTEXT_QPC_BYTE_24_REMOTE_ENABLE_E2E_CREDITS_S,
		     1);
	roce_set_field(context->qpc_bytes_24,
		       QP_CONTEXT_QPC_BYTES_24_MINIMUM_RNR_NAK_TIMER_M,
		       QP_CONTEXT_QPC_BYTES_24_MINIMUM_RNR_NAK_TIMER_S,
		       attr->min_rnr_timer);
	context->irrl_ba_l = (u32)(dma_handle_2);
	roce_set_field(context->qpc_bytes_32,
		       QP_CONTEXT_QPC_BYTES_32_IRRL_BA_H_M,
		       QP_CONTEXT_QPC_BYTES_32_IRRL_BA_H_S,
		       ((u32)(dma_handle_2 >> 32)) &
			QP_CONTEXT_QPC_BYTES_32_IRRL_BA_H_M);
	roce_set_field(context->qpc_bytes_32,
		       QP_CONTEXT_QPC_BYTES_32_MIG_STATE_M,
		       QP_CONTEXT_QPC_BYTES_32_MIG_STATE_S, 0);
	roce_set_bit(context->qpc_bytes_32,
		     QP_CONTEXT_QPC_BYTE_32_LOCAL_ENABLE_E2E_CREDITS_S,
		     1);
	roce_set_bit(context->qpc_bytes_32,
		     QP_CONTEXT_QPC_BYTE_32_SIGNALING_TYPE_S,
		     hr_qp->sq_signal_bits);

	port = (attr_mask & IB_QP_PORT) ? (attr->port_num - 1) :
		hr_qp->port;
	smac = (u8 *)hr_dev->dev_addr[port];
	if ((dmac[0] == smac[0]) && (dmac[1] == smac[1]) &&
	    (dmac[2] == smac[2]) && (dmac[3] == smac[3]) &&
	    (dmac[4] == smac[4]) && (dmac[5] == smac[5]))
		roce_set_bit(context->qpc_bytes_32,
		    QP_CONTEXT_QPC_BYTE_32_LOOPBACK_INDICATOR_S,
		    1);

	if (hr_dev->loop_idc == 0x1)
		roce_set_bit(context->qpc_bytes_32,
			QP_CONTEXT_QPC_BYTE_32_LOOPBACK_INDICATOR_S, 1);
	roce_set_bit(context->qpc_bytes_32,
		     QP_CONTEXT_QPC_BYTE_32_GLOBAL_HEADER_S,
		     attr->ah_attr.ah_flags);
	roce_set_field(context->qpc_bytes_32,
		       QP_CONTEXT_QPC_BYTES_32_RESPONDER_RESOURCES_M,
		       QP_CONTEXT_QPC_BYTES_32_RESPONDER_RESOURCES_S,
			       ilog2((unsigned int)attr->max_dest_rd_atomic));

	roce_set_field(context->qpc_bytes_36,
		       QP_CONTEXT_QPC_BYTES_36_DEST_QP_M,
		       QP_CONTEXT_QPC_BYTES_36_DEST_QP_S,
		       attr->dest_qp_num);

	/* Configure GID index */
	roce_set_field(context->qpc_bytes_36,
		       QP_CONTEXT_QPC_BYTES_36_SGID_INDEX_M,
		       QP_CONTEXT_QPC_BYTES_36_SGID_INDEX_S,
		       hns_get_gid_index(hr_dev,
					 attr->ah_attr.port_num - 1,
					 attr->ah_attr.grh.sgid_index));

	memcpy(&(context->dmac_l), dmac, 4);

	roce_set_field(context->qpc_bytes_44,
		       QP_CONTEXT_QPC_BYTES_44_DMAC_H_M,
		       QP_CONTEXT_QPC_BYTES_44_DMAC_H_S,
		       *((u16 *)(&dmac[4])));
	roce_set_field(context->qpc_bytes_44,
		       QP_CONTEXT_QPC_BYTES_44_MAXIMUM_STATIC_RATE_M,
		       QP_CONTEXT_QPC_BYTES_44_MAXIMUM_STATIC_RATE_S,
		       attr->ah_attr.static_rate);
	roce_set_field(context->qpc_bytes_44,
		       QP_CONTEXT_QPC_BYTES_44_HOPLMT_M,
		       QP_CONTEXT_QPC_BYTES_44_HOPLMT_S,
		       attr->ah_attr.grh.hop_limit);

	roce_set_field(context->qpc_bytes_48,
		       QP_CONTEXT_QPC_BYTES_48_FLOWLABEL_M,
		       QP_CONTEXT_QPC_BYTES_48_FLOWLABEL_S,
		       attr->ah_attr.grh.flow_label);
	roce_set_field(context->qpc_bytes_48,
		       QP_CONTEXT_QPC_BYTES_48_TCLASS_M,
		       QP_CONTEXT_QPC_BYTES_48_TCLASS_S,
		       attr->ah_attr.grh.traffic_class);
	roce_set_field(context->qpc_bytes_48,
		       QP_CONTEXT_QPC_BYTES_48_MTU_M,
		       QP_CONTEXT_QPC_BYTES_48_MTU_S, attr->path_mtu);
	hr_qp->mtu = ib_mtu_enum_to_int(attr->path_mtu);

	memcpy(context->dgid, attr->ah_attr.grh.dgid.raw,
	       sizeof(attr->ah_attr.grh.dgid.raw));

	roce_set_field(context->qpc_bytes_68,
		       QP_CONTEXT_QPC_BYTES_68_RQ_HEAD_M,
		       QP_CONTEXT_QPC_BYTES_68_RQ_HEAD_S,
			   hr_qp->rq.head);
	roce_set_field(context->qpc_bytes_68,
		       QP_CONTEXT_QPC_BYTES_68_RQ_CUR_INDEX_M,
		       QP_CONTEXT_QPC_BYTES_68_RQ_CUR_INDEX_S, 0);

	rq_pa_start = (u32)hr_qp->rq.offset / PAGE_SIZE;
	context->cur_rq_wqe_ba_l = (u32)(mtts[rq_pa_start]);

	roce_set_field(context->qpc_bytes_76,
		QP_CONTEXT_QPC_BYTES_76_CUR_RQ_WQE_BA_H_M,
		QP_CONTEXT_QPC_BYTES_76_CUR_RQ_WQE_BA_H_S,
		mtts[rq_pa_start] >> 32);
	roce_set_field(context->qpc_bytes_76,
		       QP_CONTEXT_QPC_BYTES_76_RX_REQ_MSN_M,
		       QP_CONTEXT_QPC_BYTES_76_RX_REQ_MSN_S, 0);

	context->rx_rnr_time = 0;

	roce_set_field(context->qpc_bytes_84,
		       QP_CONTEXT_QPC_BYTES_84_LAST_ACK_PSN_M,
		       QP_CONTEXT_QPC_BYTES_84_LAST_ACK_PSN_S,
		       attr->rq_psn - 1);
	roce_set_field(context->qpc_bytes_84,
		       QP_CONTEXT_QPC_BYTES_84_TRRL_HEAD_M,
		       QP_CONTEXT_QPC_BYTES_84_TRRL_HEAD_S, 0);

	roce_set_field(context->qpc_bytes_88,
		       QP_CONTEXT_QPC_BYTES_88_RX_REQ_EPSN_M,
		       QP_CONTEXT_QPC_BYTES_88_RX_REQ_EPSN_S,
		       attr->rq_psn);
	roce_set_bit(context->qpc_bytes_88,
		     QP_CONTEXT_QPC_BYTES_88_RX_REQ_PSN_ERR_FLAG_S, 0);
	roce_set_bit(context->qpc_bytes_88,
		     QP_CONTEXT_QPC_BYTES_88_RX_LAST_OPCODE_FLG_S, 0);
	roce_set_field(context->qpc_bytes_88,
		QP_CONTEXT_QPC_BYTES_88_RQ_REQ_LAST_OPERATION_TYPE_M,
		QP_CONTEXT_QPC_BYTES_88_RQ_REQ_LAST_OPERATION_TYPE_S,
		0);
	roce_set_field(context->qpc_bytes_88,
		       QP_CONTEXT_QPC_BYTES_88_RQ_REQ_RDMA_WR_FLAG_M,
		       QP_CONTEXT_QPC_BYTES_88_RQ_REQ_RDMA_WR_FLAG_S,
		       0);

	context->dma_length = 0;
	context->r_key = 0;
	context->va_l = 0;
	context->va_h = 0;

	roce_set_field(context->qpc_bytes_108,
		       QP_CONTEXT_QPC_BYTES_108_TRRL_SDB_PSN_M,
		       QP_CONTEXT_QPC_BYTES_108_TRRL_SDB_PSN_S, 0);
	roce_set_bit(context->qpc_bytes_108,
		     QP_CONTEXT_QPC_BYTES_108_TRRL_SDB_PSN_FLG_S, 0);
	roce_set_bit(context->qpc_bytes_108,
		     QP_CONTEXT_QPC_BYTES_108_TRRL_TDB_PSN_FLG_S, 0);

	roce_set_field(context->qpc_bytes_112,
		       QP_CONTEXT_QPC_BYTES_112_TRRL_TDB_PSN_M,
		       QP_CONTEXT_QPC_BYTES_112_TRRL_TDB_PSN_S, 0);
	roce_set_field(context->qpc_bytes_112,
		       QP_CONTEXT_QPC_BYTES_112_TRRL_TAIL_M,
		       QP_CONTEXT_QPC_BYTES_112_TRRL_TAIL_S, 0);

	/* For chip resp ack */
	roce_set_field(context->qpc_bytes_156,
		       QP_CONTEXT_QPC_BYTES_156_PORT_NUM_M,
		       QP_CONTEXT_QPC_BYTES_156_PORT_NUM_S,
		       hr_qp->port);
	roce_set_field(context->qpc_bytes_156,
		       QP_CONTEXT_QPC_BYTES_156_SL_M,
		       QP_CONTEXT_QPC_BYTES_156_SL_S, attr->ah_attr.sl);
	hr_qp->sl = attr->ah_attr.sl;
	return 0;
}
static int modify_qp_rtr_to_rts(struct ib_qp *ibqp,
			const struct ib_qp_attr *attr, int attr_mask,
			struct hns_roce_qp_context *context)
{
	struct hns_roce_dev *hr_dev = to_hr_dev(ibqp->device);
	struct hns_roce_qp *hr_qp = to_hr_qp(ibqp);
	struct device *dev = &hr_dev->pdev->dev;
	dma_addr_t dma_handle = 0;
	u64 *mtts = NULL;

	/* search qp buf's mtts */
	mtts = hns_roce_table_find(&hr_dev->mr_table.mtt_table,
				hr_qp->mtt.first_seg, &dma_handle);
	if (mtts == NULL) {
		dev_err(dev, "Qp buf pa find failed!\n");
		return -EINVAL;
	}

	if (RTR_2_RTS_ATTR_MASK_VALID(attr_mask)) {
		dev_err(dev, "RTR2RTS attr_mask error!\n");
		return -EINVAL;
	}

	context->rx_cur_sq_wqe_ba_l = (u32)(mtts[0]);

	roce_set_field(context->qpc_bytes_120,
		       QP_CONTEXT_QPC_BYTES_120_RX_CUR_SQ_WQE_BA_H_M,
		       QP_CONTEXT_QPC_BYTES_120_RX_CUR_SQ_WQE_BA_H_S,
			   (mtts[0]) >> 32);

	roce_set_field(context->qpc_bytes_124,
		       QP_CONTEXT_QPC_BYTES_124_RX_ACK_MSN_M,
		       QP_CONTEXT_QPC_BYTES_124_RX_ACK_MSN_S, 0);
	roce_set_field(context->qpc_bytes_124,
		       QP_CONTEXT_QPC_BYTES_124_IRRL_MSG_IDX_M,
		       QP_CONTEXT_QPC_BYTES_124_IRRL_MSG_IDX_S, 0);

	roce_set_field(context->qpc_bytes_128,
		       QP_CONTEXT_QPC_BYTES_128_RX_ACK_EPSN_M,
		       QP_CONTEXT_QPC_BYTES_128_RX_ACK_EPSN_S,
		       attr->sq_psn);
	roce_set_bit(context->qpc_bytes_128,
		     QP_CONTEXT_QPC_BYTES_128_RX_ACK_PSN_ERR_FLG_S, 0);
	roce_set_field(context->qpc_bytes_128,
		     QP_CONTEXT_QPC_BYTES_128_ACK_LAST_OPERATION_TYPE_M,
		     QP_CONTEXT_QPC_BYTES_128_ACK_LAST_OPERATION_TYPE_S,
		     0);
	roce_set_bit(context->qpc_bytes_128,
		     QP_CONTEXT_QPC_BYTES_128_IRRL_PSN_VLD_FLG_S, 0);

	roce_set_field(context->qpc_bytes_132,
		       QP_CONTEXT_QPC_BYTES_132_IRRL_PSN_M,
		       QP_CONTEXT_QPC_BYTES_132_IRRL_PSN_S, 0);
	roce_set_field(context->qpc_bytes_132,
		       QP_CONTEXT_QPC_BYTES_132_IRRL_TAIL_M,
		       QP_CONTEXT_QPC_BYTES_132_IRRL_TAIL_S, 0);

	roce_set_field(context->qpc_bytes_136,
		       QP_CONTEXT_QPC_BYTES_136_RETRY_MSG_PSN_M,
		       QP_CONTEXT_QPC_BYTES_136_RETRY_MSG_PSN_S,
		       attr->sq_psn);
	roce_set_field(context->qpc_bytes_136,
		       QP_CONTEXT_QPC_BYTES_136_RETRY_MSG_FPKT_PSN_L_M,
		       QP_CONTEXT_QPC_BYTES_136_RETRY_MSG_FPKT_PSN_L_S,
		       attr->sq_psn);

	roce_set_field(context->qpc_bytes_140,
		       QP_CONTEXT_QPC_BYTES_140_RETRY_MSG_FPKT_PSN_H_M,
		       QP_CONTEXT_QPC_BYTES_140_RETRY_MSG_FPKT_PSN_H_S,
		       (attr->sq_psn >> SQ_PSN_SHIFT));
	roce_set_field(context->qpc_bytes_140,
		       QP_CONTEXT_QPC_BYTES_140_RETRY_MSG_MSN_M,
		       QP_CONTEXT_QPC_BYTES_140_RETRY_MSG_MSN_S, 0);
	roce_set_bit(context->qpc_bytes_140,
		     QP_CONTEXT_QPC_BYTES_140_RNR_RETRY_FLG_S, 0);

	roce_set_field(context->qpc_bytes_144,
		       QP_CONTEXT_QPC_BYTES_144_QP_STATE_M,
		       QP_CONTEXT_QPC_BYTES_144_QP_STATE_S,
		       attr->qp_state);

	roce_set_field(context->qpc_bytes_148,
		       QP_CONTEXT_QPC_BYTES_148_CHECK_FLAG_M,
		       QP_CONTEXT_QPC_BYTES_148_CHECK_FLAG_S, 0);
	roce_set_field(context->qpc_bytes_148,
		       QP_CONTEXT_QPC_BYTES_148_RETRY_COUNT_M,
		       QP_CONTEXT_QPC_BYTES_148_RETRY_COUNT_S,
		       attr->retry_cnt);
	roce_set_field(context->qpc_bytes_148,
		       QP_CONTEXT_QPC_BYTES_148_RNR_RETRY_COUNT_M,
		       QP_CONTEXT_QPC_BYTES_148_RNR_RETRY_COUNT_S,
		       attr->rnr_retry);
	roce_set_field(context->qpc_bytes_148,
		       QP_CONTEXT_QPC_BYTES_148_LSN_M,
		       QP_CONTEXT_QPC_BYTES_148_LSN_S, 0x100);

	context->rnr_retry = 0;

	roce_set_field(context->qpc_bytes_156,
		       QP_CONTEXT_QPC_BYTES_156_RETRY_COUNT_INIT_M,
		       QP_CONTEXT_QPC_BYTES_156_RETRY_COUNT_INIT_S,
		       attr->retry_cnt);
	if (attr->timeout < 0x12) {
		dev_info(dev, "ack timeout value(0x%x) must bigger than 0x12.\n",
			attr->timeout);
		roce_set_field(context->qpc_bytes_156,
			QP_CONTEXT_QPC_BYTES_156_ACK_TIMEOUT_M,
			QP_CONTEXT_QPC_BYTES_156_ACK_TIMEOUT_S,
			0x12);
	} else
		roce_set_field(context->qpc_bytes_156,
			QP_CONTEXT_QPC_BYTES_156_ACK_TIMEOUT_M,
			QP_CONTEXT_QPC_BYTES_156_ACK_TIMEOUT_S,
			attr->timeout);
	roce_set_field(context->qpc_bytes_156,
		       QP_CONTEXT_QPC_BYTES_156_RNR_RETRY_COUNT_INIT_M,
		       QP_CONTEXT_QPC_BYTES_156_RNR_RETRY_COUNT_INIT_S,
		       attr->rnr_retry);
	roce_set_field(context->qpc_bytes_156,
		       QP_CONTEXT_QPC_BYTES_156_PORT_NUM_M,
		       QP_CONTEXT_QPC_BYTES_156_PORT_NUM_S,
		       hr_qp->port);
	roce_set_field(context->qpc_bytes_156,
		       QP_CONTEXT_QPC_BYTES_156_SL_M,
		       QP_CONTEXT_QPC_BYTES_156_SL_S, attr->ah_attr.sl);
	hr_qp->sl = attr->ah_attr.sl;
	roce_set_field(context->qpc_bytes_156,
		       QP_CONTEXT_QPC_BYTES_156_INITIATOR_DEPTH_M,
		       QP_CONTEXT_QPC_BYTES_156_INITIATOR_DEPTH_S,
			       ilog2((unsigned int)attr->max_rd_atomic));
	roce_set_field(context->qpc_bytes_156,
		       QP_CONTEXT_QPC_BYTES_156_ACK_REQ_IND_M,
		       QP_CONTEXT_QPC_BYTES_156_ACK_REQ_IND_S, 0);
	context->pkt_use_len = 0;

	roce_set_field(context->qpc_bytes_164,
		       QP_CONTEXT_QPC_BYTES_164_SQ_PSN_M,
		       QP_CONTEXT_QPC_BYTES_164_SQ_PSN_S, attr->sq_psn);
	roce_set_field(context->qpc_bytes_164,
		       QP_CONTEXT_QPC_BYTES_164_IRRL_HEAD_M,
		       QP_CONTEXT_QPC_BYTES_164_IRRL_HEAD_S, 0);

	roce_set_field(context->qpc_bytes_168,
		       QP_CONTEXT_QPC_BYTES_168_RETRY_SQ_PSN_M,
		       QP_CONTEXT_QPC_BYTES_168_RETRY_SQ_PSN_S,
		       attr->sq_psn);
	roce_set_field(context->qpc_bytes_168,
		       QP_CONTEXT_QPC_BYTES_168_SGE_USE_FLA_M,
		       QP_CONTEXT_QPC_BYTES_168_SGE_USE_FLA_S, 0);
	roce_set_field(context->qpc_bytes_168,
		       QP_CONTEXT_QPC_BYTES_168_DB_TYPE_M,
		       QP_CONTEXT_QPC_BYTES_168_DB_TYPE_S, 0);
	roce_set_bit(context->qpc_bytes_168,
		     QP_CONTEXT_QPC_BYTES_168_MSG_LP_IND_S, 0);
	roce_set_bit(context->qpc_bytes_168,
		     QP_CONTEXT_QPC_BYTES_168_CSDB_LP_IND_S, 0);
	roce_set_bit(context->qpc_bytes_168,
		     QP_CONTEXT_QPC_BYTES_168_QP_ERR_FLG_S, 0);
	context->sge_use_len = 0;

	roce_set_field(context->qpc_bytes_176,
		       QP_CONTEXT_QPC_BYTES_176_DB_CUR_INDEX_M,
		       QP_CONTEXT_QPC_BYTES_176_DB_CUR_INDEX_S, 0);
	roce_set_field(context->qpc_bytes_176,
		       QP_CONTEXT_QPC_BYTES_176_RETRY_DB_CUR_INDEX_M,
		       QP_CONTEXT_QPC_BYTES_176_RETRY_DB_CUR_INDEX_S,
		       0);
	roce_set_field(context->qpc_bytes_180,
		       QP_CONTEXT_QPC_BYTES_180_SQ_CUR_INDEX_M,
		       QP_CONTEXT_QPC_BYTES_180_SQ_CUR_INDEX_S, 0);
	roce_set_field(context->qpc_bytes_180,
		       QP_CONTEXT_QPC_BYTES_180_SQ_HEAD_M,
		       QP_CONTEXT_QPC_BYTES_180_SQ_HEAD_S, 0);

	context->tx_cur_sq_wqe_ba_l = (u32)(mtts[0]);

	roce_set_field(context->qpc_bytes_188,
		       QP_CONTEXT_QPC_BYTES_188_TX_CUR_SQ_WQE_BA_H_M,
		       QP_CONTEXT_QPC_BYTES_188_TX_CUR_SQ_WQE_BA_H_S,
			   (mtts[0]) >> 32);
	roce_set_bit(context->qpc_bytes_188,
		     QP_CONTEXT_QPC_BYTES_188_PKT_RETRY_FLG_S, 0);
	roce_set_field(context->qpc_bytes_188,
		       QP_CONTEXT_QPC_BYTES_188_TX_RETRY_CUR_INDEX_M,
		       QP_CONTEXT_QPC_BYTES_188_TX_RETRY_CUR_INDEX_S,
		       0);

	hr_qp->psn_head = attr->sq_psn;
	hr_qp->psn_tail = attr->sq_psn;
	hr_qp->sq_last_wqe = 0;

	hr_qp->context = kzalloc(sizeof(struct hns_roce_qp_context),
		GFP_KERNEL);
	if (!hr_qp->context)
		return -ENOMEM;
	return 0;
}

static void modify_qp_init_to_init_bh(struct ib_qp *ibqp,
				const struct ib_qp_attr *attr, int attr_mask,
				enum ib_qp_state cur_state,
				enum ib_qp_state new_state)
{
	struct hns_roce_dev *hr_dev = to_hr_dev(ibqp->device);
	struct hns_roce_qp *hr_qp = to_hr_qp(ibqp);
	uint32_t doorbell[2] = {0};

	/*
	* Use rst2init to instead of init2init with drv, need to hw to flash
	* RQ HEAD by DB again.
	*/
	if (cur_state == IB_QPS_INIT && new_state == IB_QPS_INIT) {
		/* Memory barrier */
		wmb();

		roce_set_field(doorbell[0], RQ_DOORBELL_U32_4_RQ_HEAD_M,
			RQ_DOORBELL_U32_4_RQ_HEAD_S,
			hr_qp->rq.head);
		roce_set_field(doorbell[1], RQ_DOORBELL_U32_8_QPN_M,
			RQ_DOORBELL_U32_8_QPN_S, hr_qp->qpn);
		roce_set_field(doorbell[1], RQ_DOORBELL_U32_8_CMD_M,
			RQ_DOORBELL_U32_8_CMD_S, 1);
		roce_set_bit(doorbell[1], RQ_DOORBELL_U32_8_HW_SYNC_S,
			1);

		/*
		 * User mode will use the db chanel which reserved
		 * to kernel roce
		 */
		if (ibqp->uobject)
			hr_qp->rq.db_reg_l = hr_dev->reg_base +
				ROCEE_DB_OTHERS_L_0_REG +
				DB_REG_OFFSET * hr_dev->priv_uar.index;

		hns_roce_write64_k(doorbell, hr_qp->rq.db_reg_l);
	}

	hr_qp->state = new_state;

	if (attr_mask & IB_QP_MAX_DEST_RD_ATOMIC)
		hr_qp->resp_depth = attr->max_dest_rd_atomic;
	if (attr_mask & IB_QP_PORT)
		hr_qp->port = (attr->port_num - 1);

	if (new_state == IB_QPS_RESET && !ibqp->uobject) {
		hns_roce_v1_cq_clean(to_hr_cq(ibqp->recv_cq), hr_qp->qpn,
			ibqp->srq ? to_hr_srq(ibqp->srq) : NULL);
		if (ibqp->send_cq != ibqp->recv_cq)
			hns_roce_v1_cq_clean(to_hr_cq(ibqp->send_cq),
					hr_qp->qpn, NULL);

		hr_qp->rq.head = 0;
		hr_qp->rq.tail = 0;
		hr_qp->sq.head = 0;
		hr_qp->sq.tail = 0;
		hr_qp->sq_next_wqe = 0;
	}
}

static int hns_roce_v1_m_qp(struct ib_qp *ibqp, const struct ib_qp_attr *attr,
			    int attr_mask, enum ib_qp_state cur_state,
			    enum ib_qp_state new_state)
{
	struct hns_roce_dev *hr_dev = to_hr_dev(ibqp->device);
	struct hns_roce_qp *hr_qp = to_hr_qp(ibqp);
	struct device *dev = &hr_dev->pdev->dev;
	struct hns_roce_qp_context *context;
	int ret = 0;

	context = kzalloc(sizeof(*context), GFP_KERNEL);
	if (!context)
		return -ENOMEM;


	/*
	*Reset to init
	*	Mandatory param:
	*	IB_QP_STATE | IB_QP_PKEY_INDEX | IB_QP_PORT | IB_QP_ACCESS_FLAGS
	*	Optional param: NA
	*/
	if (cur_state == IB_QPS_RESET && new_state == IB_QPS_INIT)
		modify_qp_rst_to_init(ibqp, attr, context);
	else if (cur_state == IB_QPS_INIT && new_state == IB_QPS_INIT)
		modify_qp_init_to_init(ibqp, attr, attr_mask, context);
	else if (cur_state == IB_QPS_INIT && new_state == IB_QPS_RTR) {
		ret = modify_qp_init_to_rtr(ibqp, attr, attr_mask, context);
		if (ret)
			goto out;
	} else if (cur_state == IB_QPS_RTR && new_state == IB_QPS_RTS) {
		ret = modify_qp_rtr_to_rts(ibqp, attr, attr_mask, context);
		if (ret)
			goto out;
	} else if (QP_OTHERS_SUPPORT_STATE(cur_state, new_state)) {
		roce_set_field(context->qpc_bytes_144,
			       QP_CONTEXT_QPC_BYTES_144_QP_STATE_M,
			       QP_CONTEXT_QPC_BYTES_144_QP_STATE_S,
			       new_state);
	} else {
		dev_err(dev, "Not support state %d to state %d modify.\n",
			cur_state, new_state);
		goto out;
	}

	/* Every status migrate must change state */
	roce_set_field(context->qpc_bytes_144,
		       QP_CONTEXT_QPC_BYTES_144_QP_STATE_M,
		       QP_CONTEXT_QPC_BYTES_144_QP_STATE_S, new_state);

	/* SW pass context to HW */
	ret = hns_roce_v1_qp_modify(hr_dev, &hr_qp->mtt,
				    to_hns_roce_state(cur_state),
				    to_hns_roce_state(new_state), context,
				    hr_qp);
	if (ret) {
		dev_err(dev, "Hns_roce_qp_modify failed(%d)!\n", ret);
		goto out;
	}

	modify_qp_init_to_init_bh(ibqp, attr, attr_mask, cur_state, new_state);

out:
	kfree(context);
	return ret;
}

int hns_roce_v1_modify_qp(struct ib_qp *ibqp, const struct ib_qp_attr *attr,
			  int attr_mask, enum ib_qp_state cur_state,
			  enum ib_qp_state new_state)
{

	if (ibqp->qp_type == IB_QPT_GSI || ibqp->qp_type == IB_QPT_SMI)
		return hns_roce_v1_m_sqp(ibqp, attr, attr_mask, cur_state,
					 new_state);
	else
		return hns_roce_v1_m_qp(ibqp, attr, attr_mask, cur_state,
					new_state);
}

static enum ib_qp_state to_ib_qp_state(enum hns_roce_qp_state state)
{
	switch (state) {
	case HNS_ROCE_QP_STATE_RST:
		return IB_QPS_RESET;
	case HNS_ROCE_QP_STATE_INIT:
		return IB_QPS_INIT;
	case HNS_ROCE_QP_STATE_RTR:
		return IB_QPS_RTR;
	case HNS_ROCE_QP_STATE_RTS:
		return IB_QPS_RTS;
	case HNS_ROCE_QP_STATE_SQD:
		return IB_QPS_SQD;
	case HNS_ROCE_QP_STATE_ERR:
		return IB_QPS_ERR;
	default:
		return IB_QPS_ERR;
	}
}

static int hns_roce_v1_query_qpc(struct hns_roce_dev *hr_dev,
				 struct hns_roce_qp *hr_qp,
				 struct hns_roce_qp_context *hr_context)
{
	struct hns_roce_cmd_mailbox *mailbox;
	int ret;

	mailbox = hns_roce_alloc_cmd_mailbox(hr_dev);
	if (IS_ERR(mailbox)) {
		dev_err(&hr_dev->pdev->dev, "Alloc mailbox failed while query qpc!\n");
		return PTR_ERR(mailbox);
	}

	ret = hns_roce_cmd_mbox(hr_dev, 0, mailbox->dma, hr_qp->qpn, 0,
				HNS_ROCE_CMD_QUERY_QP,
				HNS_ROCE_CMD_TIME_CLASS_A);
	if (!ret)
		memcpy(hr_context, mailbox->buf, sizeof(*hr_context));
	else
		dev_err(&hr_dev->pdev->dev, "QUERY QP cmd process failed(%d)!\n",
			ret);

	hns_roce_free_cmd_mailbox(hr_dev, mailbox);

	return ret;
}

int hns_roce_v1_q_sqp(struct ib_qp *ibqp, struct ib_qp_attr *qp_attr,
			 int qp_attr_mask, struct ib_qp_init_attr *qp_init_attr)
{
	struct hns_roce_dev *hr_dev = to_hr_dev(ibqp->device);
	struct hns_roce_qp *hr_qp = to_hr_qp(ibqp);
	struct hns_roce_sqp_context *context;
	u32 addr = 0;

	context = kzalloc(sizeof(*context), GFP_KERNEL);
	if (!context)
		return -ENOMEM;

	mutex_lock(&hr_qp->mutex);

	if (hr_qp->state == IB_QPS_RESET) {
		qp_attr->qp_state = IB_QPS_RESET;
		goto done;
	}

	addr = ROCEE_QP1C_CFG0_0_REG +
			hr_qp->port * sizeof(*context);
	context->qp1c_bytes_4 = roce_read(hr_dev, addr);
	context->sq_rq_bt_l = roce_read(hr_dev, addr + 1);
	context->qp1c_bytes_12 = roce_read(hr_dev, addr + 2);
	context->qp1c_bytes_16 = roce_read(hr_dev, addr + 3);
	context->qp1c_bytes_20 = roce_read(hr_dev, addr + 4);
	context->cur_rq_wqe_ba_l = roce_read(hr_dev, addr + 5);
	context->qp1c_bytes_28 = roce_read(hr_dev, addr + 6);
	context->qp1c_bytes_32 = roce_read(hr_dev, addr + 7);
	context->cur_sq_wqe_ba_l = roce_read(hr_dev, addr + 8);

	hr_qp->state	= roce_get_field(context->qp1c_bytes_4,
		QP1C_BYTES_4_QP_STATE_SHIFT_M, QP1C_BYTES_4_QP_STATE_SHIFT_S);
	qp_attr->qp_state	= hr_qp->state;
	qp_attr->path_mtu	= IB_MTU_256;
	qp_attr->path_mig_state	= IB_MIG_ARMED;
	qp_attr->qkey		= 0x80010000;
	qp_attr->rq_psn		= 0;
	qp_attr->sq_psn		= 0;
	qp_attr->dest_qp_num	= 1;
	qp_attr->qp_access_flags = 6;

	qp_attr->pkey_index	= roce_get_field(context->qp1c_bytes_20,
		QP1C_BYTES_20_PKEY_IDX_M, QP1C_BYTES_20_PKEY_IDX_S);
	qp_attr->port_num	= roce_get_field(context->qp1c_bytes_16,
		QP1C_BYTES_16_PORT_NUM_M, QP1C_BYTES_16_PORT_NUM_S) + 1;

	qp_attr->sq_draining		= 0;
	qp_attr->max_rd_atomic		= 0;
	qp_attr->max_dest_rd_atomic	= 0;
	qp_attr->min_rnr_timer		= 0;
	qp_attr->timeout		= 0;
	qp_attr->retry_cnt		= 0;
	qp_attr->rnr_retry		= 0;
	qp_attr->alt_timeout		= 0;

done:
	qp_attr->cur_qp_state		= qp_attr->qp_state;
	qp_attr->cap.max_recv_wr	= hr_qp->rq.wqe_cnt;
	qp_attr->cap.max_recv_sge	= hr_qp->rq.max_gs;
	qp_attr->cap.max_send_wr	= hr_qp->sq.wqe_cnt;
	qp_attr->cap.max_send_sge	= hr_qp->sq.max_gs;
	qp_attr->cap.max_inline_data	= 0;
	qp_init_attr->cap		= qp_attr->cap;
	qp_init_attr->create_flags	= 0;

	mutex_unlock(&hr_qp->mutex);
	kfree(context);
	context = NULL;
	return 0;
}

int hns_roce_v1_q_qp(struct ib_qp *ibqp, struct ib_qp_attr *qp_attr,
			 int qp_attr_mask, struct ib_qp_init_attr *qp_init_attr)
{
	struct hns_roce_dev *hr_dev = to_hr_dev(ibqp->device);
	struct hns_roce_qp *hr_qp = to_hr_qp(ibqp);
	struct device *dev = &hr_dev->pdev->dev;
	struct hns_roce_qp_context *context;
	int tmp_qp_state = 0;
	int ret = 0;
	int state;

	context = kzalloc(sizeof(*context), GFP_KERNEL);
	if (!context)
		return -ENOMEM;

	memset(qp_attr, 0, sizeof(*qp_attr));
	memset(qp_init_attr, 0, sizeof(*qp_init_attr));

	mutex_lock(&hr_qp->mutex);

	if (hr_qp->state == IB_QPS_RESET) {
		qp_attr->qp_state = IB_QPS_RESET;
		goto done;
	}

	ret = hns_roce_v1_query_qpc(hr_dev, hr_qp, context);
	if (ret) {
		dev_err(dev, "Query qp(0x%x) failed!\n", ibqp->qp_num);
		goto out;
	}

	state = roce_get_field(context->qpc_bytes_144,
			       QP_CONTEXT_QPC_BYTES_144_QP_STATE_M,
			       QP_CONTEXT_QPC_BYTES_144_QP_STATE_S);
	tmp_qp_state = (int)to_ib_qp_state((enum hns_roce_qp_state)state);
	if (tmp_qp_state == -1) {
		dev_err(dev, "to_ib_qp_state error\n");
		ret = -EINVAL;
		goto out;
	}
	hr_qp->state = (u8)tmp_qp_state;
	qp_attr->qp_state = (enum ib_qp_state)hr_qp->state;
	qp_attr->path_mtu = (enum ib_mtu)roce_get_field(context->qpc_bytes_48,
					       QP_CONTEXT_QPC_BYTES_48_MTU_M,
					       QP_CONTEXT_QPC_BYTES_48_MTU_S);
	qp_attr->path_mig_state = IB_MIG_ARMED;
	if (hr_qp->ibqp.qp_type == IB_QPT_UD)
		qp_attr->qkey = QKEY_VAL;

	qp_attr->rq_psn = roce_get_field(context->qpc_bytes_88,
					 QP_CONTEXT_QPC_BYTES_88_RX_REQ_EPSN_M,
					 QP_CONTEXT_QPC_BYTES_88_RX_REQ_EPSN_S);
	qp_attr->sq_psn = roce_get_field(context->qpc_bytes_140,
			QP_CONTEXT_QPC_BYTES_140_RETRY_MSG_FPKT_PSN_H_M,
			QP_CONTEXT_QPC_BYTES_140_RETRY_MSG_FPKT_PSN_H_S) << 8;
	qp_attr->sq_psn += roce_get_field(context->qpc_bytes_136,
			QP_CONTEXT_QPC_BYTES_136_RETRY_MSG_FPKT_PSN_L_M,
			QP_CONTEXT_QPC_BYTES_136_RETRY_MSG_FPKT_PSN_L_S);
	qp_attr->dest_qp_num = (u8)roce_get_field(context->qpc_bytes_36,
					QP_CONTEXT_QPC_BYTES_36_DEST_QP_M,
					QP_CONTEXT_QPC_BYTES_36_DEST_QP_S);
	qp_attr->qp_access_flags =
		((roce_get_bit(context->qpc_bytes_4,
			QP_CONTEXT_QPC_BYTE_4_RDMA_READ_ENABLE_S)) << 2) |
		((roce_get_bit(context->qpc_bytes_4,
			QP_CONTEXT_QPC_BYTE_4_RDMA_WRITE_ENABLE_S)) << 1) |
		((roce_get_bit(context->qpc_bytes_4,
			QP_CONTEXT_QPC_BYTE_4_ATOMIC_OPERATION_ENABLE_S)) << 3);

	if (hr_qp->ibqp.qp_type == IB_QPT_RC ||
	    hr_qp->ibqp.qp_type == IB_QPT_UC) {
		qp_attr->ah_attr.sl = roce_get_field(context->qpc_bytes_156,
					QP_CONTEXT_QPC_BYTES_156_SL_M,
					QP_CONTEXT_QPC_BYTES_156_SL_S);
		qp_attr->ah_attr.grh.flow_label =
					roce_get_field(context->qpc_bytes_48,
					QP_CONTEXT_QPC_BYTES_48_FLOWLABEL_M,
					QP_CONTEXT_QPC_BYTES_48_FLOWLABEL_S);
		qp_attr->ah_attr.grh.sgid_index =
					roce_get_field(context->qpc_bytes_36,
					QP_CONTEXT_QPC_BYTES_36_SGID_INDEX_M,
					QP_CONTEXT_QPC_BYTES_36_SGID_INDEX_S);
		qp_attr->ah_attr.grh.hop_limit =
					roce_get_field(context->qpc_bytes_44,
					QP_CONTEXT_QPC_BYTES_44_HOPLMT_M,
					QP_CONTEXT_QPC_BYTES_44_HOPLMT_S);
		qp_attr->ah_attr.grh.traffic_class = roce_get_field(
					context->qpc_bytes_48,
					QP_CONTEXT_QPC_BYTES_48_TCLASS_M,
					QP_CONTEXT_QPC_BYTES_48_TCLASS_S);

		memcpy(qp_attr->ah_attr.grh.dgid.raw, context->dgid,
		       sizeof(qp_attr->ah_attr.grh.dgid.raw));
	}

	qp_attr->pkey_index = roce_get_field(context->qpc_bytes_12,
			      QP_CONTEXT_QPC_BYTES_12_P_KEY_INDEX_M,
			      QP_CONTEXT_QPC_BYTES_12_P_KEY_INDEX_S);
	qp_attr->port_num = (u8)roce_get_field(context->qpc_bytes_156,
			     QP_CONTEXT_QPC_BYTES_156_PORT_NUM_M,
			     QP_CONTEXT_QPC_BYTES_156_PORT_NUM_S) + 1;
	qp_attr->sq_draining = 0;
	qp_attr->max_rd_atomic = roce_get_field(context->qpc_bytes_156,
				 QP_CONTEXT_QPC_BYTES_156_INITIATOR_DEPTH_M,
				 QP_CONTEXT_QPC_BYTES_156_INITIATOR_DEPTH_S);
	qp_attr->max_dest_rd_atomic = roce_get_field(context->qpc_bytes_32,
				 QP_CONTEXT_QPC_BYTES_32_RESPONDER_RESOURCES_M,
				 QP_CONTEXT_QPC_BYTES_32_RESPONDER_RESOURCES_S);
	qp_attr->min_rnr_timer = (u8)(roce_get_field(context->qpc_bytes_24,
			QP_CONTEXT_QPC_BYTES_24_MINIMUM_RNR_NAK_TIMER_M,
			QP_CONTEXT_QPC_BYTES_24_MINIMUM_RNR_NAK_TIMER_S));
	qp_attr->timeout = (u8)(roce_get_field(context->qpc_bytes_156,
			    QP_CONTEXT_QPC_BYTES_156_ACK_TIMEOUT_M,
			    QP_CONTEXT_QPC_BYTES_156_ACK_TIMEOUT_S));
	qp_attr->retry_cnt = roce_get_field(context->qpc_bytes_148,
			     QP_CONTEXT_QPC_BYTES_148_RETRY_COUNT_M,
			     QP_CONTEXT_QPC_BYTES_148_RETRY_COUNT_S);
	qp_attr->rnr_retry = context->rnr_retry;

done:
	qp_attr->cur_qp_state = qp_attr->qp_state;
	qp_attr->cap.max_recv_wr = hr_qp->rq.wqe_cnt;
	qp_attr->cap.max_recv_sge = hr_qp->rq.max_gs;

	if (!ibqp->uobject) {
		qp_attr->cap.max_send_wr = hr_qp->sq.wqe_cnt;
		qp_attr->cap.max_send_sge = hr_qp->sq.max_gs;
	} else {
		qp_attr->cap.max_send_wr = 0;
		qp_attr->cap.max_send_sge = 0;
	}

	qp_init_attr->cap = qp_attr->cap;

out:
	mutex_unlock(&hr_qp->mutex);
	kfree(context);
	return ret;
}
int hns_roce_v1_query_qp(struct ib_qp *ibqp, struct ib_qp_attr *qp_attr,
			 int qp_attr_mask, struct ib_qp_init_attr *qp_init_attr)
{
	struct hns_roce_qp *hr_qp = to_hr_qp(ibqp);

	return hr_qp->doorbell_qpn <= 1 ?
		hns_roce_v1_q_sqp(ibqp, qp_attr, qp_attr_mask, qp_init_attr) :
		hns_roce_v1_q_qp(ibqp, qp_attr, qp_attr_mask, qp_init_attr);
}

static void hns_roce_v1_check_sdb_statue(struct hns_roce_dev *hr_dev,
					 u32 *send_ptr_old,
					 u32 *retry_cnt_old,
					 u32 *tsp_bp_st,
					 u32 *flag)
{
	u32 retry_cnt;
	u32 send_ptr;
	u32 send_ptr_bit;
	u32 cur_cnt, old_cnt;

	retry_cnt = roce_read(hr_dev, ROCEE_SDB_RETRY_CNT_REG);
	send_ptr = roce_read(hr_dev, ROCEE_SDB_SEND_PTR_REG);
	cur_cnt = roce_get_field(send_ptr, ROCEE_SDB_SEND_PTR_SDB_SEND_PTR_M,
				 ROCEE_SDB_SEND_PTR_SDB_SEND_PTR_S) +
		  roce_get_field(retry_cnt, ROCEE_SDB_RETRY_CNT_SDB_RETRY_CNT_M,
				 ROCEE_SDB_RETRY_CNT_SDB_RETRY_CNT_S);
	if (!roce_get_bit(*tsp_bp_st, ROCEE_CNT_CLR_CE_CNT_CLR_CE_S)) {
		old_cnt = roce_get_field(*send_ptr_old,
					 ROCEE_SDB_SEND_PTR_SDB_SEND_PTR_M,
					 ROCEE_SDB_SEND_PTR_SDB_SEND_PTR_S) +
			  roce_get_field(*retry_cnt_old,
					 ROCEE_SDB_RETRY_CNT_SDB_RETRY_CNT_M,
					 ROCEE_SDB_RETRY_CNT_SDB_RETRY_CNT_S);
		*flag = (cur_cnt - old_cnt > 8) ? 1 : 0;
	} else {
		old_cnt = roce_get_field(*send_ptr_old,
					 ROCEE_SDB_SEND_PTR_SDB_SEND_PTR_M,
					 ROCEE_SDB_SEND_PTR_SDB_SEND_PTR_S);
		if (cur_cnt - old_cnt > 8) {
			*flag = 1;
		} else {
			send_ptr_bit = roce_get_field(*send_ptr_old,
				       ROCEE_SDB_SEND_PTR_SDB_SEND_PTR_M,
				       ROCEE_SDB_SEND_PTR_SDB_SEND_PTR_S) +
				       roce_get_field(retry_cnt,
				       ROCEE_SDB_RETRY_CNT_SDB_RETRY_CNT_M,
				       ROCEE_SDB_RETRY_CNT_SDB_RETRY_CNT_S);
			roce_set_field(*send_ptr_old,
				       ROCEE_SDB_SEND_PTR_SDB_SEND_PTR_M,
				       ROCEE_SDB_SEND_PTR_SDB_SEND_PTR_S,
				       send_ptr_bit);
			*flag = 0;
		}
	}
}

static int hns_roce_v1_destroy_qp_check(struct hns_roce_dev *hr_dev,
					struct hns_roce_qp *hr_qp,
					u32 *sdb_issue_ptr,
					u32 *sdb_inv_cnt, u32 *wait_stage)
{
	struct device *dev = &hr_dev->pdev->dev;
	u32 sdb_send_ptr;
	u32 sdb_send_ptr_old;
	u32 sdb_inv_cnt_tmp;
	u32 tsp_bp_st;
	u32 sdb_retry_cnt_old;
	unsigned long end =
		msecs_to_jiffies(HNS_QP_DESTROY_TIMEOUT_MSECS) + jiffies;
	u32 success_flags = 0;

	if (*wait_stage > ROCE_DB_WAIT_STAGE2 ||
		*wait_stage < ROCE_DB_WAIT_STAGE1) {
		dev_err(dev, "Db wait stage(%d) error in destroy qp check!\n",
			*wait_stage);
		return -EINVAL;
	}

	if (*wait_stage == ROCE_DB_WAIT_STAGE1) {
		/* Query db process status, until hw process completely */
		sdb_send_ptr = roce_read(hr_dev, ROCEE_SDB_SEND_PTR_REG);
		while (BITS28_CMP_CHECK(roce_get_field(sdb_send_ptr,
				ROCEE_SDB_SEND_PTR_SDB_SEND_PTR_M,
				ROCEE_SDB_SEND_PTR_SDB_SEND_PTR_S),
				roce_get_field(*sdb_issue_ptr,
				ROCEE_SDB_ISSUE_PTR_SDB_ISSUE_PTR_M,
				ROCEE_SDB_ISSUE_PTR_SDB_ISSUE_PTR_S)) < 0) {
			if (!time_before(jiffies, end)) {
				dev_info(dev, "destroy qp(0x%lx) timeout. SdbIsusePr = 0x%x, SdbSendPtr = 0x%x\n",
					hr_qp->qpn, *sdb_issue_ptr,
					sdb_send_ptr);
				return 0;
			}
			sdb_send_ptr = roce_read(hr_dev,
				ROCEE_SDB_SEND_PTR_REG);
			msleep(DB_WAIT_TIMEOUT_VALUE);
		}

		if (roce_get_field(*sdb_issue_ptr,
				ROCEE_SDB_ISSUE_PTR_SDB_ISSUE_PTR_M,
				ROCEE_SDB_ISSUE_PTR_SDB_ISSUE_PTR_S) ==
			roce_get_field(sdb_send_ptr,
				ROCEE_SDB_SEND_PTR_SDB_SEND_PTR_M,
				ROCEE_SDB_SEND_PTR_SDB_SEND_PTR_S)) {
			sdb_send_ptr_old = roce_read(hr_dev,
				ROCEE_SDB_SEND_PTR_REG);
			sdb_retry_cnt_old = roce_read(hr_dev,
				ROCEE_SDB_RETRY_CNT_REG);

			do {
				tsp_bp_st = roce_read(hr_dev,
					ROCEE_TSP_BP_ST_REG);
				if (roce_get_bit(tsp_bp_st,
					ROCEE_TSP_BP_ST_QH_FIFO_ENTRY_S) ==
					1) {
					*wait_stage = ROCE_DB_WAIT_OK;
					return 0;
				}

				if (!time_before(jiffies, end)) {
					dev_info(dev, "destroy qp(0x%lx) timeout while send ptr equal isuse ptr.",
						hr_qp->qpn);
					dev_info(dev, "SdbIsusePr = 0x%x, SdbSendPtr = 0x%x\n",
						*sdb_issue_ptr,
						sdb_send_ptr);
					return 0;
				}

				msleep(DB_WAIT_TIMEOUT_VALUE);

				hns_roce_v1_check_sdb_statue(hr_dev,
							     &sdb_send_ptr_old,
							     &sdb_retry_cnt_old,
							     &tsp_bp_st,
							     &success_flags);
			} while (!success_flags);
		}

		*wait_stage = ROCE_DB_WAIT_STAGE2;

		/* Get list pointer */
		*sdb_inv_cnt = roce_read(hr_dev, ROCEE_SDB_INV_CNT_REG);
		dev_dbg(dev, "QP(0x%lx), SdbSendPtr = 0x%x\n",
			hr_qp->qpn, sdb_send_ptr);
		dev_dbg(dev, "QP(0x%lx), SdbInvCnt = 0x%x\n",
			hr_qp->qpn, *sdb_inv_cnt);
	}

	if (*wait_stage == ROCE_DB_WAIT_STAGE2) {
		/* Query db's list status, until hw reversal */
		sdb_inv_cnt_tmp = roce_read(hr_dev, ROCEE_SDB_INV_CNT_REG);
		while ((short)((u16)roce_get_field(sdb_inv_cnt_tmp,
					ROCEE_SDB_INV_CNT_SDB_INV_CNT_M,
					ROCEE_SDB_INV_CNT_SDB_INV_CNT_S) -
				(u16)(*sdb_inv_cnt + 8)) < 0) {
			if (!time_before(jiffies, end)) {
				dev_info(dev, "destroy qp(0x%lx) timeout. SdbInvCnt = 0x%x\n",
						hr_qp->qpn, sdb_inv_cnt_tmp);
				return 0;
			}
			sdb_inv_cnt_tmp =
				roce_read(hr_dev, ROCEE_SDB_INV_CNT_REG);
			msleep(DB_WAIT_TIMEOUT_VALUE);
		}

		*wait_stage = ROCE_DB_WAIT_OK;

		dev_dbg(dev, "QP(0x%lx), SdbInvCntNew = 0x%x.\n",
			hr_qp->qpn, sdb_inv_cnt_tmp);
	}

	return 0;
}

static int hns_roce_v1_destroy_qp_to_rst(struct hns_roce_dev *hr_dev,
					 struct hns_roce_qp *hr_qp)
{
	struct hns_roce_qp_context *context;
	struct device *dev = &hr_dev->pdev->dev;
	int ret;

	context = kzalloc(sizeof(*context), GFP_KERNEL);
	if (!context)
		return -ENOMEM;

	roce_set_field(context->qpc_bytes_144,
		QP_CONTEXT_QPC_BYTES_144_QP_STATE_M,
		QP_CONTEXT_QPC_BYTES_144_QP_STATE_S, IB_QPS_RESET);

	ret = hns_roce_v1_qp_modify(hr_dev, &hr_qp->mtt,
		HNS_ROCE_QP_STATE_ERR, HNS_ROCE_QP_STATE_RST,
		context, hr_qp);
	if (ret) {
		dev_err(dev, "Qp modify failed(%d) while destroy qp to rst!\n",
			ret);
		goto out;
	}

	hr_qp->state = IB_QPS_RESET;

	hr_qp->rq.head = 0;
	hr_qp->rq.tail = 0;
	hr_qp->sq.head = 0;
	hr_qp->sq.tail = 0;
	hr_qp->sq_next_wqe = 0;
	kfree(hr_qp->context);

out:
	kfree(context);

	return ret;
}

static void hns_roce_v1_destroy_qp_bh(struct work_struct *work)
{
	struct hns_roce_qp_work *qp_work_entry;
	struct hns_roce_qp *hr_qp;
	struct hns_roce_dev *hr_dev;
	struct hns_roce_v1_priv *priv;
	struct device *dev;
	int ret;

	qp_work_entry = container_of(work, struct hns_roce_qp_work, work);
	hr_qp = (struct hns_roce_qp *)qp_work_entry->qp;
	hr_dev = to_hr_dev(qp_work_entry->ib_dev);
	dev = &hr_dev->pdev->dev;
	priv = (struct hns_roce_v1_priv *)hr_dev->hw->priv;

	dev_info(dev, "Schedule qp(0x%lx) destroy.\n", hr_qp->qpn);

	qp_work_entry->sche_cnt++;

	ret = hns_roce_v1_destroy_qp_check(hr_dev, hr_qp,
			&qp_work_entry->sdb_issue_ptr,
			&qp_work_entry->sdb_inv_cnt,
			&qp_work_entry->db_wait_stage);
	if (ret) {
		dev_err(dev, "Destroy qp check failed(%d)!\n", ret);
		WARN_ON(1);
		return;
	}

	if (qp_work_entry->db_wait_stage != ROCE_DB_WAIT_OK &&
		priv->requeue_flag) {
		queue_work(priv->des_qp_queue, work);
		return;
	}

	/* Modify qp to reset before destroying qp */
	if (hr_qp->doorbell_qpn < 2)
		dev_err(dev, "Qpn(0x%lx) error while destroy qp bh scheduling.\n",
			hr_qp->qpn);
	else {
		ret = hns_roce_v1_destroy_qp_to_rst(hr_dev, hr_qp);
		if (ret)
			dev_err(dev, "modify QP(0x%lx) to ERR failed(%d).\n",
				hr_qp->qpn, ret);
	}

	hns_roce_qp_remove(hr_dev, hr_qp);

	hns_roce_qp_free(hr_dev, hr_qp);

	if (V1_SUPPORT_QP_TYPE(hr_qp->ibqp.qp_type))
		hns_roce_release_range_qp(hr_dev, hr_qp->qpn, 1);

	dev_dbg(dev, "Schedule:qp(0x%lx) destroy OK.\n", hr_qp->qpn);

	kfree(hr_qp);

	kfree(qp_work_entry);
}

static int qp_state_reset_check(struct hns_roce_dev *hr_dev,
				struct hns_roce_qp *hr_qp,
				struct hns_roce_qp_work *qp_work_entry,
				int *is_timeout)
{
	struct device *dev = &hr_dev->pdev->dev;
	int ret;

	if (hr_qp->state != IB_QPS_RESET) {
		/* Set qp to ERR, waiting for hw complete processing all dbs */
		if (hr_qp->doorbell_qpn < 2) {
			ret = hns_roce_v1_m_sqp(&hr_qp->ibqp, NULL, 0,
					hr_qp->state, IB_QPS_ERR);
			if (ret) {
				dev_err(dev, "Modify SQP(0x%06lx) to ERR failed(%d)!\n",
					hr_qp->qpn, ret);
				return ret;
			}
		} else {
			ret = hns_roce_v1_m_qp(&hr_qp->ibqp, NULL, 0,
					hr_qp->state, IB_QPS_ERR);
			if (ret) {
				dev_err(dev, "Modify QP(0x%06lx) to ERR failed(%d)!\n",
					hr_qp->qpn, ret);
				return ret;
			}
		}

		/* Record issued doorbell */
		qp_work_entry->sdb_issue_ptr =
			roce_read(hr_dev, ROCEE_SDB_ISSUE_PTR_REG);
		dev_dbg(dev, "QP(0x%lx), SdbIsusePtr = 0x%x\n",
				hr_qp->qpn, qp_work_entry->sdb_issue_ptr);
		dev_dbg(dev, "QP(0x%lx), SdbSendPtr = 0x%x\n",
				hr_qp->qpn,
				roce_read(hr_dev, ROCEE_SDB_SEND_PTR_REG));

		/* Query db process status, until hw process completely */
		qp_work_entry->db_wait_stage = ROCE_DB_WAIT_STAGE1;
		ret = hns_roce_v1_destroy_qp_check(hr_dev, hr_qp,
				&qp_work_entry->sdb_issue_ptr,
				&qp_work_entry->sdb_inv_cnt,
				&qp_work_entry->db_wait_stage);
		if (ret) {
			dev_err(dev, "Destroy qp check failed(%d)!\n", ret);
			WARN_ON(1);
			return ret;
		}

		if (qp_work_entry->db_wait_stage != ROCE_DB_WAIT_OK) {
			qp_work_entry->sche_cnt = 0;
			*is_timeout = 1;
			return 0;
		}

		/* Modify qp to reset before destroying qp */
		if (hr_qp->doorbell_qpn < 2) {
			ret = hns_roce_v1_m_sqp(&hr_qp->ibqp, NULL, 0,
					hr_qp->state, IB_QPS_RESET);
			if (ret) {
				dev_err(dev, "Modify QP(0x%06lx) to RESET failed(%d)!\n",
					hr_qp->qpn, ret);
				return ret;
			}
		} else {
			ret = hns_roce_v1_m_qp(&hr_qp->ibqp, NULL, 0,
					hr_qp->state, IB_QPS_RESET);
			if (ret) {
				dev_err(dev, "Modify QP(0x%06lx) to RESET failed(%d)!\n",
					hr_qp->qpn, ret);
				return ret;
			}
		}
	}

	return 0;
}

static int hns_roce_v1_destroy_qp_common(struct hns_roce_dev *hr_dev,
					  struct hns_roce_qp *hr_qp,
					  int is_user)
{
	struct device *dev = &hr_dev->pdev->dev;
	struct hns_roce_sqp *hr_sqp = hr_to_hr_sqp(hr_qp);
	struct hns_roce_qp_work qp_work_entry;
	struct hns_roce_qp_work *qp_work;
	struct hns_roce_v1_priv *priv;
	struct hns_roce_cq *send_cq, *recv_cq;
	int is_timeout = 0;
	int ret;

	priv = (struct hns_roce_v1_priv *)hr_dev->hw->priv;

	ret = qp_state_reset_check(hr_dev, hr_qp, &qp_work_entry, &is_timeout);
	if (ret) {
		dev_err(dev, "Qp reset check failed(%d)!", ret);
		return ret;
	}

	if (!is_timeout) {
		send_cq = to_hr_cq(hr_qp->ibqp.send_cq);
		recv_cq = to_hr_cq(hr_qp->ibqp.recv_cq);

		hns_roce_lock_cqs(send_cq, recv_cq);

		hns_roce_v1_clean_cq(send_cq, recv_cq, hr_qp, is_user);

		hns_roce_unlock_cqs(send_cq, recv_cq);

		hns_roce_qp_remove(hr_dev, hr_qp);

		hns_roce_qp_free(hr_dev, hr_qp);

		/* Not special_QP, free their QPN */
		if (V1_SUPPORT_QP_TYPE(hr_qp->ibqp.qp_type))
			hns_roce_release_range_qp(hr_dev, hr_qp->qpn, 1);

		hns_roce_mtt_cleanup(hr_dev, &hr_qp->mtt);

		if (is_user) {
			if (hr_qp->ibqp.srq) {
				/* SRQ not support */
				dev_err(dev, "Srq(%p) not support at user mode(%d)!\n",
					hr_qp->ibqp.srq, is_user);
			}
			ib_umem_release(hr_qp->umem);
			hr_qp->umem = NULL;
		} else {
			kfree(hr_qp->sq.wrid);
			hr_qp->sq.wrid = NULL;
			kfree(hr_qp->rq.wrid);
			hr_qp->rq.wrid = NULL;
			hns_roce_buf_free(hr_dev, hr_qp->buff_size,
				&hr_qp->hr_buf);
		}

		if (hr_qp->ibqp.qp_type == IB_QPT_GSI) {
			kfree(hr_sqp);
			hr_sqp = NULL;
		} else {
			kfree(hr_qp);
			hr_qp = NULL;
		}
	} else {
		send_cq = to_hr_cq(hr_qp->ibqp.send_cq);
		recv_cq = to_hr_cq(hr_qp->ibqp.recv_cq);

		hns_roce_lock_cqs(send_cq, recv_cq);

		hns_roce_v1_clean_cq(send_cq, recv_cq, hr_qp, is_user);

		hns_roce_unlock_cqs(send_cq, recv_cq);

		hns_roce_mtt_cleanup(hr_dev, &hr_qp->mtt);

		if (is_user) {
			if (hr_qp->ibqp.srq) {
				/* SRQ not support */
				dev_err(dev, "Srq(%p) not support at user mode(%d)!\n",
					hr_qp->ibqp.srq, is_user);
			}
			ib_umem_release(hr_qp->umem);
			hr_qp->umem = NULL;
		} else {
			kfree(hr_qp->sq.wrid);
			hr_qp->sq.wrid = NULL;
			kfree(hr_qp->rq.wrid);
			hr_qp->rq.wrid = NULL;
			hns_roce_buf_free(hr_dev, hr_qp->buff_size,
				&hr_qp->hr_buf);
		}

		hr_qp->priv =
			kzalloc(sizeof(struct hns_roce_qp_work), GFP_KERNEL);
		if (!hr_qp->priv)
			return -ENOMEM;

		qp_work = (struct hns_roce_qp_work *)hr_qp->priv;

		INIT_WORK(&(qp_work->work), hns_roce_v1_destroy_qp_bh);
		qp_work->ib_dev = &(hr_dev->ib_dev);
		qp_work->db_wait_stage	= qp_work_entry.db_wait_stage;
		qp_work->sdb_issue_ptr	= qp_work_entry.sdb_issue_ptr;
		qp_work->sdb_inv_cnt	= qp_work_entry.sdb_inv_cnt;
		qp_work->sche_cnt	= qp_work_entry.sche_cnt;
		qp_work->qp		= (void *)hr_qp;
		queue_work(priv->des_qp_queue, &(qp_work->work));
	}

	return 0;
}

int hns_roce_v1_destroy_qp(struct ib_qp *ibqp)
{
	struct hns_roce_dev *hr_dev = to_hr_dev(ibqp->device);
	struct hns_roce_qp *hr_qp = to_hr_qp(ibqp);

	return hns_roce_v1_destroy_qp_common(hr_dev, hr_qp,
						!!ibqp->pd->uobject);
}

static struct hns_roce_qp *hns_roce_v1_create_lp_qp(struct hns_roce_dev *hr_dev,
						    struct ib_pd *pd)
{
	struct device *dev = &hr_dev->pdev->dev;
	struct ib_qp_init_attr init_attr;
	struct ib_qp *qp;

	memset(&init_attr, 0, sizeof(struct ib_qp_init_attr));
	init_attr.qp_type = IB_QPT_RC;
	init_attr.sq_sig_type = IB_SIGNAL_ALL_WR;
	init_attr.cap.max_recv_wr = HNS_ROCE_MIN_WQE_NUM;
	init_attr.cap.max_send_wr = HNS_ROCE_MIN_WQE_NUM;

	qp = hns_roce_create_qp(pd, &init_attr, NULL);
	if (IS_ERR(qp)) {
		dev_err(dev, "Create loop qp for mr free failed!");
		return NULL;
	}

	return to_hr_qp(qp);
}

static int hns_roce_v1_rsv_lp_qp(struct hns_roce_dev *hr_dev)
{
	struct device *dev = &hr_dev->pdev->dev;
	struct hns_roce_caps *caps = &hr_dev->caps;
	struct hns_roce_v1_priv *priv;
	struct hns_roce_qp *hr_qp;
	struct hns_roce_qp_context *context;
	struct ib_qp_attr attr = {0};
	struct ib_cq *cq;
	struct ib_pd *pd;
	u64 subnet_prefix;
	int attr_mask = 0;
	int cqe_num;
	int i;
	int j;
	int ret;
	u8 phy_port;
	u8 sl;

	priv = (struct hns_roce_v1_priv *)hr_dev->hw->priv;

	/* Reserved cq for loop qp */
	cqe_num = HNS_ROCE_MIN_WQE_NUM * 2;
	cq = hns_roce_ib_create_cq(&hr_dev->ib_dev, cqe_num, 0, NULL, NULL);
	if (IS_ERR(cq)) {
		dev_err(dev, "Create cq for reseved loop qp failed!");
		return -ENOMEM;
	}
	priv->mr_free_cq = to_hr_cq(cq);
	priv->mr_free_cq->ib_cq.device		= &hr_dev->ib_dev;
	priv->mr_free_cq->ib_cq.uobject		= NULL;
	priv->mr_free_cq->ib_cq.comp_handler	= NULL;
	priv->mr_free_cq->ib_cq.event_handler	= NULL;
	priv->mr_free_cq->ib_cq.cq_context	= NULL;
	atomic_set(&priv->mr_free_cq->ib_cq.usecnt, 0);

	pd = hns_roce_alloc_pd(&hr_dev->ib_dev, NULL, NULL);
	if (IS_ERR(pd)) {
		dev_err(dev, "Create pd for reseved loop qp failed!");
		ret = -ENOMEM;
		goto alloc_pd_failed;
	}
	priv->mr_free_pd = to_hr_pd(pd);
	priv->mr_free_pd->ibpd.device  = &hr_dev->ib_dev;
	priv->mr_free_pd->ibpd.uobject = NULL;
	atomic_set(&priv->mr_free_pd->ibpd.usecnt, 0);

	context = kzalloc(sizeof(*context), GFP_KERNEL);
	if (!context) {
		ret = -ENOMEM;
		goto alloc_context_failed;
	}

	attr.qp_access_flags	= IB_ACCESS_REMOTE_WRITE;
	attr.pkey_index		= 0;
	attr.min_rnr_timer	= 0;
	/* Disable read ability */
	attr.max_dest_rd_atomic = 0;
	attr.max_rd_atomic	= 0;
	/* Use arbitrary values as rq_psn and sq_psn */
	attr.rq_psn		= 0x0808;
	attr.sq_psn		= 0x0808;
	attr.retry_cnt		= 7;
	attr.rnr_retry		= 7;
	attr.timeout		= 0x12;
	attr.path_mtu		= IB_MTU_256;
	attr.ah_attr.ah_flags	= 1;
	attr.ah_attr.static_rate	= 3;
	attr.ah_attr.grh.sgid_index	= 0;
	attr.ah_attr.grh.hop_limit	= 1;
	attr.ah_attr.grh.flow_label	= 0;
	attr.ah_attr.grh.traffic_class	= 0;

	subnet_prefix = cpu_to_be64(0xfe80000000000000LL);
	for (i = 0; i < HNS_ROCE_V1_RESV_QP; i++) {
		priv->mr_free_qp[i] = hns_roce_v1_create_lp_qp(hr_dev, pd);
		if (IS_ERR(priv->mr_free_qp[i])) {
			dev_err(dev, "Create loop qp failed!\n");
			goto create_lp_qp_failed;
		}
		hr_qp = priv->mr_free_qp[i];

		sl = i / caps->num_ports;

		if (caps->num_ports == HNS_ROCE_MAX_PORTS)
			phy_port = (i >= HNS_ROCE_MAX_PORTS) ? (i - 2) :
				(i % caps->num_ports);
		else
			phy_port = i % caps->num_ports;

		hr_qp->port		= phy_port;
		hr_qp->ibqp.qp_type	= IB_QPT_RC;
		hr_qp->ibqp.device	= &hr_dev->ib_dev;
		hr_qp->ibqp.uobject	= NULL;
		atomic_set(&hr_qp->ibqp.usecnt, 0);
		hr_qp->ibqp.pd		= pd;
		hr_qp->ibqp.recv_cq	= cq;
		hr_qp->ibqp.send_cq	= cq;

		attr.ah_attr.port_num = phy_port + 1;
		attr.ah_attr.sl = sl;

		attr.dest_qp_num	= hr_qp->qpn;
		memcpy(attr.ah_attr.dmac, hr_dev->dev_addr[phy_port],
		       MAC_ADDR_OCTET_NUM);

		memcpy(attr.ah_attr.grh.dgid.raw,
			&subnet_prefix, sizeof(u64));
		memcpy(&attr.ah_attr.grh.dgid.raw[8],
		       hr_dev->dev_addr[phy_port], 3);
		memcpy(&attr.ah_attr.grh.dgid.raw[13],
		       hr_dev->dev_addr[phy_port] + 3, 3);
		attr.ah_attr.grh.dgid.raw[11] = 0xff;
		attr.ah_attr.grh.dgid.raw[12] = 0xfe;
		attr.ah_attr.grh.dgid.raw[8] ^= 2;

		modify_qp_rst_to_init(&hr_qp->ibqp, &attr, context);
		roce_set_field(context->qpc_bytes_144,
		       QP_CONTEXT_QPC_BYTES_144_QP_STATE_M,
		       QP_CONTEXT_QPC_BYTES_144_QP_STATE_S, IB_QPS_INIT);

		ret = hns_roce_v1_qp_modify(hr_dev, &hr_qp->mtt,
					    HNS_ROCE_QP_STATE_RST,
					    HNS_ROCE_QP_STATE_INIT,
					    context, hr_qp);
		if (ret) {
			dev_err(dev, "Hns_roce_qp_modify failed(%d)!\n", ret);
			goto create_lp_qp_failed;
		}

		hr_qp->state = IB_QPS_INIT;

		ret = modify_qp_init_to_rtr(&hr_qp->ibqp, &attr, attr_mask,
					    context);
		if (ret) {
			dev_err(dev, "Modify loop qp from init to rtr failed(%d)!\n",
				ret);
			goto create_lp_qp_failed;
		}

		roce_set_field(context->qpc_bytes_144,
			QP_CONTEXT_QPC_BYTES_144_QP_STATE_M,
			QP_CONTEXT_QPC_BYTES_144_QP_STATE_S, IB_QPS_RTR);

		ret = hns_roce_v1_qp_modify(hr_dev, &hr_qp->mtt,
					    HNS_ROCE_QP_STATE_INIT,
					    HNS_ROCE_QP_STATE_RTR,
					    context, hr_qp);
		if (ret) {
			dev_err(dev, "Hns_roce_qp_modify failed(%d)!\n", ret);
			goto create_lp_qp_failed;
		}
		hr_qp->state = IB_QPS_RTR;

		attr.qp_state = IB_QPS_RTS;
		ret = modify_qp_rtr_to_rts(&hr_qp->ibqp, &attr, attr_mask,
					   context);
		if (ret) {
			dev_err(dev, "Modify loop qp from rtr to rts failed(%d)!\n",
				ret);
			goto create_lp_qp_failed;
		}

		roce_set_field(context->qpc_bytes_144,
			QP_CONTEXT_QPC_BYTES_144_QP_STATE_M,
			QP_CONTEXT_QPC_BYTES_144_QP_STATE_S, IB_QPS_RTS);

		/* SW pass context to HW */
		ret = hns_roce_v1_qp_modify(hr_dev, &hr_qp->mtt,
					    HNS_ROCE_QP_STATE_RTR,
					    HNS_ROCE_QP_STATE_RTS,
					    context, hr_qp);
		if (ret) {
			dev_err(dev, "Hns_roce_qp_modify failed(%d)!\n", ret);
			goto create_lp_qp_failed;
		}
		hr_qp->state = IB_QPS_RTS;
	}

	kfree(context);

	return 0;

create_lp_qp_failed:
	for (j = 0; j < i; j++) {
		hr_qp = priv->mr_free_qp[j];
		if (hns_roce_v1_destroy_qp(&hr_qp->ibqp))
			dev_err(dev, "Destroy qp %d for mr free failed!\n", j);
	}

	kfree(context);
alloc_context_failed:
	if (hns_roce_dealloc_pd(pd))
		dev_err(dev, "Destroy pd for create_lp_qp failed!\n");

alloc_pd_failed:
	if (hns_roce_ib_destroy_cq(cq))
		dev_err(dev, "Destroy cq for create_lp_qp failed!\n");

	return ret;
}

static void hns_roce_v1_release_lp_qp(struct hns_roce_dev *hr_dev)
{
	struct device *dev = &hr_dev->pdev->dev;
	struct hns_roce_v1_priv *priv;
	struct hns_roce_qp *hr_qp;
	int ret;
	int i;

	priv = (struct hns_roce_v1_priv *)hr_dev->hw->priv;

	for (i = 0; i < HNS_ROCE_V1_RESV_QP; i++) {
		hr_qp = priv->mr_free_qp[i];
		ret = hns_roce_v1_destroy_qp(&hr_qp->ibqp);
		if (ret)
			dev_err(dev, "Destroy qp %d for mr free failed(%d)!\n",
				i, ret);
	}

	ret = hns_roce_ib_destroy_cq(&priv->mr_free_cq->ib_cq);
	if (ret)
		dev_err(dev, "Destroy cq for mr_free failed(%d)!\n", ret);

	ret = hns_roce_dealloc_pd(&priv->mr_free_pd->ibpd);
	if (ret)
		dev_err(dev, "Destroy pd for mr_free failed(%d)!\n", ret);
}

static inline void release_set_mac_work(struct kref *kref)
{
	struct hns_roce_set_mac_work *work_entry =
		container_of(kref, struct hns_roce_set_mac_work, ref);

	kfree(work_entry);
}

void hns_roce_v1_set_mac_work(struct work_struct *work)
{
	struct hns_roce_set_mac_work *work_entry;
	struct hns_roce_dev *hr_dev;

	work_entry = container_of(work, struct hns_roce_set_mac_work, work);
	hr_dev = to_hr_dev(work_entry->ib_dev);

	hns_roce_v1_release_lp_qp(hr_dev);

	if (hns_roce_v1_rsv_lp_qp(hr_dev)) {
		dev_err(&hr_dev->pdev->dev, "create reserver qp failed\n");
		work_entry->state = 1;
	}

	complete(&work_entry->comp);
	kref_put(&work_entry->ref, release_set_mac_work);
}

static int hns_roce_v1_set_mac_fh(struct hns_roce_dev *hr_dev)
{
	struct device *dev = &hr_dev->pdev->dev;
	struct hns_roce_set_mac_work *work_entry;
	struct hns_roce_v1_priv *priv;
	unsigned long end =
		msecs_to_jiffies(SET_MAC_TIMEOUT_MSECS) + jiffies;
	unsigned long start = jiffies;
	int ret = 0;

	priv = (struct hns_roce_v1_priv *)hr_dev->hw->priv;

	work_entry = kzalloc(sizeof(struct hns_roce_set_mac_work), GFP_KERNEL);
	if (!work_entry)
		return -ENOMEM;

	INIT_WORK(&(work_entry->work), hns_roce_v1_set_mac_work);

	work_entry->ib_dev = &(hr_dev->ib_dev);
	kref_init(&work_entry->ref);
	init_completion(&work_entry->comp);

	kref_get(&work_entry->ref);
	queue_work(priv->mr_free_queue, &(work_entry->work));

	while (time_before_eq(jiffies, end)) {
		if (try_wait_for_completion(&work_entry->comp)) {
			if (work_entry->state)
				ret = -EBUSY;
			goto free_work;
		}
		msleep(SET_MAC_WAIT_VALUE);
	}

	dev_err(dev, "set mac failed 20s timeout and return failed!\n");
	ret = -EBUSY;

free_work:
	dev_dbg(dev, "Set mac use 0x%x us.\n",
		jiffies_to_usecs(jiffies) - jiffies_to_usecs(start));

	kref_put(&work_entry->ref, release_set_mac_work);

	return ret;
}

static int hns_roce_v1_send_lp_wqe(struct hns_roce_qp *hr_qp)
{
	struct hns_roce_dev *hr_dev = to_hr_dev(hr_qp->ibqp.device);
	struct device *dev = &hr_dev->pdev->dev;
	struct ib_send_wr send_wr, *bad_wr;
	int ret;

	memset(&send_wr, 0, sizeof(send_wr));
	send_wr.next	= NULL;
	send_wr.num_sge	= 0;
	send_wr.send_flags = 0;
	send_wr.sg_list	= NULL;
	send_wr.wr_id	= (unsigned long long)&send_wr;
	send_wr.opcode	= IB_WR_RDMA_WRITE;

	ret = hns_roce_v1_post_send(&hr_qp->ibqp, &send_wr, &bad_wr);
	if (ret) {
		dev_err(dev, "Post write wqe for mr free failed(%d)!", ret);
		return ret;
	}

	return 0;
}

static inline void release_mr_work(struct kref *kref)
{
	struct hns_roce_mr_free_work *work_entry =
		container_of(kref, struct hns_roce_mr_free_work, ref);

	kfree(work_entry);
}

static void hns_roce_v1_mr_free_bh(struct work_struct *work)
{
	struct hns_roce_mr_free_work *work_entry;
	struct hns_roce_v1_priv *priv;
	struct hns_roce_dev *hr_dev;
	struct hns_roce_qp *hr_qp;
	struct ib_wc wc[HNS_ROCE_V1_RESV_QP];
	struct hns_roce_cq *mr_free_cq;
	struct device *dev;
	unsigned long end =
		msecs_to_jiffies(MR_FREE_TIMEOUT_MSECS) + jiffies;
	int i;
	int ret;
	int ne;

	work_entry = container_of(work, struct hns_roce_mr_free_work, work);
	hr_dev = to_hr_dev(work_entry->ib_dev);
	dev = &hr_dev->pdev->dev;

	priv = (struct hns_roce_v1_priv *)hr_dev->hw->priv;
	mr_free_cq = priv->mr_free_cq;

	for (i = 0; i < HNS_ROCE_V1_RESV_QP; i++) {
		hr_qp = priv->mr_free_qp[i];
		ret = hns_roce_v1_send_lp_wqe(hr_qp);
		if (ret) {
			dev_err(dev, "Send wqe (qp:0x%lx) for mr free failed(%d)!\n",
					hr_qp->qpn, ret);
			work_entry->state = 1;
			goto free_work;
		}
	}

	ne = HNS_ROCE_V1_RESV_QP;
	do {
		ret = hns_roce_v1_poll_cq(&mr_free_cq->ib_cq, ne, wc);
		if (ret < 0) {
			dev_err(dev, "(qp:0x%lx) starts, Poll cqe failed(%d) for mr 0x%x free! Remain %d cqe\n",
				hr_qp->qpn, ret, work_entry->lkey, ne);
			work_entry->state = 1;
			goto free_work;
		}
		ne -= ret;
		msleep(MR_FREE_WAIT_VALUE);
	} while (ne && time_before_eq(jiffies, end));

	if (ne != 0) {
		dev_err(dev, "Poll cqe for mr 0x%x free timeout! Remain %d cqe\n",
			 work_entry->lkey, ne);
		work_entry->state = 1;
	}

free_work:
	complete(&work_entry->comp);
	kref_put(&work_entry->ref, release_mr_work);
}

int hns_roce_v1_dereg_mr_bh(struct hns_roce_dev *hr_dev, struct hns_roce_mr *mr)
{
	struct device *dev = &hr_dev->pdev->dev;
	struct hns_roce_mr_free_work *work_entry;
	struct hns_roce_v1_priv *priv;
	unsigned long end =
		msecs_to_jiffies(MR_FREE_TIMEOUT_MSECS) + jiffies;
	unsigned long start = jiffies;
	int npages;
	int ret = 0;

	priv = (struct hns_roce_v1_priv *)hr_dev->hw->priv;

	if (mr->enabled) {
		ret = hns_roce_hw2sw_mpt(hr_dev, NULL, key_to_hw_index(mr->key)
					 & (hr_dev->caps.num_mtpts - 1));
		if (ret)
			dev_err(dev, "HW2SW_MPT failed(%d)!\n", ret);
	}

	mr->priv = kzalloc(sizeof(struct hns_roce_mr_free_work), GFP_KERNEL);
	if (!mr->priv)
		return -ENOMEM;

	work_entry = (struct hns_roce_mr_free_work *)mr->priv;

	INIT_WORK(&(work_entry->work), hns_roce_v1_mr_free_bh);

	work_entry->ib_dev = &(hr_dev->ib_dev);
	work_entry->lkey = mr->key;
	kref_init(&work_entry->ref);
	init_completion(&work_entry->comp);

	kref_get(&work_entry->ref);
	queue_work(priv->mr_free_queue, &(work_entry->work));

	while (time_before_eq(jiffies, end)) {
		if (try_wait_for_completion(&work_entry->comp)) {
			if (work_entry->state)
				ret = -EBUSY;
			goto free_mr;
		}
		msleep(MR_FREE_WAIT_VALUE);
	}

	dev_err(dev, "Free mr 0x%x over 50s and return failed!\n", mr->key);
	ret = -EBUSY;

free_mr:
	dev_dbg(dev, "Free mr 0x%x use 0x%x us.\n",
		mr->key, jiffies_to_usecs(jiffies) - jiffies_to_usecs(start));

	if (mr->size != ~0ULL) {
		npages = ib_umem_page_count(mr->umem);
		dma_free_coherent(dev, npages * 8, mr->pbl_buf,
				  mr->pbl_dma_addr);
	}

	hns_roce_bitmap_free(&hr_dev->mr_table.mtpt_bitmap,
			     key_to_hw_index(mr->key), 0);

	if (mr->umem)
		ib_umem_release(mr->umem);

	kref_put(&work_entry->ref, release_mr_work);

	kfree(mr);

	return ret;
}

int hns_roce_v1_init(struct hns_roce_dev *hr_dev)
{
	int ret;
	u32 val;
	int i;
	struct device *dev = &hr_dev->pdev->dev;
	struct hns_roce_v1_priv *priv;

	priv = (struct hns_roce_v1_priv *)hr_dev->hw->priv;

	/* DMAE user config */
	val = roce_read(hr_dev, ROCEE_DMAE_USER_CFG1_REG);
	roce_set_field(val, ROCEE_DMAE_USER_CFG1_ROCEE_CACHE_TB_CFG_M,
		       ROCEE_DMAE_USER_CFG1_ROCEE_CACHE_TB_CFG_S, 0xf);
	roce_set_field(val, ROCEE_DMAE_USER_CFG1_ROCEE_STREAM_ID_TB_CFG_M,
		       ROCEE_DMAE_USER_CFG1_ROCEE_STREAM_ID_TB_CFG_S,
		       1 << 16);
	roce_write(hr_dev, ROCEE_DMAE_USER_CFG1_REG, val);

	val = roce_read(hr_dev, ROCEE_DMAE_USER_CFG2_REG);
	roce_set_field(val, ROCEE_DMAE_USER_CFG2_ROCEE_CACHE_PKT_CFG_M,
		       ROCEE_DMAE_USER_CFG2_ROCEE_CACHE_PKT_CFG_S, 0xf);
	roce_set_field(val, ROCEE_DMAE_USER_CFG2_ROCEE_STREAM_ID_PKT_CFG_M,
		       ROCEE_DMAE_USER_CFG2_ROCEE_STREAM_ID_PKT_CFG_S,
		       1 << 16);

	ret = hns_roce_db_init(hr_dev);
	if (ret) {
		dev_err(dev, "doorbell init failed(%d)!\n", ret);
		return ret;
	}

	ret = hns_roce_raq_init(hr_dev);
	if (ret) {
		dev_err(dev, "raq init failed(%d)!\n", ret);
		goto error_failed_raq_init;
	}

	hns_roce_port_enable(hr_dev, HNS_ROCE_PORT_UP);

	priv->bt_rsv_buf_qpc = dma_alloc_coherent(dev, HNS_ROCE_BT_RSV_BUF_SIZE,
			&priv->bt_rsv_buf_dma_qpc, GFP_KERNEL);
	if (!priv->bt_rsv_buf_qpc) {
		dev_err(dev, "Alloc bt rsv buffer for qpc failed!\n");
		ret = -ENOMEM;
		goto err_failed_alloc_bt_buf_qpc;
	}

	priv->bt_rsv_buf_mtpt = dma_alloc_coherent(dev,
			HNS_ROCE_BT_RSV_BUF_SIZE,
			&priv->bt_rsv_buf_dma_mtpt, GFP_KERNEL);
	if (!priv->bt_rsv_buf_mtpt) {
		dev_err(dev, "Alloc bt rsv buffer for mtpt failed!\n");
		ret = -ENOMEM;
		goto err_failed_alloc_bt_buf_mtpt;
	}

	priv->bt_rsv_buf_cqc = dma_alloc_coherent(dev, HNS_ROCE_BT_RSV_BUF_SIZE,
			&priv->bt_rsv_buf_dma_cqc, GFP_KERNEL);
	if (!priv->bt_rsv_buf_cqc) {
		dev_err(dev, "Alloc bt rsv buffer for cqc failed!\n");
		ret = -ENOMEM;
		goto err_failed_alloc_bt_buf_cqc;
	}

	priv->requeue_flag = 1;
	priv->des_qp_queue =
		create_singlethread_workqueue("hns_roce_qp_destroy");
	if (!priv->des_qp_queue) {
		dev_err(dev, "Create destroy qp queue failed!\n");
		ret = -ENOMEM;
		goto err_create_des_qp_queue;
	}

	priv->mr_free_queue =
		create_singlethread_workqueue("hns_roce_mr_free");
	if (!priv->mr_free_queue) {
		dev_err(dev, "Create mr free queue failed!\n");
		ret = -ENOMEM;
		goto err_create_mr_free_queue;
	}

	ret = hns_roce_v1_rsv_lp_qp(hr_dev);
	if (ret) {
		dev_err(dev, "Reserved loop qp failed(%d)!", ret);
		goto err_rsv_lp_qp;
	}
	/* Clear mac addr of hr_dev, and these will init in register device */
	for (i = 0; i < HNS_ROCE_MAX_PORTS; i++)
		memset(hr_dev->dev_addr[i], 0, MAC_ADDR_OCTET_NUM);


	return 0;

err_rsv_lp_qp:
	flush_workqueue(priv->mr_free_queue);
	destroy_workqueue(priv->mr_free_queue);
err_create_mr_free_queue:
	priv->requeue_flag = 0;
	flush_workqueue(priv->des_qp_queue);
	destroy_workqueue(priv->des_qp_queue);
err_create_des_qp_queue:
	dma_free_coherent(dev, HNS_ROCE_BT_RSV_BUF_SIZE,
		priv->bt_rsv_buf_cqc, priv->bt_rsv_buf_dma_cqc);
	priv->bt_rsv_buf_cqc = NULL;
err_failed_alloc_bt_buf_cqc:
	dma_free_coherent(dev, HNS_ROCE_BT_RSV_BUF_SIZE,
		priv->bt_rsv_buf_mtpt, priv->bt_rsv_buf_dma_mtpt);
	priv->bt_rsv_buf_mtpt = NULL;
err_failed_alloc_bt_buf_mtpt:
	dma_free_coherent(dev, HNS_ROCE_BT_RSV_BUF_SIZE,
		priv->bt_rsv_buf_qpc, priv->bt_rsv_buf_dma_qpc);
	priv->bt_rsv_buf_qpc = NULL;
err_failed_alloc_bt_buf_qpc:
	hns_roce_port_enable(hr_dev, HNS_ROCE_PORT_DOWN);
	hns_roce_raq_free(hr_dev);
error_failed_raq_init:
	hns_roce_db_free(hr_dev);
	return ret;
}

void hns_roce_v1_exit(struct hns_roce_dev *hr_dev)
{
	struct device *dev = &hr_dev->pdev->dev;
	struct hns_roce_v1_priv *priv;

	priv = (struct hns_roce_v1_priv *)hr_dev->hw->priv;

	flush_workqueue(priv->mr_free_queue);
	destroy_workqueue(priv->mr_free_queue);

	hns_roce_v1_release_lp_qp(hr_dev);

	priv->requeue_flag = 0;
	flush_workqueue(priv->des_qp_queue);
	destroy_workqueue(priv->des_qp_queue);

	dma_free_coherent(dev, HNS_ROCE_BT_RSV_BUF_SIZE,
		priv->bt_rsv_buf_cqc, priv->bt_rsv_buf_dma_cqc);
	priv->bt_rsv_buf_cqc = NULL;
	dma_free_coherent(dev, HNS_ROCE_BT_RSV_BUF_SIZE,
		priv->bt_rsv_buf_mtpt, priv->bt_rsv_buf_dma_mtpt);
	priv->bt_rsv_buf_mtpt = NULL;
	dma_free_coherent(dev, HNS_ROCE_BT_RSV_BUF_SIZE,
		priv->bt_rsv_buf_qpc, priv->bt_rsv_buf_dma_qpc);
	priv->bt_rsv_buf_qpc = NULL;
	hns_roce_port_enable(hr_dev, HNS_ROCE_PORT_DOWN);
	hns_roce_raq_free(hr_dev);
	hns_roce_db_free(hr_dev);
}

struct hns_roce_v1_priv hr_v1_priv;

struct hns_roce_hw hns_roce_hw_v1 = {
	.reset = hns_roce_v1_reset,
	.hw_profile = hns_roce_v1_profile,
	.hw_init = hns_roce_v1_init,
	.hw_exit = hns_roce_v1_exit,
	.set_gid = hns_roce_v1_set_gid,
	.set_mac = hns_roce_v1_set_mac,
	.set_mtu = hns_roce_v1_set_mtu,
	.write_mtpt = hns_roce_v1_write_mtpt,
	.write_cqc = hns_roce_v1_write_cqc,
	.clear_hem = hns_roce_v1_clear_hem,
	.modify_qp = hns_roce_v1_modify_qp,
	.query_qp = hns_roce_v1_query_qp,
	.destroy_qp = hns_roce_v1_destroy_qp,
	.post_send = hns_roce_v1_post_send,
	.post_recv = hns_roce_v1_post_recv,
	.req_notify_cq = hns_roce_v1_req_notify_cq,
	.poll_cq = hns_roce_v1_poll_cq,
	.dereg_mr_bh = hns_roce_v1_dereg_mr_bh,
	.priv = &hr_v1_priv,
};
