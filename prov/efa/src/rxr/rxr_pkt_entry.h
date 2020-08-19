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

#ifndef _RXR_PKT_ENTRY_H
#define _RXR_PKT_ENTRY_H

#include <ofi_list.h>

/* pkt_entry state for retransmit tracking */
enum rxr_pkt_entry_state {
	RXR_PKT_ENTRY_FREE = 0,
	RXR_PKT_ENTRY_IN_USE,
	RXR_PKT_ENTRY_RNR_RETRANSMIT,
	RXR_PKT_ENTRY_COPY_BY_READ, /* the pkt entry contains data. A RDMA read has been issued to copy 
				       data to GPU receivingbuffer */
};

/* pkt_entry types for rx pkts */
enum rxr_pkt_entry_type {
	RXR_PKT_ENTRY_POSTED = 1,   /* entries that are posted to the device from the RX bufpool */
	RXR_PKT_ENTRY_UNEXP,        /* entries used to stage unexpected msgs */
	RXR_PKT_ENTRY_OOO,	    /* entries used to stage out-of-order RTM or RTA */
	RXR_PKT_ENTRY_USER	    /* entries backed by user-provided msg prefix (FI_MSG_PREFIX)*/
};

struct rxr_pkt_sendv {
	/* Because core EP current only support 2 iov,
	 * and for the sake of code simplicity, we use 2 iov.
	 * One for header, and the other for data.
	 * iov_count here is used as an indication
	 * of whether iov is used, it is either 0 or 2.
	 */
	int iov_count;
	struct iovec iov[2];
	void *desc[2];
};

struct rxr_pkt_entry {
	/* for rx/tx_entry queued_pkts list */
	struct dlist_entry entry;
#if ENABLE_DEBUG
	/* for tx/rx debug list or posted buf list */
	struct dlist_entry dbg_entry;
#endif
	void *x_entry; /* pointer to rxr rx/tx entry */
	size_t pkt_size;

	struct fid_mr *mr;
	fi_addr_t addr;
	enum rxr_pkt_entry_type type;
	enum rxr_pkt_entry_state state;

	/*
	 * next is used on receiving end.
	 * send is used on sending end.
	 */
	union {
		struct rxr_pkt_entry *next;
		struct rxr_pkt_sendv *send;
	};

#if ENABLE_DEBUG
	/* pad to cache line size of 64 bytes */
	uint8_t pad[48];
#endif
	char pkt[0]; /* rxr_ctrl_*_pkt, or rxr_data_pkt */
};

static inline void *rxr_pkt_start(struct rxr_pkt_entry *pkt_entry)
{
	return pkt_entry->pkt;
}

#if defined(static_assert) && defined(__x86_64__)
#if ENABLE_DEBUG
static_assert(sizeof(struct rxr_pkt_entry) == 128, "rxr_pkt_entry check");
#else
static_assert(sizeof(struct rxr_pkt_entry) == 64, "rxr_pkt_entry check");
#endif
#endif

OFI_DECL_RECVWIN_BUF(struct rxr_pkt_entry*, rxr_robuf, uint32_t);
DECLARE_FREESTACK(struct rxr_robuf, rxr_robuf_fs);

struct rxr_ep;

struct rxr_tx_entry;

struct rxr_pkt_entry *rxr_pkt_entry_init_prefix(struct rxr_ep *ep,
						const struct fi_msg *posted_buf,
						struct ofi_bufpool *pkt_pool);

struct rxr_pkt_entry *rxr_pkt_entry_alloc(struct rxr_ep *ep,
					  struct ofi_bufpool *pkt_pool);

void rxr_pkt_entry_release_tx(struct rxr_ep *ep,
			      struct rxr_pkt_entry *pkt_entry);

void rxr_pkt_entry_release_rx(struct rxr_ep *ep,
			      struct rxr_pkt_entry *pkt_entry);

void rxr_pkt_entry_append(struct rxr_pkt_entry *dst,
			  struct rxr_pkt_entry *src);

struct rxr_pkt_entry *rxr_pkt_entry_clone(struct rxr_ep *ep,
					  struct ofi_bufpool *pkt_pool,
					  struct rxr_pkt_entry *src,
					  int new_entry_type);

struct rxr_pkt_entry *rxr_pkt_get_unexp(struct rxr_ep *ep,
					struct rxr_pkt_entry **pkt_entry_ptr);

ssize_t rxr_pkt_entry_send_with_flags(struct rxr_ep *ep,
				      struct rxr_pkt_entry *pkt_entry,
				      fi_addr_t addr, uint64_t flags);

ssize_t rxr_pkt_entry_sendv(struct rxr_ep *ep,
			    struct rxr_pkt_entry *pkt_entry,
			    fi_addr_t addr, const struct iovec *iov,
			    void **desc, size_t count, uint64_t flags);

ssize_t rxr_pkt_entry_send(struct rxr_ep *ep,
			   struct rxr_pkt_entry *pkt_entry,
			   fi_addr_t addr);

ssize_t rxr_pkt_entry_inject(struct rxr_ep *ep,
			     struct rxr_pkt_entry *pkt_entry,
			     fi_addr_t addr);

struct rxr_pkt_rx_key {
	uint64_t msg_id;
	fi_addr_t addr;
};

struct rxr_pkt_rx_map {
	struct rxr_pkt_rx_key key;
	struct rxr_rx_entry *rx_entry;
	UT_hash_handle hh;
};

struct rxr_rx_entry *rxr_pkt_rx_map_lookup(struct rxr_ep *ep,
					   struct rxr_pkt_entry *pkt_entry);

void rxr_pkt_rx_map_insert(struct rxr_ep *ep,
			   struct rxr_pkt_entry *pkt_entry,
			   struct rxr_rx_entry *rx_entry);

void rxr_pkt_rx_map_remove(struct rxr_ep *pkt_rx_map,
			   struct rxr_pkt_entry *pkt_entry,
			   struct rxr_rx_entry *rx_entry);

#endif
