/*
 * Copyright (c) 2007 Cisco, Inc.  All rights reserved.
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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <netinet/in.h>

#include "mlx4.h"
#include "mlx4-abi.h"
#include "wqe.h"

static void parse_raw_fw_ver(uint64_t raw_fw_ver, unsigned *major,
			     unsigned *minor, unsigned *sub_minor)
{
	*major     = (raw_fw_ver >> 32) & 0xffff;
	*minor     = (raw_fw_ver >> 16) & 0xffff;
	*sub_minor = raw_fw_ver & 0xffff;
}

int mlx4_query_device(struct ibv_context *context, struct ibv_device_attr *attr)
{
	struct ibv_query_device cmd;
	uint64_t raw_fw_ver;
	unsigned major, minor, sub_minor;
	int ret;

	ret = ibv_cmd_query_device(context, attr, &raw_fw_ver, &cmd, sizeof cmd);
	if (ret)
		return ret;

	parse_raw_fw_ver(raw_fw_ver, &major, &minor, &sub_minor);

	snprintf(attr->fw_ver, sizeof attr->fw_ver,
		 "%d.%d.%03d", major, minor, sub_minor);

	return 0;
}

int _mlx4_query_device_ex(struct ibv_context *context,
			  const struct ibv_query_device_ex_input *input,
			  struct ibv_device_attr_ex *attr, size_t attr_size,
			  uint32_t *comp_mask)
{
	struct ibv_query_device_ex cmd;
	struct query_device_ex_resp resp;
	uint64_t raw_fw_ver;
	unsigned major, minor, sub_minor;
	int ret;

	memset(&resp, 0, sizeof(resp));

	ret = ibv_cmd_query_device_ex(context, input, attr, attr_size,
				      &raw_fw_ver, &cmd, sizeof(cmd),
				      sizeof(cmd), &resp.core,
				      sizeof(resp.core), sizeof(resp));
	if (ret)
		return ret;

	parse_raw_fw_ver(raw_fw_ver, &major, &minor, &sub_minor);

	snprintf(attr->orig_attr.fw_ver, sizeof(attr->orig_attr.fw_ver),
		 "%d.%d.%03d", major, minor, sub_minor);

	if (resp.comp_mask & QUERY_DEVICE_RESP_MASK_TIMESTAMP)
		to_mctx(context)->core_clock_offset =
			resp.hca_core_clock_offset;

	if (comp_mask)
		*comp_mask = resp.comp_mask;

	return 0;
}

int mlx4_query_device_ex(struct ibv_context *context,
			 const struct ibv_query_device_ex_input *input,
			 struct ibv_device_attr_ex *attr, size_t attr_size)
{
	return _mlx4_query_device_ex(context, input, attr, attr_size, NULL);
}

int mlx4_query_port(struct ibv_context *context, uint8_t port,
		     struct ibv_port_attr *attr)
{
	struct ibv_query_port cmd;
	int err;

	err = ibv_cmd_query_port(context, port, attr, &cmd, sizeof(cmd));
	if (!err && port <= MLX4_PORTS_NUM && port > 0) {
		struct mlx4_context *mctx = to_mctx(context);
		if (!mctx->port_query_cache[port - 1].valid) {
			mctx->port_query_cache[port - 1].link_layer =
				attr->link_layer;
			mctx->port_query_cache[port - 1].caps =
				attr->port_cap_flags;
			mctx->port_query_cache[port - 1].valid = 1;
		}
	}

	return err;
}

/* Only the fields in the port cache will be valid */
static int query_port_cache(struct ibv_context *context, uint8_t port_num,
			    struct ibv_port_attr *port_attr)
{
	struct mlx4_context *mctx = to_mctx(context);
	if (port_num <= 0 || port_num > MLX4_PORTS_NUM)
		return -EINVAL;
	if (mctx->port_query_cache[port_num - 1].valid) {
		port_attr->link_layer =
			mctx->
			port_query_cache[port_num - 1].
			link_layer;
		port_attr->port_cap_flags =
			mctx->
			port_query_cache[port_num - 1].
			caps;
		return 0;
	}
	return mlx4_query_port(context, port_num,
			       (struct ibv_port_attr *)port_attr);

}

