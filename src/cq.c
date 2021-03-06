/*
 * Copyright (c) 2005 Topspin Communications.  All rights reserved.
 * Copyright (c) 2005 Mellanox Technologies Ltd.  All rights reserved.
 * Copyright (c) 2006, 2007 Cisco Systems.  All rights reserved.
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

#if HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <netinet/in.h>
#include <string.h>

#include <infiniband/opcode.h>

#include "mlx4.h"
#include "doorbell.h"

enum {
	MLX4_CQ_DOORBELL			= 0x20
};

enum {
	CQ_CONTINUE				=  1,
	CQ_OK					=  0,
	CQ_EMPTY				= -1,
	CQ_POLL_ERR				= -2
};

#define MLX4_CQ_DB_REQ_NOT_SOL			(1 << 24)
#define MLX4_CQ_DB_REQ_NOT			(2 << 24)

enum {
	MLX4_CQE_VLAN_PRESENT_MASK		= 1 << 29,
	MLX4_CQE_QPN_MASK			= 0xffffff,
};

enum {
	MLX4_CQE_OWNER_MASK			= 0x80,
	MLX4_CQE_IS_SEND_MASK			= 0x40,
	MLX4_CQE_OPCODE_MASK			= 0x1f
};

enum {
	MLX4_CQE_SYNDROME_LOCAL_LENGTH_ERR		= 0x01,
	MLX4_CQE_SYNDROME_LOCAL_QP_OP_ERR		= 0x02,
	MLX4_CQE_SYNDROME_LOCAL_PROT_ERR		= 0x04,
	MLX4_CQE_SYNDROME_WR_FLUSH_ERR			= 0x05,
	MLX4_CQE_SYNDROME_MW_BIND_ERR			= 0x06,
	MLX4_CQE_SYNDROME_BAD_RESP_ERR			= 0x10,
	MLX4_CQE_SYNDROME_LOCAL_ACCESS_ERR		= 0x11,
	MLX4_CQE_SYNDROME_REMOTE_INVAL_REQ_ERR		= 0x12,
	MLX4_CQE_SYNDROME_REMOTE_ACCESS_ERR		= 0x13,
	MLX4_CQE_SYNDROME_REMOTE_OP_ERR			= 0x14,
	MLX4_CQE_SYNDROME_TRANSPORT_RETRY_EXC_ERR	= 0x15,
	MLX4_CQE_SYNDROME_RNR_RETRY_EXC_ERR		= 0x16,
	MLX4_CQE_SYNDROME_REMOTE_ABORTED_ERR		= 0x22,
};

struct mlx4_err_cqe {
	uint32_t	vlan_my_qpn;
	uint32_t	reserved1[5];
	uint16_t	wqe_index;
	uint8_t		vendor_err;
	uint8_t		syndrome;
	uint8_t		reserved2[3];
	uint8_t		owner_sr_opcode;
};

static struct mlx4_cqe *get_cqe(struct mlx4_cq *cq, int entry)
{
	return cq->buf.buf + entry * cq->cqe_size;
}

static void *get_sw_cqe(struct mlx4_cq *cq, int n)
{
	struct mlx4_cqe *cqe = get_cqe(cq, n & cq->ibv_cq.cqe);
	struct mlx4_cqe *tcqe = cq->cqe_size == 64 ? cqe + 1 : cqe;

	return (!!(tcqe->owner_sr_opcode & MLX4_CQE_OWNER_MASK) ^
		!!(n & (cq->ibv_cq.cqe + 1))) ? NULL : cqe;
}

static struct mlx4_cqe *next_cqe_sw(struct mlx4_cq *cq)
{
	return get_sw_cqe(cq, cq->cons_index);
}

static void update_cons_index(struct mlx4_cq *cq)
{
	*cq->set_ci_db = htonl(cq->cons_index & 0xffffff);
}

static void mlx4_handle_error_cqe(struct mlx4_err_cqe *cqe,
				  enum ibv_wc_status *status,
				  enum ibv_wc_opcode *vendor_err)
{
	if (cqe->syndrome == MLX4_CQE_SYNDROME_LOCAL_QP_OP_ERR)
		printf(PFX "local QP operation err "
		       "(QPN %06x, WQE index %x, vendor syndrome %02x, "
		       "opcode = %02x)\n",
		       htonl(cqe->vlan_my_qpn), htonl(cqe->wqe_index),
		       cqe->vendor_err,
		       cqe->owner_sr_opcode & ~MLX4_CQE_OWNER_MASK);

	switch (cqe->syndrome) {
	case MLX4_CQE_SYNDROME_LOCAL_LENGTH_ERR:
		*status = IBV_WC_LOC_LEN_ERR;
		break;
	case MLX4_CQE_SYNDROME_LOCAL_QP_OP_ERR:
		*status = IBV_WC_LOC_QP_OP_ERR;
		break;
	case MLX4_CQE_SYNDROME_LOCAL_PROT_ERR:
		*status = IBV_WC_LOC_PROT_ERR;
		break;
	case MLX4_CQE_SYNDROME_WR_FLUSH_ERR:
		*status = IBV_WC_WR_FLUSH_ERR;
		break;
	case MLX4_CQE_SYNDROME_MW_BIND_ERR:
		*status = IBV_WC_MW_BIND_ERR;
		break;
	case MLX4_CQE_SYNDROME_BAD_RESP_ERR:
		*status = IBV_WC_BAD_RESP_ERR;
		break;
	case MLX4_CQE_SYNDROME_LOCAL_ACCESS_ERR:
		*status = IBV_WC_LOC_ACCESS_ERR;
		break;
	case MLX4_CQE_SYNDROME_REMOTE_INVAL_REQ_ERR:
		*status = IBV_WC_REM_INV_REQ_ERR;
		break;
	case MLX4_CQE_SYNDROME_REMOTE_ACCESS_ERR:
		*status = IBV_WC_REM_ACCESS_ERR;
		break;
	case MLX4_CQE_SYNDROME_REMOTE_OP_ERR:
		*status = IBV_WC_REM_OP_ERR;
		break;
	case MLX4_CQE_SYNDROME_TRANSPORT_RETRY_EXC_ERR:
		*status = IBV_WC_RETRY_EXC_ERR;
		break;
	case MLX4_CQE_SYNDROME_RNR_RETRY_EXC_ERR:
		*status = IBV_WC_RNR_RETRY_EXC_ERR;
		break;
	case MLX4_CQE_SYNDROME_REMOTE_ABORTED_ERR:
		*status = IBV_WC_REM_ABORT_ERR;
		break;
	default:
		*status = IBV_WC_GENERAL_ERR;
		break;
	}

	*vendor_err = cqe->vendor_err;
}

static inline int mlx4_handle_cq(struct mlx4_cq *cq,
				 struct mlx4_qp **cur_qp,
				 uint64_t *wc_wr_id,
				 enum ibv_wc_status *wc_status,
				 uint32_t *wc_vendor_err,
				 struct mlx4_cqe **pcqe,
				 uint32_t *pqpn,
				 int *pis_send)
{
	struct mlx4_wq *wq;
	struct mlx4_cqe *cqe;
	struct mlx4_srq *srq;
	uint32_t qpn;
	int is_error;
	int is_send;
	uint16_t wqe_index;

	cqe = next_cqe_sw(cq);
	if (!cqe)
		return CQ_EMPTY;

	if (cq->cqe_size == 64)
		++cqe;

	++cq->cons_index;

	VALGRIND_MAKE_MEM_DEFINED(cqe, sizeof(*cqe));

	/*
	 * Make sure we read CQ entry contents after we've checked the
	 * ownership bit.
	 */
	rmb();

	qpn = ntohl(cqe->vlan_my_qpn) & MLX4_CQE_QPN_MASK;

	is_send  = cqe->owner_sr_opcode & MLX4_CQE_IS_SEND_MASK;
	is_error = (cqe->owner_sr_opcode & MLX4_CQE_OPCODE_MASK) ==
		MLX4_CQE_OPCODE_ERROR;

	if ((qpn & MLX4_XRC_QPN_BIT) && !is_send) {
		/*
		 * We do not have to take the XSRQ table lock here,
		 * because CQs will be locked while SRQs are removed
		 * from the table.
		 */
		srq = mlx4_find_xsrq(&to_mctx(cq->ibv_cq.context)->xsrq_table,
				     ntohl(cqe->g_mlpath_rqpn) & MLX4_CQE_QPN_MASK);
		if (!srq)
			return CQ_POLL_ERR;
	} else {
		if (!*cur_qp || (qpn != (*cur_qp)->verbs_qp.qp.qp_num)) {
			/*
			 * We do not have to take the QP table lock here,
			 * because CQs will be locked while QPs are removed
			 * from the table.
			 */
			*cur_qp = mlx4_find_qp(to_mctx(cq->ibv_cq.context), qpn);
			if (!*cur_qp)
				return CQ_POLL_ERR;
		}
		srq = ((*cur_qp)->verbs_qp.qp.srq) ? to_msrq((*cur_qp)->verbs_qp.qp.srq) : NULL;
	}

	if (is_send) {
		wq = &(*cur_qp)->sq;
		wqe_index = ntohs(cqe->wqe_index);
		wq->tail += (uint16_t)(wqe_index - (uint16_t)wq->tail);
		*wc_wr_id = wq->wrid[wq->tail & (wq->wqe_cnt - 1)];
		++wq->tail;
	} else if (srq) {
		wqe_index = htons(cqe->wqe_index);
		*wc_wr_id = srq->wrid[wqe_index];
		mlx4_free_srq_wqe(srq, wqe_index);
	} else {
		wq = &(*cur_qp)->rq;
		*wc_wr_id = wq->wrid[wq->tail & (wq->wqe_cnt - 1)];
		++wq->tail;
	}

	if (is_error) {
		mlx4_handle_error_cqe((struct mlx4_err_cqe *)cqe,
				      wc_status, wc_vendor_err);
		return CQ_OK;
	}

	*wc_status = IBV_WC_SUCCESS;

	*pcqe = cqe;
	*pqpn = qpn;
	*pis_send = is_send;

	return CQ_CONTINUE;
}

