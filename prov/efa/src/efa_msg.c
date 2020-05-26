/*
 * Copyright (c) 2013-2015 Intel Corporation, Inc.  All rights reserved.
 * Copyright (c) 2017-2020 Amazon.com, Inc. or its affiliates. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
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

#include "config.h"


#include "ofi.h"
#include "ofi_enosys.h"
#include "ofi_iov.h"

#include "efa.h"

#define EFA_SETUP_IOV(iov, buf, len)           \
	do {                                   \
		iov.iov_base = (void *)buf;    \
		iov.iov_len = (size_t)len;     \
	} while (0)

#define EFA_SETUP_MSG(msg, iov, _desc, count, _addr, _context, _data)    \
	do {                                                             \
		msg.msg_iov = (const struct iovec *)iov;                 \
		msg.desc = (void **)_desc;                               \
		msg.iov_count = (size_t)count;                           \
		msg.addr = (fi_addr_t)_addr;                             \
		msg.context = (void *)_context;                          \
		msg.data = (uint32_t)_data;                              \
	} while (0)

#ifndef EFA_MSG_DUMP
static inline void dump_msg(const struct fi_msg *msg, const char *context) {}
#else
#define DUMP_IOV(i, iov, desc) \
	EFA_DBG(FI_LOG_EP_DATA, \
		"\t{ iov[%d] = { base = %p, buff = \"%s\", len = %zu }, desc = %p },\n", \
		i, iov.iov_base, (char *)iov.iov_base, iov.iov_len, (desc ? desc[i] : NULL))

static inline void dump_msg(const struct fi_msg *msg, const char *context)
{
	int i;

	EFA_DBG(FI_LOG_EP_DATA, "%s: { data = %u, addr = %" PRIu64 ", iov_count = %zu, [\n",
		context, (unsigned)msg->data, msg->addr, msg->iov_count);
	for (i = 0; i < msg->iov_count; ++i)
		DUMP_IOV(i, msg->msg_iov[i], msg->desc);
	EFA_DBG(FI_LOG_EP_DATA, " ] }\n");
}
#endif /* EFA_MSG_DUMP */

static void free_send_wr_list(struct ibv_send_wr *head)
{
	struct ibv_send_wr *wr = head;
	struct ibv_send_wr *tmp;

	while (wr) {
		tmp = wr->next;
		ofi_buf_free(container_of(wr, struct efa_send_wr, wr));
		wr = tmp;
	}
}

static void free_recv_wr_list(struct ibv_recv_wr *head)
{
	struct ibv_recv_wr *wr = head;
	struct ibv_recv_wr *tmp;

	while (wr) {
		tmp = wr->next;
		ofi_buf_free(container_of(wr, struct efa_recv_wr, wr));
		wr = tmp;
	}
}

static ssize_t efa_post_recv_validate(struct efa_ep *ep, const struct fi_msg *msg)
{
	if (OFI_UNLIKELY(!ep->rcq)) {
		EFA_WARN(FI_LOG_EP_DATA, "No receive cq was bound to ep.\n");
		return -FI_EINVAL;
	}

	if (OFI_UNLIKELY(msg->iov_count > ep->info->rx_attr->iov_limit)) {
		EFA_WARN(FI_LOG_EP_DATA, "requested sge[%zu] is greater than max supported[%zu]!\n",
			 msg->iov_count, ep->info->tx_attr->iov_limit);
		return -FI_EINVAL;
	}

	if (OFI_UNLIKELY(msg->msg_iov[0].iov_len <
			 ep->info->ep_attr->msg_prefix_size)) {
		EFA_WARN(FI_LOG_EP_DATA, "prefix not present on first iov, iov_len[%zu]\n",
			 msg->msg_iov[0].iov_len);
		return -EINVAL;
	}

	return 0;
}