struct ibv_pd *mlx4_alloc_pd(struct ibv_context *context)
{
	struct ibv_alloc_pd       cmd;
	struct mlx4_alloc_pd_resp resp;
	struct mlx4_pd		 *pd;

	pd = malloc(sizeof *pd);
	if (!pd)
		return NULL;

	if (ibv_cmd_alloc_pd(context, &pd->ibv_pd, &cmd, sizeof cmd,
			     &resp.ibv_resp, sizeof resp)) {
		free(pd);
		return NULL;
	}

	pd->pdn = resp.pdn;

	return &pd->ibv_pd;
}

int mlx4_free_pd(struct ibv_pd *pd)
{
	int ret;

	ret = ibv_cmd_dealloc_pd(pd);
	if (ret)
		return ret;

	free(to_mpd(pd));
	return 0;
}

struct ibv_xrcd *mlx4_open_xrcd(struct ibv_context *context,
				struct ibv_xrcd_init_attr *attr)
{
	struct ibv_open_xrcd cmd;
	struct ibv_open_xrcd_resp resp;
	struct verbs_xrcd *xrcd;
	int ret;

	xrcd = calloc(1, sizeof *xrcd);
	if (!xrcd)
		return NULL;

	ret = ibv_cmd_open_xrcd(context, xrcd, sizeof(*xrcd), attr,
				&cmd, sizeof cmd, &resp, sizeof resp);
	if (ret)
		goto err;

	return &xrcd->xrcd;

err:
	free(xrcd);
	return NULL;
}

int mlx4_close_xrcd(struct ibv_xrcd *ib_xrcd)
{
	struct verbs_xrcd *xrcd = container_of(ib_xrcd, struct verbs_xrcd, xrcd);
	int ret;

	ret = ibv_cmd_close_xrcd(xrcd);
	if (!ret)
		free(xrcd);

	return ret;
}

struct ibv_mr *mlx4_reg_mr(struct ibv_pd *pd, void *addr, size_t length,
			   int access)
{
	struct ibv_mr *mr;
	struct ibv_reg_mr cmd;
	struct ibv_reg_mr_resp resp;
	int ret;

	mr = malloc(sizeof *mr);
	if (!mr)
		return NULL;

	ret = ibv_cmd_reg_mr(pd, addr, length, (uintptr_t) addr,
			     access, mr, &cmd, sizeof cmd,
			     &resp, sizeof resp);
	if (ret) {
		free(mr);
		return NULL;
	}

	return mr;
}

int mlx4_rereg_mr(struct ibv_mr *mr,
		  int flags,
		  struct ibv_pd *pd, void *addr,
		  size_t length, uint64_t access)
{
	struct ibv_rereg_mr cmd;
	struct ibv_rereg_mr_resp resp;

	return ibv_cmd_rereg_mr(mr, flags, addr, length,
				(uintptr_t) addr,
				access, pd,
				&cmd, sizeof(cmd),
				&resp, sizeof(resp));
}

int mlx4_dereg_mr(struct ibv_mr *mr)
{
	int ret;

	ret = ibv_cmd_dereg_mr(mr);
	if (ret)
		return ret;

	free(mr);
	return 0;
}

int align_queue_size(int req)
{
	int nent;

	for (nent = 1; nent < req; nent <<= 1)
		; /* nothing */

	return nent;
}