static int mlx4_poll_one(struct mlx4_cq *cq,
			 struct mlx4_qp **cur_qp,
			 struct ibv_wc *wc)
{
	struct mlx4_cqe *cqe;
	uint32_t qpn;
	uint32_t g_mlpath_rqpn;
	int is_send;
	int err;

	err = mlx4_handle_cq(cq, cur_qp, &wc->wr_id, &wc->status,
			     &wc->vendor_err, &cqe, &qpn, &is_send);
	if (err != CQ_CONTINUE)
		return err;

	wc->qp_num = qpn;
	if (is_send) {
		wc->wc_flags = 0;
		switch (cqe->owner_sr_opcode & MLX4_CQE_OPCODE_MASK) {
		case MLX4_OPCODE_RDMA_WRITE_IMM:
			wc->wc_flags |= IBV_WC_WITH_IMM;
		case MLX4_OPCODE_RDMA_WRITE:
			wc->opcode    = IBV_WC_RDMA_WRITE;
			break;
		case MLX4_OPCODE_SEND_IMM:
			wc->wc_flags |= IBV_WC_WITH_IMM;
		case MLX4_OPCODE_SEND:
			wc->opcode    = IBV_WC_SEND;
			break;
		case MLX4_OPCODE_RDMA_READ:
			wc->opcode    = IBV_WC_RDMA_READ;
			wc->byte_len  = ntohl(cqe->byte_cnt);
			break;
		case MLX4_OPCODE_ATOMIC_CS:
			wc->opcode    = IBV_WC_COMP_SWAP;
			wc->byte_len  = 8;
			break;
		case MLX4_OPCODE_ATOMIC_FA:
			wc->opcode    = IBV_WC_FETCH_ADD;
			wc->byte_len  = 8;
			break;
		case MLX4_OPCODE_BIND_MW:
			wc->opcode    = IBV_WC_BIND_MW;
			break;
		default:
			/* assume it's a send completion */
			wc->opcode    = IBV_WC_SEND;
			break;
		}
	} else {
		wc->byte_len = ntohl(cqe->byte_cnt);

		switch (cqe->owner_sr_opcode & MLX4_CQE_OPCODE_MASK) {
		case MLX4_RECV_OPCODE_RDMA_WRITE_IMM:
			wc->opcode   = IBV_WC_RECV_RDMA_WITH_IMM;
			wc->wc_flags = IBV_WC_WITH_IMM;
			wc->imm_data = cqe->immed_rss_invalid;
			break;
		case MLX4_RECV_OPCODE_SEND:
			wc->opcode   = IBV_WC_RECV;
			wc->wc_flags = 0;
			break;
		case MLX4_RECV_OPCODE_SEND_IMM:
			wc->opcode   = IBV_WC_RECV;
			wc->wc_flags = IBV_WC_WITH_IMM;
			wc->imm_data = cqe->immed_rss_invalid;
			break;
		}

		wc->slid	   = ntohs(cqe->rlid);
		g_mlpath_rqpn	   = ntohl(cqe->g_mlpath_rqpn);
		wc->src_qp	   = g_mlpath_rqpn & 0xffffff;
		wc->dlid_path_bits = (g_mlpath_rqpn >> 24) & 0x7f;
		wc->wc_flags	  |= g_mlpath_rqpn & 0x80000000 ? IBV_WC_GRH : 0;
		wc->pkey_index     = ntohl(cqe->immed_rss_invalid) & 0x7f;
		/* When working with xrc srqs, don't have qp to check link layer.
		  * Using IB SL, should consider Roce. (TBD)
		*/
		if ((*cur_qp) && (*cur_qp)->link_layer == IBV_LINK_LAYER_ETHERNET)
			wc->sl	   = ntohs(cqe->sl_vid) >> 13;
		else
			wc->sl	   = ntohs(cqe->sl_vid) >> 12;

		if ((*cur_qp) && ((*cur_qp)->qp_cap_cache & MLX4_RX_CSUM_VALID)) {
			wc->wc_flags |= ((cqe->status & htonl(MLX4_CQE_STATUS_IPV4_CSUM_OK)) ==
					 htonl(MLX4_CQE_STATUS_IPV4_CSUM_OK)) <<
					IBV_WC_IP_CSUM_OK_SHIFT;
		}
	}