static ssize_t efa_post_recv(struct efa_ep *ep, const struct fi_msg *msg, uint64_t flags)
{
	struct ibv_mr *ibv_mr;
	struct efa_qp *qp = ep->qp;
	struct ibv_recv_wr *bad_wr;
	struct efa_recv_wr *ewr;
	struct ibv_recv_wr *wr;
	uintptr_t addr;
	ssize_t err;
	size_t i;

	ewr = ofi_buf_alloc(ep->recv_wr_pool);
	if (OFI_UNLIKELY(!ewr))
		return -FI_ENOMEM;

	memset(ewr, 0, sizeof(*ewr) + sizeof(*ewr->sge) * msg->iov_count);
	wr = &ewr->wr;
	dump_msg(msg, "recv");

	err = efa_post_recv_validate(ep, msg);
	if (OFI_UNLIKELY(err)) {
		ofi_buf_free(ewr);
		goto out_err;
	}

	wr->wr_id = (uintptr_t)msg->context;
	wr->num_sge = msg->iov_count;
	wr->sg_list = ewr->sge;

	for (i = 0; i < msg->iov_count; i++) {
		addr = (uintptr_t)msg->msg_iov[i].iov_base;

		/* Set RX buffer desc from SGE */
		wr->sg_list[i].length = msg->msg_iov[i].iov_len;
		assert(msg->desc[i]);
		ibv_mr = (struct ibv_mr *)msg->desc[i];
		wr->sg_list[i].lkey = ibv_mr->lkey;
		wr->sg_list[i].addr = addr;
	}

	ep->recv_more_wr_tail->next = wr;
	ep->recv_more_wr_tail = wr;

	if (flags & FI_MORE)
		return 0;

	err = ibv_post_recv(qp->ibv_qp, ep->recv_more_wr_head.next, &bad_wr);

	free_recv_wr_list(ep->recv_more_wr_head.next);
	ep->recv_more_wr_tail = &ep->recv_more_wr_head;

	return err;

out_err:
	if (ep->recv_more_wr_head.next)
		ibv_post_recv(qp->ibv_qp, ep->recv_more_wr_head.next, &bad_wr);

	free_recv_wr_list(ep->recv_more_wr_head.next);
	ep->recv_more_wr_tail = &ep->recv_more_wr_head;

	return err;
}

static ssize_t efa_ep_recvmsg(struct fid_ep *ep_fid, const struct fi_msg *msg, uint64_t flags)
{
	struct efa_ep *ep = container_of(ep_fid, struct efa_ep, util_ep.ep_fid);

	return efa_post_recv(ep, msg, flags);
}

static ssize_t efa_ep_recv(struct fid_ep *ep_fid, void *buf, size_t len,
			   void *desc, fi_addr_t src_addr, void *context)
{
	struct efa_ep *ep = container_of(ep_fid, struct efa_ep, util_ep.ep_fid);
	struct iovec iov;
	struct fi_msg msg;

	EFA_SETUP_IOV(iov, buf, len);
	EFA_SETUP_MSG(msg, &iov, &desc, 1, src_addr, context, 0);

	return efa_post_recv(ep, &msg, 0);
}

static ssize_t efa_ep_recvv(struct fid_ep *ep_fid, const struct iovec *iov, void **desc,
			    size_t count, fi_addr_t src_addr, void *context)
{
	struct efa_ep *ep = container_of(ep_fid, struct efa_ep, util_ep.ep_fid);
	struct fi_msg msg;

	EFA_SETUP_MSG(msg, iov, desc, count, src_addr, context, 0);

	return efa_post_recv(ep, &msg, 0);
}

static ssize_t efa_post_send_validate(struct efa_ep *ep, const struct fi_msg *msg,
				      struct efa_conn *conn, uint64_t flags, size_t *len)
{
	if (OFI_UNLIKELY(!ep->scq)) {
		EFA_WARN(FI_LOG_EP_DATA, "No send cq was bound to ep.\n");
		return -FI_EINVAL;
	}

	if (OFI_UNLIKELY(msg->iov_count > ep->info->tx_attr->iov_limit)) {
		EFA_WARN(FI_LOG_EP_DATA, "requested sge[%zu] is greater than max supported[%zu]!\n",
			 msg->iov_count, ep->info->tx_attr->iov_limit);
		return -FI_EINVAL;
	}

	if (OFI_UNLIKELY(msg->msg_iov[0].iov_len <
			 ep->info->ep_attr->msg_prefix_size)) {
		EFA_WARN(FI_LOG_EP_DATA, "prefix not present on first iov, iov_len[%zu]\n",
			 msg->msg_iov[0].iov_len);
		return -EINVAL;
	}

	*len = ofi_total_iov_len(msg->msg_iov, msg->iov_count) - ep->info->ep_attr->msg_prefix_size;
	if (OFI_UNLIKELY(*len > ep->info->ep_attr->max_msg_size)) {
		EFA_WARN(FI_LOG_EP_DATA, "requested size[%zu] is greater than max[%zu]!\n",
			 *len, ep->info->ep_attr->max_msg_size);
		return -FI_EINVAL;
	}

	return 0;
}

static void efa_post_send_sgl(struct efa_ep *ep, const struct fi_msg *msg,
			      struct efa_send_wr *ewr)
{
	struct ibv_mr *ibv_mr;
	struct ibv_send_wr *wr = &ewr->wr;
	struct ibv_sge *sge;
	size_t sgl_idx = 0;
	uint32_t length;
	uintptr_t addr;
	size_t i;

	wr->num_sge = msg->iov_count;
	wr->sg_list = ewr->sge;