struct ibv_cq *mlx4_create_cq(struct ibv_context *context, int cqe,
			       struct ibv_comp_channel *channel,
			       int comp_vector)
{
	struct mlx4_create_cq      cmd;
	struct mlx4_create_cq_resp resp;
	struct mlx4_cq		  *cq;
	int			   ret;
	struct mlx4_context       *mctx = to_mctx(context);

	/* Sanity check CQ size before proceeding */
	if (cqe > 0x3fffff)
		return NULL;

	cq = malloc(sizeof *cq);
	if (!cq)
		return NULL;

	cq->cons_index = 0;

	if (pthread_spin_init(&cq->lock, PTHREAD_PROCESS_PRIVATE))
		goto err;

	cqe = align_queue_size(cqe + 1);

	if (mlx4_alloc_cq_buf(to_mdev(context->device), &cq->buf, cqe, mctx->cqe_size))
		goto err;

	cq->cqe_size = mctx->cqe_size;
	cq->set_ci_db  = mlx4_alloc_db(to_mctx(context), MLX4_DB_TYPE_CQ);
	if (!cq->set_ci_db)
		goto err_buf;

	cq->arm_db     = cq->set_ci_db + 1;
	*cq->arm_db    = 0;
	cq->arm_sn     = 1;
	*cq->set_ci_db = 0;

	cmd.buf_addr = (uintptr_t) cq->buf.buf;
	cmd.db_addr  = (uintptr_t) cq->set_ci_db;

	ret = ibv_cmd_create_cq(context, cqe - 1, channel, comp_vector,
				&cq->ibv_cq, &cmd.ibv_cmd, sizeof cmd,
				&resp.ibv_resp, sizeof resp);
	if (ret)
		goto err_db;

	cq->cqn = resp.cqn;

	return &cq->ibv_cq;

err_db:
	mlx4_free_db(to_mctx(context), MLX4_DB_TYPE_CQ, cq->set_ci_db);

err_buf:
	mlx4_free_buf(&cq->buf);

err:
	free(cq);

	return NULL;
}

int mlx4_resize_cq(struct ibv_cq *ibcq, int cqe)
{
	struct mlx4_cq *cq = to_mcq(ibcq);
	struct mlx4_resize_cq cmd;
	struct ibv_resize_cq_resp resp;
	struct mlx4_buf buf;
	int old_cqe, outst_cqe, ret;

	/* Sanity check CQ size before proceeding */
	if (cqe > 0x3fffff)
		return EINVAL;

	pthread_spin_lock(&cq->lock);

	cqe = align_queue_size(cqe + 1);
	if (cqe == ibcq->cqe + 1) {
		ret = 0;
		goto out;
	}

	/* Can't be smaller then the number of outstanding CQEs */
	outst_cqe = mlx4_get_outstanding_cqes(cq);
	if (cqe < outst_cqe + 1) {
		ret = 0;
		goto out;
	}

	ret = mlx4_alloc_cq_buf(to_mdev(ibcq->context->device), &buf, cqe, cq->cqe_size);
	if (ret)
		goto out;

	old_cqe = ibcq->cqe;
	cmd.buf_addr = (uintptr_t) buf.buf;

	ret = ibv_cmd_resize_cq(ibcq, cqe - 1, &cmd.ibv_cmd, sizeof cmd,
				&resp, sizeof resp);
	if (ret) {
		mlx4_free_buf(&buf);
		goto out;
	}

	mlx4_cq_resize_copy_cqes(cq, buf.buf, old_cqe);

	mlx4_free_buf(&cq->buf);
	cq->buf = buf;

out:
	pthread_spin_unlock(&cq->lock);
	return ret;
}

int mlx4_destroy_cq(struct ibv_cq *cq)
{
	int ret;

	ret = ibv_cmd_destroy_cq(cq);
	if (ret)
		return ret;

	mlx4_free_db(to_mctx(cq->context), MLX4_DB_TYPE_CQ, to_mcq(cq)->set_ci_db);
	mlx4_free_buf(&to_mcq(cq)->buf);
	free(to_mcq(cq));

	return 0;
}

struct ibv_srq *mlx4_create_srq(struct ibv_pd *pd,
				struct ibv_srq_init_attr *attr)
{
	struct mlx4_create_srq      cmd;
	struct mlx4_create_srq_resp resp;
	struct mlx4_srq		   *srq;
	int			    ret;

	/* Sanity check SRQ size before proceeding */
	if (attr->attr.max_wr > 1 << 16 || attr->attr.max_sge > 64)
		return NULL;

	srq = malloc(sizeof *srq);
	if (!srq)
		return NULL;