	return CQ_OK;
}

union wc_buffer {
	uint8_t		*b8;
	uint16_t	*b16;
	uint32_t	*b32;
	uint64_t	*b64;
};

#define IS_IN_WC_FLAGS(yes, no, maybe, flag) (((yes) & (flag)) ||    \
					      (!((no) & (flag)) && \
					       ((maybe) & (flag))))
static inline int _mlx4_poll_one_ex(struct mlx4_cq *cq,
				    struct mlx4_qp **cur_qp,
				    struct ibv_wc_ex **pwc_ex,
				    uint64_t wc_flags,
				    uint64_t yes_wc_flags,
				    uint64_t no_wc_flags)
	ALWAYS_INLINE;
static inline int _mlx4_poll_one_ex(struct mlx4_cq *cq,
				    struct mlx4_qp **cur_qp,
				    struct ibv_wc_ex **pwc_ex,
				    uint64_t wc_flags,
				    uint64_t wc_flags_yes,
				    uint64_t wc_flags_no)
{
	struct mlx4_cqe *cqe;
	uint32_t qpn;
	uint32_t g_mlpath_rqpn;
	int is_send;
	struct ibv_wc_ex *wc_ex = *pwc_ex;
	union wc_buffer wc_buffer;
	int err;
	uint64_t wc_flags_out = 0;

	wc_buffer.b64 = (uint64_t *)&wc_ex->buffer;
	wc_ex->reserved = 0;
	err = mlx4_handle_cq(cq, cur_qp, &wc_ex->wr_id, &wc_ex->status,
			     &wc_ex->vendor_err, &cqe, &qpn, &is_send);
	if (err != CQ_CONTINUE)
		return err;

	if (IS_IN_WC_FLAGS(wc_flags_yes, wc_flags_no, wc_flags,
			   IBV_WC_EX_WITH_COMPLETION_TIMESTAMP)) {
		uint16_t timestamp_0_15 = cqe->timestamp_0_7 |
			cqe->timestamp_8_15 << 8;

		wc_flags_out |= IBV_WC_EX_WITH_COMPLETION_TIMESTAMP;
		*wc_buffer.b64++ = (((uint64_t)ntohl(cqe->timestamp_16_47)
					     + !timestamp_0_15) << 16) |
					   (uint64_t)timestamp_0_15;
	}

	if (is_send) {
		switch (cqe->owner_sr_opcode & MLX4_CQE_OPCODE_MASK) {
		case MLX4_OPCODE_RDMA_WRITE_IMM:
			wc_flags_out |= IBV_WC_EX_IMM;
		case MLX4_OPCODE_RDMA_WRITE:
			wc_ex->opcode    = IBV_WC_RDMA_WRITE;
			if (IS_IN_WC_FLAGS(wc_flags_yes, wc_flags_no, wc_flags,
					   IBV_WC_EX_WITH_BYTE_LEN))
				wc_buffer.b32++;
			if (IS_IN_WC_FLAGS(wc_flags_yes, wc_flags_no, wc_flags,
					   IBV_WC_EX_WITH_IMM))
				wc_buffer.b32++;
			break;
		case MLX4_OPCODE_SEND_IMM:
			wc_flags_out |= IBV_WC_EX_IMM;
		case MLX4_OPCODE_SEND:
			wc_ex->opcode    = IBV_WC_SEND;
			if (IS_IN_WC_FLAGS(wc_flags_yes, wc_flags_no, wc_flags,
					   IBV_WC_EX_WITH_BYTE_LEN))
				wc_buffer.b32++;
			if (IS_IN_WC_FLAGS(wc_flags_yes, wc_flags_no, wc_flags,
					   IBV_WC_EX_WITH_IMM))
				wc_buffer.b32++;
			break;
		case MLX4_OPCODE_RDMA_READ:
			wc_ex->opcode    = IBV_WC_RDMA_READ;
			if (IS_IN_WC_FLAGS(wc_flags_yes, wc_flags_no, wc_flags,
					   IBV_WC_EX_WITH_BYTE_LEN)) {
				*wc_buffer.b32++  = ntohl(cqe->byte_cnt);
				wc_flags_out |= IBV_WC_EX_WITH_BYTE_LEN;
			}
			if (IS_IN_WC_FLAGS(wc_flags_yes, wc_flags_no, wc_flags,
					   IBV_WC_EX_WITH_IMM))
				wc_buffer.b32++;
			break;
		case MLX4_OPCODE_ATOMIC_CS:
			wc_ex->opcode    = IBV_WC_COMP_SWAP;
			if (IS_IN_WC_FLAGS(wc_flags_yes, wc_flags_no, wc_flags,
					   IBV_WC_EX_WITH_BYTE_LEN)) {
				*wc_buffer.b32++  = 8;
				wc_flags_out |= IBV_WC_EX_WITH_BYTE_LEN;
			}
			if (IS_IN_WC_FLAGS(wc_flags_yes, wc_flags_no, wc_flags,
					   IBV_WC_EX_WITH_IMM))
				wc_buffer.b32++;
			break;
		case MLX4_OPCODE_ATOMIC_FA:
			wc_ex->opcode    = IBV_WC_FETCH_ADD;
			if (IS_IN_WC_FLAGS(wc_flags_yes, wc_flags_no, wc_flags,
					   IBV_WC_EX_WITH_BYTE_LEN)) {
				*wc_buffer.b32++  = 8;
				wc_flags_out |= IBV_WC_EX_WITH_BYTE_LEN;
			}
			if (IS_IN_WC_FLAGS(wc_flags_yes, wc_flags_no, wc_flags,
					   IBV_WC_EX_WITH_IMM))
				wc_buffer.b32++;
			break;
		case MLX4_OPCODE_BIND_MW:
			wc_ex->opcode    = IBV_WC_BIND_MW;
			if (IS_IN_WC_FLAGS(wc_flags_yes, wc_flags_no, wc_flags,
					   IBV_WC_EX_WITH_BYTE_LEN))
				wc_buffer.b32++;
			if (IS_IN_WC_FLAGS(wc_flags_yes, wc_flags_no, wc_flags,
					   IBV_WC_EX_WITH_IMM))
				wc_buffer.b32++;
			break;
		default:
			/* assume it's a send completion */
			wc_ex->opcode    = IBV_WC_SEND;
			if (IS_IN_WC_FLAGS(wc_flags_yes, wc_flags_no, wc_flags,
					   IBV_WC_EX_WITH_BYTE_LEN))
				wc_buffer.b32++;
			if (IS_IN_WC_FLAGS(wc_flags_yes, wc_flags_no, wc_flags,
					   IBV_WC_EX_WITH_IMM))
				wc_buffer.b32++;
			break;
		}

		if (IS_IN_WC_FLAGS(wc_flags_yes, wc_flags_no, wc_flags,
				   IBV_WC_EX_WITH_QP_NUM)) {
			*wc_buffer.b32++  = qpn;
			wc_flags_out |= IBV_WC_EX_WITH_QP_NUM;
		}
		if (IS_IN_WC_FLAGS(wc_flags_yes, wc_flags_no, wc_flags,
				   IBV_WC_EX_WITH_SRC_QP))
			wc_buffer.b32++;
		if (IS_IN_WC_FLAGS(wc_flags_yes, wc_flags_no, wc_flags,
				   IBV_WC_EX_WITH_PKEY_INDEX))
			wc_buffer.b16++;
		if (IS_IN_WC_FLAGS(wc_flags_yes, wc_flags_no, wc_flags,
				   IBV_WC_EX_WITH_SLID))
			wc_buffer.b16++;
		if (IS_IN_WC_FLAGS(wc_flags_yes, wc_flags_no, wc_flags,
				   IBV_WC_EX_WITH_SL))
			wc_buffer.b8++;
		if (IS_IN_WC_FLAGS(wc_flags_yes, wc_flags_no, wc_flags,
				   IBV_WC_EX_WITH_DLID_PATH_BITS))
			wc_buffer.b8++;
	} else {
		if (IS_IN_WC_FLAGS(wc_flags_yes, wc_flags_no, wc_flags,
				   IBV_WC_EX_WITH_BYTE_LEN)) {
			*wc_buffer.b32++ = ntohl(cqe->byte_cnt);
			wc_flags_out |= IBV_WC_EX_WITH_BYTE_LEN;
		}

		switch (cqe->owner_sr_opcode & MLX4_CQE_OPCODE_MASK) {
		case MLX4_RECV_OPCODE_RDMA_WRITE_IMM:
			wc_ex->opcode   = IBV_WC_RECV_RDMA_WITH_IMM;
			wc_flags_out |= IBV_WC_EX_IMM;
			if (IS_IN_WC_FLAGS(wc_flags_yes, wc_flags_no, wc_flags,
					   IBV_WC_EX_WITH_IMM)) {
				*wc_buffer.b32++ = cqe->immed_rss_invalid;
				wc_flags_out |= IBV_WC_EX_WITH_IMM;
			}
			break;
		case MLX4_RECV_OPCODE_SEND:
			wc_ex->opcode   = IBV_WC_RECV;
			if (IS_IN_WC_FLAGS(wc_flags_yes, wc_flags_no, wc_flags,
					   IBV_WC_EX_WITH_IMM))
				wc_buffer.b32++;
			break;
		case MLX4_RECV_OPCODE_SEND_IMM:
			wc_ex->opcode   = IBV_WC_RECV;
			wc_flags_out |= IBV_WC_EX_IMM;
			if (IS_IN_WC_FLAGS(wc_flags_yes, wc_flags_no, wc_flags,
					   IBV_WC_EX_WITH_IMM)) {
				*wc_buffer.b32++ = cqe->immed_rss_invalid;
				wc_flags_out |= IBV_WC_EX_WITH_IMM;
			}
			break;
		}

		if (IS_IN_WC_FLAGS(wc_flags_yes, wc_flags_no, wc_flags,
				   IBV_WC_EX_WITH_QP_NUM)) {
			*wc_buffer.b32++  = qpn;
			wc_flags_out |= IBV_WC_EX_WITH_QP_NUM;
		}
		g_mlpath_rqpn	   = ntohl(cqe->g_mlpath_rqpn);
		if (IS_IN_WC_FLAGS(wc_flags_yes, wc_flags_no, wc_flags,
				   IBV_WC_EX_WITH_SRC_QP)) {
			*wc_buffer.b32++  = g_mlpath_rqpn & 0xffffff;
			wc_flags_out |= IBV_WC_EX_WITH_SRC_QP;
		}
		if (IS_IN_WC_FLAGS(wc_flags_yes, wc_flags_no, wc_flags,
				   IBV_WC_EX_WITH_PKEY_INDEX)) {
			*wc_buffer.b16++  = ntohl(cqe->immed_rss_invalid) & 0x7f;
			wc_flags_out |= IBV_WC_EX_WITH_PKEY_INDEX;
		}
		if (IS_IN_WC_FLAGS(wc_flags_yes, wc_flags_no, wc_flags,
				   IBV_WC_EX_WITH_SLID)) {
			*wc_buffer.b16++  = ntohs(cqe->rlid);
			wc_flags_out |= IBV_WC_EX_WITH_SLID;
		}
		if (IS_IN_WC_FLAGS(wc_flags_yes, wc_flags_no, wc_flags,
				   IBV_WC_EX_WITH_SL)) {
			wc_flags_out |= IBV_WC_EX_WITH_SL;
			if ((*cur_qp) && (*cur_qp)->link_layer == IBV_LINK_LAYER_ETHERNET)
				*wc_buffer.b8++  = ntohs(cqe->sl_vid) >> 13;
			else
				*wc_buffer.b8++  = ntohs(cqe->sl_vid) >> 12;
		}
		if (IS_IN_WC_FLAGS(wc_flags_yes, wc_flags_no, wc_flags,
				   IBV_WC_EX_WITH_DLID_PATH_BITS)) {
			*wc_buffer.b8++  = (g_mlpath_rqpn >> 24) & 0x7f;
			wc_flags_out |= IBV_WC_EX_WITH_DLID_PATH_BITS;
		}
		wc_flags_out |= g_mlpath_rqpn & 0x80000000 ? IBV_WC_EX_GRH : 0;
		/* When working with xrc srqs, don't have qp to check link layer.
		  * Using IB SL, should consider Roce. (TBD)
		*/
	}

	wc_ex->wc_flags = wc_flags_out;
	/* Align the WC ex to the next 64bit. This is mandatory as ibv_wc_ex is
	 * 64bit aligned. pwc_ex is used to write to the next wc and thus we
	 * need to align it.
	 */
	*pwc_ex = (struct ibv_wc_ex *)((uintptr_t)(wc_buffer.b8 + sizeof(uint64_t) - 1) &
				       ~(sizeof(uint64_t) - 1));

	return CQ_OK;
}

