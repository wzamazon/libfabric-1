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

	return NULL;
}