	if (pthread_spin_init(&srq->lock, PTHREAD_PROCESS_PRIVATE))
		goto err;

	srq->max     = align_queue_size(attr->attr.max_wr + 1);
	srq->max_gs  = attr->attr.max_sge;
	srq->counter = 0;
	srq->ext_srq = 0;

	if (mlx4_alloc_srq_buf(pd, &attr->attr, srq))
		goto err;

	srq->db = mlx4_alloc_db(to_mctx(pd->context), MLX4_DB_TYPE_RQ);
	if (!srq->db)
		goto err_free;

	*srq->db = 0;

	cmd.buf_addr = (uintptr_t) srq->buf.buf;
	cmd.db_addr  = (uintptr_t) srq->db;

	ret = ibv_cmd_create_srq(pd, &srq->verbs_srq.srq, attr,
				 &cmd.ibv_cmd, sizeof cmd,
				 &resp.ibv_resp, sizeof resp);
	if (ret)
		goto err_db;

	return &srq->verbs_srq.srq;

err_db:
	mlx4_free_db(to_mctx(pd->context), MLX4_DB_TYPE_RQ, srq->db);

err_free:
	free(srq->wrid);
	mlx4_free_buf(&srq->buf);

err:
	free(srq);

	return NULL;
}

struct ibv_srq *mlx4_create_srq_ex(struct ibv_context *context,
				   struct ibv_srq_init_attr_ex *attr_ex)
{
	if (!(attr_ex->comp_mask & IBV_SRQ_INIT_ATTR_TYPE) ||
	    (attr_ex->srq_type == IBV_SRQT_BASIC))
		return mlx4_create_srq(attr_ex->pd, (struct ibv_srq_init_attr *) attr_ex);
	else if (attr_ex->srq_type == IBV_SRQT_XRC)
		return mlx4_create_xrc_srq(context, attr_ex);

	return NULL;
}

int mlx4_modify_srq(struct ibv_srq *srq,
		     struct ibv_srq_attr *attr,
		     int attr_mask)
{
	struct ibv_modify_srq cmd;

	return ibv_cmd_modify_srq(srq, attr, attr_mask, &cmd, sizeof cmd);
}

int mlx4_query_srq(struct ibv_srq *srq,
		    struct ibv_srq_attr *attr)
{
	struct ibv_query_srq cmd;

	return ibv_cmd_query_srq(srq, attr, &cmd, sizeof cmd);
}

int mlx4_destroy_srq(struct ibv_srq *srq)
{
	int ret;

	if (to_msrq(srq)->ext_srq)
		return mlx4_destroy_xrc_srq(srq);

	ret = ibv_cmd_destroy_srq(srq);
	if (ret)
		return ret;

	mlx4_free_db(to_mctx(srq->context), MLX4_DB_TYPE_RQ, to_msrq(srq)->db);
	mlx4_free_buf(&to_msrq(srq)->buf);
	free(to_msrq(srq)->wrid);
	free(to_msrq(srq));

	return 0;
}

struct ibv_qp *mlx4_create_qp_ex(struct ibv_context *context,
				 struct ibv_qp_init_attr_ex *attr)
{
	struct mlx4_create_qp     cmd;
	struct ibv_create_qp_resp resp;
	struct mlx4_qp		 *qp;
	int			  ret;

	/* Sanity check QP size before proceeding */
	if (attr->cap.max_send_wr     > 65536 ||
	    attr->cap.max_recv_wr     > 65536 ||
	    attr->cap.max_send_sge    > 64    ||
	    attr->cap.max_recv_sge    > 64    ||
	    attr->cap.max_inline_data > 1024)
		return NULL;

	qp = calloc(1, sizeof *qp);
	if (!qp)
		return NULL;

	if (attr->qp_type == IBV_QPT_XRC_RECV) {
		attr->cap.max_send_wr = qp->sq.wqe_cnt = 0;
	} else {
		mlx4_calc_sq_wqe_size(&attr->cap, attr->qp_type, qp);
		/*
		 * We need to leave 2 KB + 1 WQE of headroom in the SQ to
		 * allow HW to prefetch.
		 */
		qp->sq_spare_wqes = (2048 >> qp->sq.wqe_shift) + 1;
		qp->sq.wqe_cnt = align_queue_size(attr->cap.max_send_wr + qp->sq_spare_wqes);
	}