int mlx4_poll_one_ex(struct mlx4_cq *cq,
		     struct mlx4_qp **cur_qp,
		     struct ibv_wc_ex **pwc_ex,
		     uint64_t wc_flags)
{
	return _mlx4_poll_one_ex(cq, cur_qp, pwc_ex, wc_flags, 0, 0);
}

#define MLX4_POLL_ONE_EX_WC_FLAGS_NAME(wc_flags_yes, wc_flags_no) \
	mlx4_poll_one_ex_custom##wc_flags_yes ## _ ## wc_flags_no

/* The compiler will create one function per wc_flags combination. Since
 * _mlx4_poll_one_ex  is always inlined (for compilers that supports that),
 * the compiler drops the if statements and merge all wc_flags_out ORs/ANDs.
 */
#define MLX4_POLL_ONE_EX_WC_FLAGS(wc_flags_yes, wc_flags_no)	\
static int MLX4_POLL_ONE_EX_WC_FLAGS_NAME(wc_flags_yes, wc_flags_no)	       \
						   (struct mlx4_cq *cq,        \
						    struct mlx4_qp **cur_qp,   \
						    struct ibv_wc_ex **pwc_ex, \
						    uint64_t wc_flags)	       \
{									       \
	return _mlx4_poll_one_ex(cq, cur_qp, pwc_ex, wc_flags,		       \
				 wc_flags_yes, wc_flags_no);		       \
}

