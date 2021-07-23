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

	if (base_hdr->type == RXR_RECEIPT_PKT) {
		return (base_hdr->flags & RXR_RECEIPT_OPT_CONNID_HDR)
			? rxr_get_receipt_hdr(pkt_entry->pkt)->connid_hdr
			: NULL;
	}

	if (base_hdr->type == RXR_DATA_PKT) {
		return (base_hdr->flags & RXR_DATA_OPT_CONNID_HDR)
			? rxr_get_data_hdr(pkt_entry->pkt)->connid_hdr
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

/* @brief return the data size in a packet entry
 *
 * @param[in]	pkt_entry		packet entry
 * @return	the data size in the packet entry.
 * 		if the packet entry does not contain data,
 * 		return 0.
 */
size_t rxr_pkt_data_size(struct rxr_pkt_entry *pkt_entry)
{
	int pkt_type;

	assert(pkt_entry);
	pkt_type = rxr_get_base_hdr(pkt_entry->pkt)->type;

	if (pkt_type == RXR_DATA_PKT)
		return rxr_get_data_hdr(pkt_entry->pkt)->seg_size;

	if (pkt_type == RXR_READRSP_PKT)
		return pkt_entry->pkt_size - sizeof(struct rxr_readrsp_hdr);

	if (pkt_type >= RXR_REQ_PKT_BEGIN) {
		assert(pkt_type == RXR_EAGER_MSGRTM_PKT || pkt_type == RXR_EAGER_TAGRTM_PKT ||
		       pkt_type == RXR_MEDIUM_MSGRTM_PKT || pkt_type == RXR_MEDIUM_TAGRTM_PKT ||
		       pkt_type == RXR_LONG_MSGRTM_PKT || pkt_type == RXR_LONG_TAGRTM_PKT ||
		       pkt_type == RXR_EAGER_RTW_PKT ||
		       pkt_type == RXR_LONG_RTW_PKT ||
		       pkt_type == RXR_DC_EAGER_MSGRTM_PKT ||
		       pkt_type == RXR_DC_EAGER_TAGRTM_PKT ||
		       pkt_type == RXR_DC_MEDIUM_MSGRTM_PKT ||
		       pkt_type == RXR_DC_MEDIUM_TAGRTM_PKT ||
		       pkt_type == RXR_DC_LONG_MSGRTM_PKT ||
		       pkt_type == RXR_DC_LONG_TAGRTM_PKT ||
		       pkt_type == RXR_DC_EAGER_RTW_PKT ||
		       pkt_type == RXR_DC_LONG_RTW_PKT);

		return pkt_entry->pkt_size - rxr_pkt_req_hdr_size(pkt_entry);
	}

	/* other packet type does not contain data, thus return 0
	 */
	return 0;
}

/**
 * @brief copy data to receive buffer and update counter in rx_entry.
 *
 * If receiving buffer is on GPU memory, it will post a local
 * read request. Otherwise it will copy data directly, and call
 * rxr_pkt_handle_data_copied().
 *
 * @param[in]		ep		endpoint
 * @param[in,out]	rx_entry	rx_entry contains information of the receive
 *                      	        op. This function uses receive buffer in it.
 * @param[in]		data_offset	the offset of the data in the packet in respect
 *					of the receiving buffer.
 * @param[in]		pkt_entry	the packet entry that contains data
 * @param[in]		data		the pointer poiting th the beginning of data
 * @param[in]		data_size	the length of data
 * @return		On success, return 0
 * 			On failure, return libfabric error code
 */
ssize_t rxr_pkt_copy_data_to_rx_entry(struct rxr_ep *ep,
				      struct rxr_rx_entry *rx_entry,
				      size_t data_offset,
				      struct rxr_pkt_entry *pkt_entry,
				      char *data, size_t data_size)
{
	ssize_t err, bytes_copied;

	pkt_entry->x_entry = rx_entry;

	if (data_size > 0 && efa_ep_is_cuda_mr(rx_entry->desc[0])) {
		err = rxr_read_post_local_read_or_queue(ep, rx_entry, data_offset,
							pkt_entry, data, data_size);
		if (err)
			FI_WARN(&rxr_prov, FI_LOG_CQ, "cannot post read to copy data\n");

		return err;
	}

	if (OFI_LIKELY(!(rx_entry->rxr_flags & RXR_RECV_CANCEL)) &&
	    rx_entry->cq_entry.len > data_offset && data_size > 0) {
		bytes_copied = ofi_copy_to_iov(rx_entry->iov,
					       rx_entry->iov_count,
					       data_offset,
					       data,
					       data_size);
		if (bytes_copied != MIN(data_size, rx_entry->cq_entry.len - data_offset)) {
			FI_WARN(&rxr_prov, FI_LOG_CQ, "wrong size! bytes_copied: %ld\n",
				bytes_copied);
			return -FI_EINVAL;
		}
	}

	rxr_pkt_handle_data_copied(ep, pkt_entry, data_size);
	return 0;
}