	if (attr->srq || attr->qp_type == IBV_QPT_XRC_SEND ||
	    attr->qp_type == IBV_QPT_XRC_RECV) {
		attr->cap.max_recv_wr = qp->rq.wqe_cnt = attr->cap.max_recv_sge = 0;
	} else {
		qp->rq.wqe_cnt = align_queue_size(attr->cap.max_recv_wr);
		if (attr->cap.max_recv_sge < 1)
			attr->cap.max_recv_sge = 1;
		if (attr->cap.max_recv_wr < 1)
			attr->cap.max_recv_wr = 1;
	}

	if (mlx4_alloc_qp_buf(context, &attr->cap, attr->qp_type, qp))
		goto err;

	mlx4_init_qp_indices(qp);

	if (pthread_spin_init(&qp->sq.lock, PTHREAD_PROCESS_PRIVATE) ||
	    pthread_spin_init(&qp->rq.lock, PTHREAD_PROCESS_PRIVATE))
		goto err_free;

	if (attr->cap.max_recv_sge) {
		qp->db = mlx4_alloc_db(to_mctx(context), MLX4_DB_TYPE_RQ);
		if (!qp->db)
			goto err_free;

		*qp->db = 0;
		cmd.db_addr = (uintptr_t) qp->db;
	} else {
		cmd.db_addr = 0;
	}

	cmd.buf_addr	    = (uintptr_t) qp->buf.buf;
	cmd.log_sq_stride   = qp->sq.wqe_shift;
	for (cmd.log_sq_bb_count = 0;
	     qp->sq.wqe_cnt > 1 << cmd.log_sq_bb_count;
	     ++cmd.log_sq_bb_count)
		; /* nothing */
	cmd.sq_no_prefetch = 0;	/* OK for ABI 2: just a reserved field */
	memset(cmd.reserved, 0, sizeof cmd.reserved);

	pthread_mutex_lock(&to_mctx(context)->qp_table_mutex);

	ret = ibv_cmd_create_qp_ex(context, &qp->verbs_qp,
				   sizeof(qp->verbs_qp), attr,
				   &cmd.ibv_cmd, sizeof cmd, &resp, sizeof resp);
	if (ret)
		goto err_rq_db;

	if (qp->sq.wqe_cnt || qp->rq.wqe_cnt) {
		ret = mlx4_store_qp(to_mctx(context), qp->verbs_qp.qp.qp_num, qp);
		if (ret)
			goto err_destroy;
	}
	pthread_mutex_unlock(&to_mctx(context)->qp_table_mutex);

	qp->rq.wqe_cnt = qp->rq.max_post = attr->cap.max_recv_wr;
	qp->rq.max_gs  = attr->cap.max_recv_sge;
	if (attr->qp_type != IBV_QPT_XRC_RECV)
		mlx4_set_sq_sizes(qp, &attr->cap, attr->qp_type);

	qp->doorbell_qpn    = htonl(qp->verbs_qp.qp.qp_num << 8);
	if (attr->sq_sig_all)
		qp->sq_signal_bits = htonl(MLX4_WQE_CTRL_CQ_UPDATE);
	else
		qp->sq_signal_bits = 0;

	return &qp->verbs_qp.qp;

err_destroy:
	ibv_cmd_destroy_qp(&qp->verbs_qp.qp);

err_rq_db:
	pthread_mutex_unlock(&to_mctx(context)->qp_table_mutex);
	if (attr->cap.max_recv_sge)
		mlx4_free_db(to_mctx(context), MLX4_DB_TYPE_RQ, qp->db);

err_free:
	free(qp->sq.wrid);
	if (qp->rq.wqe_cnt)
		free(qp->rq.wrid);
	mlx4_free_buf(&qp->buf);

err:
	free(qp);

	return NULL;
}