/*
 *	Since we use the preprocessor here, we have to calculate the Or value
 *	ourselves:
 *	IBV_WC_EX_GRH			= 1 << 0,
 *	IBV_WC_EX_IMM			= 1 << 1,
 *	IBV_WC_EX_WITH_BYTE_LEN		= 1 << 2,
 *	IBV_WC_EX_WITH_IMM		= 1 << 3,
 *	IBV_WC_EX_WITH_QP_NUM		= 1 << 4,
 *	IBV_WC_EX_WITH_SRC_QP		= 1 << 5,
 *	IBV_WC_EX_WITH_PKEY_INDEX	= 1 << 6,
 *	IBV_WC_EX_WITH_SLID		= 1 << 7,
 *	IBV_WC_EX_WITH_SL		= 1 << 8,
 *	IBV_WC_EX_WITH_DLID_PATH_BITS	= 1 << 9,
 *	IBV_WC_EX_WITH_COMPLETION_TIMESTAMP = 1 << 10,
 */

/* Bitwise or of all flags between IBV_WC_EX_WITH_BYTE_LEN and
 * IBV_WC_EX_WITH_COMPLETION_TIMESTAMP.
 */
#define SUPPORTED_WC_ALL_FLAGS	2045
/* Bitwise or of all flags between IBV_WC_EX_WITH_BYTE_LEN and
 * IBV_WC_EX_WITH_DLID_PATH_BITS (all the fields that are available
 * in the legacy WC).
 */