	for (i = 0; i < msg->iov_count; i++) {
		sge = &wr->sg_list[sgl_idx];
		addr = (uintptr_t)msg->msg_iov[i].iov_base;
		length = msg->msg_iov[i].iov_len;

		/* Whole prefix must be on the first sgl */
		if (!i) {
			/* Check if payload exists */
			if (length <= ep->info->ep_attr->msg_prefix_size)
				continue;

			addr += ep->info->ep_attr->msg_prefix_size;
			length -= ep->info->ep_attr->msg_prefix_size;
		}

		/* Set TX buffer desc from SGE */
		sge->length = length;
		if (msg->desc && msg->desc[i]) {
			ibv_mr = (struct ibv_mr *)msg->desc[i];
			sge->lkey = ibv_mr->lkey;
		} else {
			sge->lkey = 0;
		}
		sge->addr = addr;
		sgl_idx++;
	}
}

static ssize_t efa_post_send(struct efa_ep *ep, const struct fi_msg *msg, uint64_t flags)
{
        struct efa_qp *qp = ep->qp;
        struct efa_conn *conn;
        struct ibv_sge sge_list[3];
        int i,lkey;

        /* caller must provide desc because efa claim FI_MR_LOCAL */
        assert(msg->desc);
        assert(msg->iov_count < 3 );

        ibv_wr_start(qp->ibv_qp_ex);
        qp->ibv_qp_ex->wr_id = (uintptr_t)msg->context;

	if (flags & FI_REMOTE_CQ_DATA) {
	        fprintf(stderr, "send with imm data. data: %d\n", msg->data);
        	ibv_wr_send_imm(qp->ibv_qp_ex, msg->data);
	} else {
		ibv_wr_send(qp->ibv_qp_ex);
	}

        for (i=0; i < msg->iov_count; ++i) {
                sge_list[i].addr = (uint64_t)msg->msg_iov[i].iov_base;
                sge_list[i].length = msg->msg_iov[i].iov_len;
                sge_list[i].lkey = (uint32_t)(uintptr_t)msg->desc[i];
        }

        ibv_wr_set_sge_list(qp->ibv_qp_ex, msg->iov_count, sge_list);
        conn = ep->av->addr_to_conn(ep->av, msg->addr);
        ibv_wr_set_ud_addr(qp->ibv_qp_ex, conn->ah.ibv_ah, conn->ep_addr.qpn, EFA_QKEY);
        return ibv_wr_complete(qp->ibv_qp_ex);
}

static ssize_t efa_ep_sendmsg(struct fid_ep *ep_fid, const struct fi_msg *msg, uint64_t flags)
{
	struct efa_ep *ep = container_of(ep_fid, struct efa_ep, util_ep.ep_fid);

	return efa_post_send(ep, msg, flags);
}

static ssize_t efa_ep_send(struct fid_ep *ep_fid, const void *buf, size_t len,
			   void *desc, fi_addr_t dest_addr, void *context)
{
	struct efa_ep *ep = container_of(ep_fid, struct efa_ep, util_ep.ep_fid);
	struct fi_msg msg;
	struct iovec iov;
	uint64_t flags;

	EFA_SETUP_IOV(iov, buf, len);
	EFA_SETUP_MSG(msg, &iov, &desc, 1, dest_addr, context, 0);
	flags = ep->info->tx_attr->op_flags;

	return efa_post_send(ep, &msg, flags);
}

static ssize_t efa_ep_senddata(struct fid_ep *ep_fid, const void *buf, size_t len,
			       void *desc, uint64_t data, fi_addr_t dest_addr, void *context)
{
	struct efa_ep *ep = container_of(ep_fid, struct efa_ep, util_ep.ep_fid);
	struct fi_msg msg;
	struct iovec iov;
	uint64_t flags;

	EFA_SETUP_IOV(iov, buf, len);
	EFA_SETUP_MSG(msg, &iov, &desc, 1, dest_addr, context, data);

	flags = ep->info->tx_attr->op_flags | FI_REMOTE_CQ_DATA;

	return efa_post_send(ep, &msg, flags);
}

static ssize_t efa_ep_sendv(struct fid_ep *ep_fid, const struct iovec *iov, void **desc,
			    size_t count, fi_addr_t dest_addr, void *context)
{
	struct efa_ep *ep = container_of(ep_fid, struct efa_ep, util_ep.ep_fid);
	struct fi_msg msg;
	uint64_t flags;

	EFA_SETUP_MSG(msg, iov, desc, count, dest_addr, context, 0);

	flags = ep->info->tx_attr->op_flags;

	return efa_post_send(ep, &msg, flags);
}

struct fi_ops_msg efa_ep_msg_ops = {
	.size = sizeof(struct fi_ops_msg),
	.recv = efa_ep_recv,
	.recvv = efa_ep_recvv,
	.recvmsg = efa_ep_recvmsg,
	.send = efa_ep_send,
	.sendv = efa_ep_sendv,
	.sendmsg = efa_ep_sendmsg,
	.inject = fi_no_msg_inject,
	.senddata = efa_ep_senddata,
	.injectdata = fi_no_msg_injectdata,
};