struct ibv_qp *mlx4_create_qp(struct ibv_pd *pd, struct ibv_qp_init_attr *attr)
{
	struct ibv_qp_init_attr_ex attr_ex;
	struct ibv_qp *qp;

	memcpy(&attr_ex, attr, sizeof *attr);
	attr_ex.comp_mask = IBV_QP_INIT_ATTR_PD;
	attr_ex.pd = pd;
	qp = mlx4_create_qp_ex(pd->context, &attr_ex);
	if (qp)
		memcpy(attr, &attr_ex, sizeof *attr);
	return qp;
}

struct ibv_qp *mlx4_open_qp(struct ibv_context *context, struct ibv_qp_open_attr *attr)
{
	struct ibv_open_qp cmd;
	struct ibv_create_qp_resp resp;
	struct mlx4_qp *qp;
	int ret;

	qp = calloc(1, sizeof *qp);
	if (!qp)
		return NULL;

	ret = ibv_cmd_open_qp(context, &qp->verbs_qp, sizeof(qp->verbs_qp), attr,
			      &cmd, sizeof cmd, &resp, sizeof resp);
	if (ret)
		goto err;

	return &qp->verbs_qp.qp;

err:
	free(qp);
	return NULL;
}

int mlx4_query_qp(struct ibv_qp *ibqp, struct ibv_qp_attr *attr,
		   int attr_mask,
		   struct ibv_qp_init_attr *init_attr)
{
	struct ibv_query_qp cmd;
	struct mlx4_qp *qp = to_mqp(ibqp);
	int ret;

	ret = ibv_cmd_query_qp(ibqp, attr, attr_mask, init_attr, &cmd, sizeof cmd);
	if (ret)
		return ret;

	init_attr->cap.max_send_wr     = qp->sq.max_post;
	init_attr->cap.max_send_sge    = qp->sq.max_gs;
	init_attr->cap.max_inline_data = qp->max_inline_data;

	attr->cap = init_attr->cap;

	return 0;
}

int mlx4_modify_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr,
		    int attr_mask)
{
	struct ibv_modify_qp cmd;
	struct ibv_port_attr port_attr;
	struct mlx4_qp *mqp = to_mqp(qp);
	struct ibv_device_attr device_attr;
	int ret;

	memset(&device_attr, 0, sizeof(device_attr));
	if (attr_mask & IBV_QP_PORT) {
		ret = ibv_query_port(qp->context, attr->port_num,
				     &port_attr);
		if (ret)
			return ret;
		mqp->link_layer = port_attr.link_layer;

		ret = ibv_query_device(qp->context, &device_attr);
		if (ret)
			return ret;

		switch(qp->qp_type) {
		case IBV_QPT_UD:
			if ((mqp->link_layer == IBV_LINK_LAYER_INFINIBAND) &&
			    (device_attr.device_cap_flags & IBV_DEVICE_UD_IP_CSUM))
				mqp->qp_cap_cache |= MLX4_CSUM_SUPPORT_UD_OVER_IB |
						MLX4_RX_CSUM_VALID;
			break;
		case IBV_QPT_RAW_PACKET:
			if ((mqp->link_layer == IBV_LINK_LAYER_ETHERNET) &&
			    (device_attr.device_cap_flags & IBV_DEVICE_RAW_IP_CSUM))
				mqp->qp_cap_cache |= MLX4_CSUM_SUPPORT_RAW_OVER_ETH |
						MLX4_RX_CSUM_VALID;
			break;
		default:
			break;
		}

	}

	if (qp->state == IBV_QPS_RESET &&
	    attr_mask & IBV_QP_STATE   &&
	    attr->qp_state == IBV_QPS_INIT) {
		mlx4_qp_init_sq_ownership(to_mqp(qp));
	}

	ret = ibv_cmd_modify_qp(qp, attr, attr_mask, &cmd, sizeof cmd);

	if (!ret		       &&
	    (attr_mask & IBV_QP_STATE) &&
	    attr->qp_state == IBV_QPS_RESET) {
		if (qp->recv_cq)
			mlx4_cq_clean(to_mcq(qp->recv_cq), qp->qp_num,
				      qp->srq ? to_msrq(qp->srq) : NULL);
		if (qp->send_cq && qp->send_cq != qp->recv_cq)
			mlx4_cq_clean(to_mcq(qp->send_cq), qp->qp_num, NULL);

		mlx4_init_qp_indices(to_mqp(qp));
		if (to_mqp(qp)->rq.wqe_cnt)
			*to_mqp(qp)->db = 0;
	}

	return ret;
}