#define SUPPORTED_WC_STD_FLAGS  1020

#define OPTIMIZE_POLL_CQ	/* No options */			    \
				OP(0, SUPPORTED_WC_ALL_FLAGS)		SEP \
				/* All options */			    \
				OP(SUPPORTED_WC_ALL_FLAGS, 0)		SEP \
				/* All standard options */		    \
				OP(SUPPORTED_WC_STD_FLAGS, 1024)	SEP \
				/* Just Bytelen - for DPDK */		    \
				OP(4, 1016)				SEP \
				/* Timestmap only, for FSI */		    \
				OP(1024, 1020)				SEP

#define OP	MLX4_POLL_ONE_EX_WC_FLAGS
#define SEP	;

/* Declare optimized poll_one function for popular scenarios. Each function
 * has a name of
 * mlx4_poll_one_ex_custom<supported_wc_flags>_<not_supported_wc_flags>.
 * Since the supported and not supported wc_flags are given beforehand,
 * the compiler could optimize the if and or statements and create optimized
 * code.
 */
OPTIMIZE_POLL_CQ

#define ADD_POLL_ONE(_wc_flags_yes, _wc_flags_no)			\
				{.wc_flags_yes = _wc_flags_yes,		\
				 .wc_flags_no = _wc_flags_no,		\
				 .fn = MLX4_POLL_ONE_EX_WC_FLAGS_NAME(  \
					_wc_flags_yes, _wc_flags_no)	\
				}

