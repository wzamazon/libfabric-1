/*
 * Copyright (c) 2019-2020 Amazon.com, Inc. or its affiliates.
 * All rights reserved.
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

#include "efa.h"
#include "rxr.h"
#include "rxr_rma.h"
#include "rxr_cntr.h"
#include "rxr_read.h"

int rxr_read_post_or_queue(struct rxr_ep *ep, struct rxr_x_entry *x_entry)
{
	struct rxr_peer *peer;
	int err;

	peer = rxr_ep_get_peer(ep, x_entry->addr);
	assert(peer);
	if (!peer->is_local) {
		err = rxr_ep_init_mr_desc(ep, x_entry, 0, FI_RECV);
		if (err)
			return err;
	}

	err = rxr_read_post(ep, x_entry);
	if (err == -FI_EAGAIN) {
		dlist_insert_tail(&x_entry->queued_entry, &ep->read_pending_list);
		err = 0;
	} else if(err) {
		FI_WARN(&rxr_prov, FI_LOG_CQ,
			"RDMA post read failed. errno=%d.\n", err);
	}

	return err;
}

int rxr_read_init_iov(struct rxr_ep *ep,
		      struct rxr_tx_entry *tx_entry,
		      struct fi_rma_iov *read_iov)
{
	int i;
	struct fid_mr *mr;

	for (i = 0; i < tx_entry->base.iov_count; ++i) {
		assert(tx_entry->base.desc[i]);
		read_iov[i].addr = (uint64_t)tx_entry->base.iov[i].iov_base;
		read_iov[i].len = tx_entry->base.iov[i].iov_len;
		mr = (struct fid_mr *)tx_entry->base.desc[i];
		read_iov[i].key = fi_mr_key(mr);
	}

	return 0;
}

int rxr_read_post(struct rxr_ep *ep, struct rxr_x_entry *x_entry)
{
	int ret;
	size_t iov_idx = 0, rma_iov_idx = 0;
	void *iov_ptr, *rma_iov_ptr;
	struct rxr_peer *peer;
	struct rxr_pkt_entry *pkt_entry;
	size_t iov_offset = 0, rma_iov_offset = 0;
	size_t total_iov_len, total_rma_iov_len;
	size_t segsize, max_iov_segsize, max_rma_iov_segsize, max_read_size;
	struct fid_ep *lower_ep;
	fi_addr_t lower_ep_addr;

	assert(x_entry->iov_count > 0);
	assert(x_entry->rma_iov_count > 0);
	assert(x_entry->bytes_submitted < x_entry->total_len);

	peer = rxr_ep_get_peer(ep, x_entry->addr);
	if (peer->is_local) {
		max_read_size = SIZE_MAX;
		lower_ep = ep->shm_ep;
		lower_ep_addr = peer->shm_fiaddr;
	} else {
		max_read_size = efa_max_rdma_size(ep->rdm_ep);
		lower_ep = ep->rdm_ep;
		lower_ep_addr = x_entry->addr;
	} 
	assert(max_read_size > 0);

	ret = ofi_locate_iov(x_entry->iov, x_entry->iov_count,
			     x_entry->bytes_submitted,
			     &iov_idx, &iov_offset);
	assert(ret == 0);

	ret = ofi_locate_rma_iov(x_entry->rma_iov, x_entry->rma_iov_count,
				 x_entry->bytes_submitted,
				 &rma_iov_idx, &rma_iov_offset);
	assert(ret == 0);

	total_iov_len = ofi_total_iov_len(x_entry->iov, x_entry->iov_count);
	total_rma_iov_len = ofi_total_rma_iov_len(x_entry->rma_iov, x_entry->rma_iov_count);
	assert(x_entry->total_len == MIN(total_iov_len, total_rma_iov_len));

	while (x_entry->bytes_submitted < x_entry->total_len) {
		assert(iov_idx < x_entry->iov_count);
		assert(iov_offset < x_entry->iov[iov_idx].iov_len);
		assert(rma_iov_idx < x_entry->rma_iov_count);
		assert(rma_iov_offset < x_entry->rma_iov[rma_iov_idx].len);

		iov_ptr = (char *)x_entry->iov[iov_idx].iov_base + iov_offset;
		rma_iov_ptr = (char *)x_entry->rma_iov[rma_iov_idx].addr + rma_iov_offset;

		max_iov_segsize = x_entry->iov[iov_idx].iov_len - iov_offset;
		max_rma_iov_segsize = x_entry->rma_iov[rma_iov_idx].len - rma_iov_offset;
		segsize = MIN(max_iov_segsize, max_rma_iov_segsize);
		if (!peer->is_local)
			segsize = MIN(segsize, rxr_env.efa_read_segment_size);
		segsize = MIN(segsize, max_read_size);

		/* because fi_send uses a pkt_entry as context
		 * we had to use a pkt_entry as context too
		 */
		if (peer->is_local)
			pkt_entry = rxr_pkt_entry_alloc(ep, ep->tx_pkt_shm_pool);
		else
			pkt_entry = rxr_pkt_entry_alloc(ep, ep->tx_pkt_efa_pool);

		if (OFI_UNLIKELY(!pkt_entry))
			return -FI_EAGAIN;

		rxr_pkt_init_read_context(ep, x_entry, segsize, pkt_entry);

		ret = fi_read(lower_ep,
			      iov_ptr, segsize, x_entry->desc[iov_idx],
			      lower_ep_addr,
			      (uint64_t)rma_iov_ptr, x_entry->rma_iov[rma_iov_idx].key,
			      pkt_entry);

		if (OFI_UNLIKELY(ret)) {
			rxr_pkt_entry_release_tx(ep, pkt_entry);
			return ret;
		}

		if (!peer->is_local)
			rxr_ep_inc_tx_pending(ep, peer);
		x_entry->bytes_submitted += segsize;

		iov_offset += segsize;
		assert(iov_offset <= x_entry->iov[iov_idx].iov_len);
		if (iov_offset == x_entry->iov[iov_idx].iov_len) {
			iov_idx += 1;
			iov_offset = 0;
		}

		rma_iov_offset += segsize;
		assert(rma_iov_offset <= x_entry->rma_iov[rma_iov_idx].len);
		if (rma_iov_offset == x_entry->rma_iov[rma_iov_idx].len) {
			rma_iov_idx += 1;
			rma_iov_offset = 0;
		}
	}

	if (x_entry->total_len == total_iov_len) {
		assert(iov_idx == x_entry->iov_count);
		assert(iov_offset == 0);
	}

	if (x_entry->total_len == total_rma_iov_len) {
		assert(rma_iov_idx == x_entry->rma_iov_count);
		assert(rma_iov_offset == 0);
	}

	return 0;
}

int rxr_read_handle_error(struct rxr_ep *ep, struct rxr_x_entry *x_entry, int ret)
{
	struct rxr_tx_entry *tx_entry;
	struct rxr_rx_entry *rx_entry;

	if (x_entry->type == RXR_TX_ENTRY) {
		tx_entry = ofi_bufpool_get_ibuf(ep->tx_entry_pool, x_entry->tx_id);
		ret = rxr_cq_handle_tx_error(ep, tx_entry, ret);
	} else {
		assert(x_entry->x_entry_type == RXR_RX_ENTRY);
		rx_entry = ofi_bufpool_get_ibuf(ep->rx_entry_pool, x_entry->rx_id);
		ret = rxr_cq_handle_rx_error(ep, rx_entry, ret);
	}

	dlist_remove(&x_entry->queued_entry);
	return ret;
}