static void mlx4_lock_cqs(struct ibv_qp *qp)
{
	struct mlx4_cq *send_cq = to_mcq(qp->send_cq);
	struct mlx4_cq *recv_cq = to_mcq(qp->recv_cq);

	if (!qp->send_cq || !qp->recv_cq) {
		if (qp->send_cq)
			pthread_spin_lock(&send_cq->lock);
		else if (qp->recv_cq)
			pthread_spin_lock(&recv_cq->lock);
	} else if (send_cq == recv_cq) {
		pthread_spin_lock(&send_cq->lock);
	} else if (send_cq->cqn < recv_cq->cqn) {
		pthread_spin_lock(&send_cq->lock);
		pthread_spin_lock(&recv_cq->lock);
	} else {
		pthread_spin_lock(&recv_cq->lock);
		pthread_spin_lock(&send_cq->lock);
	}
}

static void mlx4_unlock_cqs(struct ibv_qp *qp)
{
	struct mlx4_cq *send_cq = to_mcq(qp->send_cq);
	struct mlx4_cq *recv_cq = to_mcq(qp->recv_cq);


	if (!qp->send_cq || !qp->recv_cq) {
		if (qp->send_cq)
			pthread_spin_unlock(&send_cq->lock);
		else if (qp->recv_cq)
			pthread_spin_unlock(&recv_cq->lock);
	} else if (send_cq == recv_cq) {
		pthread_spin_unlock(&send_cq->lock);
	} else if (send_cq->cqn < recv_cq->cqn) {
		pthread_spin_unlock(&recv_cq->lock);
		pthread_spin_unlock(&send_cq->lock);
	} else {
		pthread_spin_unlock(&send_cq->lock);
		pthread_spin_unlock(&recv_cq->lock);
	}
}

int mlx4_destroy_qp(struct ibv_qp *ibqp)
{
	struct mlx4_qp *qp = to_mqp(ibqp);
	int ret;

	pthread_mutex_lock(&to_mctx(ibqp->context)->qp_table_mutex);
	ret = ibv_cmd_destroy_qp(ibqp);
	if (ret) {
		pthread_mutex_unlock(&to_mctx(ibqp->context)->qp_table_mutex);
		return ret;
	}

	mlx4_lock_cqs(ibqp);

	if (ibqp->recv_cq)
		__mlx4_cq_clean(to_mcq(ibqp->recv_cq), ibqp->qp_num,
				ibqp->srq ? to_msrq(ibqp->srq) : NULL);
	if (ibqp->send_cq && ibqp->send_cq != ibqp->recv_cq)
		__mlx4_cq_clean(to_mcq(ibqp->send_cq), ibqp->qp_num, NULL);

	if (qp->sq.wqe_cnt || qp->rq.wqe_cnt)
		mlx4_clear_qp(to_mctx(ibqp->context), ibqp->qp_num);

	mlx4_unlock_cqs(ibqp);
	pthread_mutex_unlock(&to_mctx(ibqp->context)->qp_table_mutex);

	if (qp->rq.wqe_cnt) {
		mlx4_free_db(to_mctx(ibqp->context), MLX4_DB_TYPE_RQ, qp->db);
		free(qp->rq.wrid);
	}
	if (qp->sq.wqe_cnt)
		free(qp->sq.wrid);
	mlx4_free_buf(&qp->buf);
	free(qp);

	return 0;
}

static int link_local_gid(const union ibv_gid *gid)
{
	uint32_t hi = *(uint32_t *)(gid->raw);
	uint32_t lo = *(uint32_t *)(gid->raw + 4);
	if (hi == htonl(0xfe800000) && lo == 0)
		return 1;

	return 0;
}