#undef OP
#undef SEP
#define OP	ADD_POLL_ONE
#define SEP	,

struct {
	int (*fn)(struct mlx4_cq *cq,
		  struct mlx4_qp **cur_qp,
		  struct ibv_wc_ex **pwc_ex,
		  uint64_t wc_flags);
	uint64_t wc_flags_yes;
	uint64_t wc_flags_no;
} mlx4_poll_one_ex_fns[] = {
	/* This array contains all the custom poll_one functions. Every entry
	 * in this array looks like:
	 * {.wc_flags_yes = <flags that are always in the wc>,
	 *  .wc_flags_no = <flags that are never in the wc>,
	 *  .fn = <the custom poll one function}.
	 * The .fn function is optimized according to the .wc_flags_yes and
	 * .wc_flags_no flags. Other flags have the "if statement".
	 */
	OPTIMIZE_POLL_CQ
};

/* This function gets wc_flags as an argument and returns a function pointer
 * of type int (*func)(struct mlx4_cq *cq, struct mlx4_qp **cur_qp,
 *		       struct ibv_wc_ex **pwc_ex, uint64_t wc_flags).
 * The returned function is one of the custom poll one functions declared in
 * mlx4_poll_one_ex_fns. The function is chosen as the function which the
 * number of wc_flags_maybe bits (the fields that aren't in the yes/no parts)
 * is the smallest.
 */
int (*mlx4_get_poll_one_fn(uint64_t wc_flags))(struct mlx4_cq *cq,
					       struct mlx4_qp **cur_qp,
					       struct ibv_wc_ex **pwc_ex,
					       uint64_t wc_flags)
{
	unsigned int i = 0;
	uint8_t min_bits = -1;
	int min_index = 0xff;

	for (i = 0;
	     i < sizeof(mlx4_poll_one_ex_fns) / sizeof(mlx4_poll_one_ex_fns[0]);
	     i++) {
		uint64_t bits;
		uint8_t nbits;

		/* Can't have required flags in "no" */
		if (wc_flags & mlx4_poll_one_ex_fns[i].wc_flags_no)
			continue;

		/* Can't have not required flags in yes */
		if (~wc_flags & mlx4_poll_one_ex_fns[i].wc_flags_yes)
			continue;

		/* Number of wc_flags_maybe. See above comment for more details */
		bits = (wc_flags  & ~mlx4_poll_one_ex_fns[i].wc_flags_yes) |
		       (~wc_flags & ~mlx4_poll_one_ex_fns[i].wc_flags_no &
			CREATE_CQ_SUPPORTED_WC_FLAGS);

		nbits = ibv_popcount64(bits);

		/* Look for the minimum number of bits */
		if (nbits < min_bits) {
			min_bits = nbits;
			min_index = i;
		}
	}

	if (min_index >= 0)
		return mlx4_poll_one_ex_fns[min_index].fn;

	return NULL;
}

int mlx4_poll_cq(struct ibv_cq *ibcq, int ne, struct ibv_wc *wc)
{
	struct mlx4_cq *cq = to_mcq(ibcq);
	struct mlx4_qp *qp = NULL;
	int npolled;
	int err = CQ_OK;

	pthread_spin_lock(&cq->lock);

	for (npolled = 0; npolled < ne; ++npolled) {
		err = mlx4_poll_one(cq, &qp, wc + npolled);
		if (err != CQ_OK)
			break;
	}

	if (npolled || err == CQ_POLL_ERR)
		update_cons_index(cq);

	pthread_spin_unlock(&cq->lock);

	return err == CQ_POLL_ERR ? err : npolled;
}

int mlx4_poll_cq_ex(struct ibv_cq *ibcq,
		    struct ibv_wc_ex *wc,
		    struct ibv_poll_cq_ex_attr *attr)
{
	struct mlx4_cq *cq = to_mcq(ibcq);
	struct mlx4_qp *qp = NULL;
	int npolled;
	int err = CQ_OK;
	unsigned int ne = attr->max_entries;
	int (*poll_fn)(struct mlx4_cq *cq, struct mlx4_qp **cur_qp,
		       struct ibv_wc_ex **wc_ex, uint64_t wc_flags) =
		cq->mlx4_poll_one;
	uint64_t wc_flags = cq->wc_flags;

	if (attr->comp_mask)
		return -EINVAL;

	pthread_spin_lock(&cq->lock);

	for (npolled = 0; npolled < ne; ++npolled) {
		err = poll_fn(cq, &qp, &wc, wc_flags);
		if (err != CQ_OK)
			break;
	}

	if (npolled || err == CQ_POLL_ERR)
		update_cons_index(cq);

	pthread_spin_unlock(&cq->lock);

	return err == CQ_POLL_ERR ? err : npolled;
}

