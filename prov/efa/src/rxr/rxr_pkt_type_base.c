/*
 * Copyright (c) 2021 Amazon.com, Inc. or its affiliates.
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
#include "rxr_read.h"

/**
 * @brief initialize the optional connid header
 *
 * @param[in]	ep	endpoint
 * @param[in]	addr	fi addr of the peer
 * @param[out]	ptr	pointer of the output header
 */
void rxr_pkt_init_connid_hdr(struct rxr_ep *ep, struct rxr_opt_connid_hdr *connid_hdr)
{
	struct efa_ep_addr *self_addr;

	self_addr = (struct efa_ep_addr *)ep->core_addr;
	connid_hdr->sender_id = self_addr->qkey;
}

/**
 * @brief return the optional connid header pointer in a packet
 *
 * @param[in]	pkt_entry	an packet entry
 * @return	If the input has the optional connid header, return the pointer to connid header
 * 		Otherwise, return NULL
 */
struct rxr_opt_connid_hdr *rxr_pkt_connid_hdr(struct rxr_pkt_entry *pkt_entry)
{
	struct rxr_base_hdr *base_hdr;

	base_hdr = rxr_get_base_hdr(pkt_entry->pkt);
	if (base_hdr->type >= RXR_REQ_PKT_BEGIN)
		return rxr_pkt_req_connid_hdr(pkt_entry);

	if (base_hdr->type == RXR_CTS_PKT) {
		return (base_hdr->flags & RXR_CTS_OPT_CONNID_HDR)
			? rxr_get_cts_hdr(pkt_entry->pkt)->connid_hdr
			: NULL;
	}

	if (base_hdr->type == RXR_HANDSHAKE_PKT) {
		struct rxr_handshake_hdr *handshake_hdr;
		char *opt_hdr;

		handshake_hdr = rxr_get_handshake_hdr(pkt_entry->pkt);
		opt_hdr = pkt_entry->pkt + sizeof(struct rxr_handshake_hdr)
			  + (handshake_hdr->maxproto - handshake_hdr->version + 1) * sizeof(uint64_t);

		return (base_hdr->flags & RXR_HANDSHAKE_OPT_CONNID_HDR)
			? (struct rxr_opt_connid_hdr *)opt_hdr
			: NULL;
	}

	return NULL;
}

/**
 * @brief set up data in a packet entry using tx_entry information, such that the packet is ready to be sent.
 *        Depend on the tx_entry, this function can either copy data to packet entry, or point
 *        pkt_entry->iov to tx_entry->iov.
 *        It requires the packet header to be set.
 *
 * @param[in]		ep		end point.
 * @param[in,out]	pkt_entry	packet entry. Header must have been set when the function is called
 * @param[in]		hdr_size	packet header size.
 * @param[in]		tx_entry	This function will use iov, iov_count and desc of tx_entry
 * @param[in]		data_offset	offset of the data to be set up. In reference to tx_entry->total_len.
 * @param[in]		data_size	length of the data to be set up. In reference to tx_entry->total_len.
 * @return		no return
 */
void rxr_pkt_init_data_from_tx_entry(struct rxr_ep *ep,
				     struct rxr_pkt_entry *pkt_entry,
				     size_t hdr_size,
				     struct rxr_tx_entry *tx_entry,
				     size_t data_offset,
				     size_t data_size)
{
	int tx_iov_index;
	char *data;
	size_t tx_iov_offset, copied;
	struct efa_mr *desc;

	assert(hdr_size > 0);

	pkt_entry->x_entry = tx_entry;
	/* pkt_sendv_pool's size equal efa_tx_pkt_pool size +
	 * shm_tx_pkt_pool size. As long as we have a pkt_entry,
	 * pkt_entry->send should be allocated successfully
	 */
	pkt_entry->send = ofi_buf_alloc(ep->pkt_sendv_pool);
	assert(pkt_entry->send);

	if (data_size == 0) {
		pkt_entry->send->iov_count = 0;
		pkt_entry->pkt_size = hdr_size;
		return;
	}

	rxr_locate_iov_pos(tx_entry->iov, tx_entry->iov_count, data_offset,
			   &tx_iov_index, &tx_iov_offset);
	desc = tx_entry->desc[0];
	assert(tx_iov_index < tx_entry->iov_count);
	assert(tx_iov_offset < tx_entry->iov[tx_iov_index].iov_len);

	/*
	 * Copy can be avoid if:
	 * 1. user provided memory descriptor, or lower provider does not need memory descriptor
	 * 2. data to be send is in 1 iov, because device only support 2 iov, and we use
	 *    1st iov for header.
	 */
	if ((!pkt_entry->mr || tx_entry->desc[tx_iov_index]) &&
	    (tx_iov_offset + data_size < tx_entry->iov[tx_iov_index].iov_len)) {

		assert(ep->core_iov_limit >= 2);
		pkt_entry->send->iov[0].iov_base = pkt_entry->pkt;
		pkt_entry->send->iov[0].iov_len = hdr_size;
		pkt_entry->send->desc[0] = pkt_entry->mr ? fi_mr_desc(pkt_entry->mr) : NULL;

		pkt_entry->send->iov[1].iov_base = (char *)tx_entry->iov[tx_iov_index].iov_base + tx_iov_offset;
		pkt_entry->send->iov[1].iov_len = data_size;
		pkt_entry->send->desc[1] = tx_entry->desc[tx_iov_index];
		pkt_entry->send->iov_count = 2;
		pkt_entry->pkt_size = hdr_size + data_size;
		return;
	}

	data = pkt_entry->pkt + hdr_size;
	copied = ofi_copy_from_hmem_iov(data,
					data_size,
					desc ? desc->peer.iface : FI_HMEM_SYSTEM,
					desc ? desc->peer.device.reserved : 0,
					tx_entry->iov,
					tx_entry->iov_count,
					data_offset);
	assert(copied == data_size);
	pkt_entry->send->iov_count = 0;
	pkt_entry->pkt_size = hdr_size + copied;
}