static int is_multicast_gid(const union ibv_gid *gid)
{
	return gid->raw[0] == 0xff;
}

static uint16_t get_vlan_id(union ibv_gid *gid)
{
	uint16_t vid;
	vid = gid->raw[11] << 8 | gid->raw[12];
	return vid < 0x1000 ? vid : 0xffff;
}

static int mlx4_resolve_grh_to_l2(struct ibv_pd *pd, struct mlx4_ah *ah,
				  struct ibv_ah_attr *attr)
{
	int err, i;
	uint16_t vid;
	union ibv_gid sgid;

	if (link_local_gid(&attr->grh.dgid)) {
		memcpy(ah->mac, &attr->grh.dgid.raw[8], 3);
		memcpy(ah->mac + 3, &attr->grh.dgid.raw[13], 3);
		ah->mac[0] ^= 2;

		vid = get_vlan_id(&attr->grh.dgid);
	} else if (is_multicast_gid(&attr->grh.dgid)) {
		ah->mac[0] = 0x33;
		ah->mac[1] = 0x33;
		for (i = 2; i < 6; ++i)
			ah->mac[i] = attr->grh.dgid.raw[i + 10];

		err = ibv_query_gid(pd->context, attr->port_num,
				    attr->grh.sgid_index, &sgid);
		if (err)
			return err;

		ah->av.dlid = htons(0xc000);
		ah->av.port_pd |= htonl(1 << 31);

		vid = get_vlan_id(&sgid);
	} else
		return 1;

	if (vid != 0xffff) {
		ah->av.port_pd |= htonl(1 << 29);
		ah->vlan = vid | ((attr->sl & 7) << 13);
	}

	return 0;
}

struct ibv_ah *mlx4_create_ah(struct ibv_pd *pd, struct ibv_ah_attr *attr)
{
	struct mlx4_ah *ah;
	struct ibv_port_attr port_attr;

	if (query_port_cache(pd->context, attr->port_num, &port_attr))
		return NULL;

	ah = malloc(sizeof *ah);
	if (!ah)
		return NULL;

	memset(&ah->av, 0, sizeof ah->av);

	ah->av.port_pd   = htonl(to_mpd(pd)->pdn | (attr->port_num << 24));

	if (port_attr.link_layer != IBV_LINK_LAYER_ETHERNET) {
		ah->av.g_slid = attr->src_path_bits;
		ah->av.dlid   = htons(attr->dlid);
		ah->av.sl_tclass_flowlabel = htonl(attr->sl << 28);
	} else
		ah->av.sl_tclass_flowlabel = htonl(attr->sl << 29);

	if (attr->static_rate) {
		ah->av.stat_rate = attr->static_rate + MLX4_STAT_RATE_OFFSET;
		/* XXX check rate cap? */
	}
	if (attr->is_global) {
		ah->av.g_slid   |= 0x80;
		ah->av.gid_index = attr->grh.sgid_index;
		ah->av.hop_limit = attr->grh.hop_limit;
		ah->av.sl_tclass_flowlabel |=
			htonl((attr->grh.traffic_class << 20) |
				    attr->grh.flow_label);
		memcpy(ah->av.dgid, attr->grh.dgid.raw, 16);
	}

	if (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET) {
		if (port_attr.port_cap_flags & IBV_PORT_IP_BASED_GIDS) {
			uint16_t vid;

			if (ibv_resolve_eth_l2_from_gid(pd->context, attr,
							ah->mac, &vid)) {
				free(ah);
				return NULL;
			}

			if (vid <= 0xfff) {
				ah->av.port_pd |= htonl(1 << 29);
				ah->vlan = vid |
					((attr->sl & 7) << 13);
			}

		} else {
			if (mlx4_resolve_grh_to_l2(pd, ah, attr)) {
				free(ah);
				return NULL;
			}
		}
	}

	return &ah->ibv_ah;
}

int mlx4_destroy_ah(struct ibv_ah *ah)
{
	free(to_mah(ah));

	return 0;
}