int mlx4_arm_cq(struct ibv_cq *ibvcq, int solicited)
{
	struct mlx4_cq *cq = to_mcq(ibvcq);
	uint32_t doorbell[2];
	uint32_t sn;
	uint32_t ci;
	uint32_t cmd;

	sn  = cq->arm_sn & 3;
	ci  = cq->cons_index & 0xffffff;
	cmd = solicited ? MLX4_CQ_DB_REQ_NOT_SOL : MLX4_CQ_DB_REQ_NOT;

	*cq->arm_db = htonl(sn << 28 | cmd | ci);

	/*
	 * Make sure that the doorbell record in host memory is
	 * written before ringing the doorbell via PCI MMIO.
	 */
	wmb();

	doorbell[0] = htonl(sn << 28 | cmd | cq->cqn);
	doorbell[1] = htonl(ci);

	mlx4_write64(doorbell, to_mctx(ibvcq->context), MLX4_CQ_DOORBELL);

	return 0;
}

void mlx4_cq_event(struct ibv_cq *cq)
{
	to_mcq(cq)->arm_sn++;
}

void __mlx4_cq_clean(struct mlx4_cq *cq, uint32_t qpn, struct mlx4_srq *srq)
{
	struct mlx4_cqe *cqe, *dest;
	uint32_t prod_index;
	uint8_t owner_bit;
	int nfreed = 0;
	int cqe_inc = cq->cqe_size == 64 ? 1 : 0;

	/*
	 * First we need to find the current producer index, so we
	 * know where to start cleaning from.  It doesn't matter if HW
	 * adds new entries after this loop -- the QP we're worried
	 * about is already in RESET, so the new entries won't come
	 * from our QP and therefore don't need to be checked.
	 */
	for (prod_index = cq->cons_index; get_sw_cqe(cq, prod_index); ++prod_index)
		if (prod_index == cq->cons_index + cq->ibv_cq.cqe)
			break;

	/*
	 * Now sweep backwards through the CQ, removing CQ entries
	 * that match our QP by copying older entries on top of them.
	 */
	while ((int) --prod_index - (int) cq->cons_index >= 0) {
		cqe = get_cqe(cq, prod_index & cq->ibv_cq.cqe);
		cqe += cqe_inc;
		if (srq && srq->ext_srq &&
		    ntohl(cqe->g_mlpath_rqpn & MLX4_CQE_QPN_MASK) == srq->verbs_srq.srq_num &&
		    !(cqe->owner_sr_opcode & MLX4_CQE_IS_SEND_MASK)) {
			mlx4_free_srq_wqe(srq, ntohs(cqe->wqe_index));
			++nfreed;
		} else if ((ntohl(cqe->vlan_my_qpn) & MLX4_CQE_QPN_MASK) == qpn) {
			if (srq && !(cqe->owner_sr_opcode & MLX4_CQE_IS_SEND_MASK))
				mlx4_free_srq_wqe(srq, ntohs(cqe->wqe_index));
			++nfreed;
		} else if (nfreed) {
			dest = get_cqe(cq, (prod_index + nfreed) & cq->ibv_cq.cqe);
			dest += cqe_inc;
			owner_bit = dest->owner_sr_opcode & MLX4_CQE_OWNER_MASK;
			memcpy(dest, cqe, sizeof *cqe);
			dest->owner_sr_opcode = owner_bit |
				(dest->owner_sr_opcode & ~MLX4_CQE_OWNER_MASK);
		}
	}

	if (nfreed) {
		cq->cons_index += nfreed;
		/*
		 * Make sure update of buffer contents is done before
		 * updating consumer index.
		 */
		wmb();
		update_cons_index(cq);
	}
}

void mlx4_cq_clean(struct mlx4_cq *cq, uint32_t qpn, struct mlx4_srq *srq)
{
	pthread_spin_lock(&cq->lock);
	__mlx4_cq_clean(cq, qpn, srq);
	pthread_spin_unlock(&cq->lock);
}

int mlx4_get_outstanding_cqes(struct mlx4_cq *cq)
{
	uint32_t i;

	for (i = cq->cons_index; get_sw_cqe(cq, i); ++i)
		;

	return i - cq->cons_index;
}

void mlx4_cq_resize_copy_cqes(struct mlx4_cq *cq, void *buf, int old_cqe)
{
	struct mlx4_cqe *cqe;
	int i;
	int cqe_inc = cq->cqe_size == 64 ? 1 : 0;

	i = cq->cons_index;
	cqe = get_cqe(cq, (i & old_cqe));
	cqe += cqe_inc;

	while ((cqe->owner_sr_opcode & MLX4_CQE_OPCODE_MASK) != MLX4_CQE_OPCODE_RESIZE) {
		cqe->owner_sr_opcode = (cqe->owner_sr_opcode & ~MLX4_CQE_OWNER_MASK) |
			(((i + 1) & (cq->ibv_cq.cqe + 1)) ? MLX4_CQE_OWNER_MASK : 0);
		memcpy(buf + ((i + 1) & cq->ibv_cq.cqe) * cq->cqe_size,
		       cqe - cqe_inc, cq->cqe_size);
		++i;
		cqe = get_cqe(cq, (i & old_cqe));
		cqe += cqe_inc;
	}

	++cq->cons_index;
}

int mlx4_alloc_cq_buf(struct mlx4_device *dev, struct mlx4_buf *buf, int nent,
		      int entry_size)
{
	if (mlx4_alloc_buf(buf, align(nent * entry_size, dev->page_size),
			   dev->page_size))
		return -1;
	memset(buf->buf, 0, nent * entry_size);

	return 0;
}
