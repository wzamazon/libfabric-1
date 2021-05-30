/*
 * Copyright (c) 2016, Cisco Systems, Inc. All rights reserved.
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

#include <malloc.h>
#include <stdio.h>

#include <infiniband/efadv.h>

#include <ofi_enosys.h>
#include "efa.h"
#include "rxr.h"

/*
 * Local/remote peer detection by comparing peer GID with stored local GIDs
 */
static bool efa_is_local_peer(struct efa_av *av, const void *addr)
{
	struct efa_ep_addr *cur_efa_addr = local_efa_addr;

#if ENABLE_DEBUG
	char peer_gid[INET6_ADDRSTRLEN] = { 0 };

	if (!inet_ntop(AF_INET6, ((struct efa_ep_addr *)addr)->raw, peer_gid, INET6_ADDRSTRLEN)) {
		EFA_WARN(FI_LOG_AV, "Failed to get current EFA's GID, errno: %d\n", errno);
		return 0;
	}
	EFA_INFO(FI_LOG_AV, "The peer's GID is %s.\n", peer_gid);
#endif
	while (cur_efa_addr) {
		if (!memcmp(((struct efa_ep_addr *)addr)->raw, cur_efa_addr->raw, 16)) {
			EFA_INFO(FI_LOG_AV, "The peer is local.\n");
			return 1;
		}
		cur_efa_addr = cur_efa_addr->next;
	}

	return 0;
}

static bool efa_is_same_addr(struct efa_ep_addr *lhs, struct efa_ep_addr *rhs)
{
	return !memcmp(lhs->raw, rhs->raw, sizeof(lhs->raw)) &&
	       lhs->qpn == rhs->qpn && lhs->qkey == rhs->qkey;
}

/**
 * @brief initialize a rdm peer
 *
 * @param[in,out]	peer	rdm peer
 * @param[in]		ep	rdm endpoint
 * @param[in]		conn	efa conn object
 */
static inline
void efa_rdm_peer_init(struct rxr_ep *ep, struct rdm_peer *peer, struct efa_conn *conn)
{
	memset(peer, 0, sizeof(struct rdm_peer));

	dlist_init(&peer->rnr_entry);
	dlist_init(&peer->tx_entry_list);
	dlist_init(&peer->rx_entry_list);

	peer->efa_fiaddr = conn->fi_addr;
	peer->is_self = efa_is_same_addr((struct efa_ep_addr *)ep->core_addr,
					 &conn->ep_addr);
}

/**
 * @brief release resources accociated with a peer
 *
 * release reorder buffer, tx_entry list and rx_entry list of a peer
 *
 * @param[in,out]	peer 	rdm peer
 */
void efa_rdm_peer_clear(struct rxr_ep *ep, struct rdm_peer *peer)
{
	struct dlist_entry *tmp;
	struct rxr_tx_entry *tx_entry;
	struct rxr_rx_entry *rx_entry;
	/*
	 * TODO: Add support for wait/signal until all pending messages have
	 * been sent/received so we do not attempt to complete a data transfer
	 * or internal transfer after the EP is shutdown.
	 */
	if ((peer->flags & RXR_PEER_REQ_SENT) &&
	    !(peer->flags & RXR_PEER_HANDSHAKE_RECEIVED))
		FI_WARN_ONCE(&rxr_prov, FI_LOG_EP_CTRL, "Closing EP with unacked CONNREQs in flight\n");

	efa_free_robuf(peer);

	dlist_foreach_container_safe(&peer->tx_entry_list,
				     struct rxr_tx_entry,
				     tx_entry, peer_entry, tmp) {
		rxr_release_tx_entry(ep, tx_entry);
	}

	dlist_foreach_container_safe(&peer->rx_entry_list,
				     struct rxr_rx_entry,
				     rx_entry, peer_entry, tmp) {
		rxr_release_rx_entry(ep, rx_entry);
	}
	
	memset(peer, 0, sizeof(struct rdm_peer));
	dlist_init(&peer->tx_entry_list);
	dlist_init(&peer->rx_entry_list);
}

/**
 * @brief find efa_conn struct using fi_addr
 *
 * @param[in]	av	efa av
 * @param[in]	addr	fi_addr
 * @return	if address is valid, return pointer to efa_conn struct
 * 		otherwise, return NULL
 */
struct efa_conn *efa_av_addr_to_conn(struct efa_av *av, fi_addr_t fi_addr)
{
	struct util_av_entry *util_av_entry;
	struct efa_av_entry *efa_av_entry;

	if (OFI_UNLIKELY(fi_addr == FI_ADDR_UNSPEC))
		return NULL;

	if (av->type == FI_AV_MAP) {
		return (struct efa_conn *)fi_addr;
	}
	
	assert(av->type == FI_AV_TABLE);
	util_av_entry = ofi_bufpool_get_ibuf(av->util_av.av_entry_pool, fi_addr);
	if (!util_av_entry)
		return NULL;

	efa_av_entry = (struct efa_av_entry *)util_av_entry->data;
	return &efa_av_entry->conn;
}

fi_addr_t efa_ahn_qpn_to_addr(struct efa_av *av, uint16_t ahn, uint16_t qpn)
{
	struct efa_reverse_av *reverse_av;
	struct efa_ah_qpn key = {
		.ahn = ahn,
		.qpn = qpn,
	};

	HASH_FIND(hh, av->reverse_av, &key, sizeof(key), reverse_av);

	return OFI_LIKELY(!!reverse_av) ? reverse_av->conn->fi_addr : FI_ADDR_NOTAVAIL;
}

/**
 * @brief find rdm_peer by address handle number (ahn) and QP number (qpn)
 *
 * @param[in]	av	address vector
 * @param[in]	ahn	address handle number
 * @param[in]	qpn	QP number
 * @return	On success, return pointer to rdm_peer
 * 		If no such peer exist, return NULL
 */
struct rdm_peer *efa_ahn_qpn_to_peer(struct efa_av *av, uint16_t ahn, uint16_t qpn)
{
	struct efa_reverse_av *reverse_av;
	struct efa_ah_qpn key = {
		.ahn = ahn,
		.qpn = qpn,
	};

	HASH_FIND(hh, av->reverse_av, &key, sizeof(key), reverse_av);

	return OFI_LIKELY(!!reverse_av) ? &reverse_av->conn->rdm_peer : NULL;
}

static inline int efa_av_is_valid_address(struct efa_ep_addr *addr)
{
	struct efa_ep_addr all_zeros = {};

	return memcmp(addr->raw, all_zeros.raw, sizeof(addr->raw));
}

/**
 * @brief allocate an ibv_ah object from GID.
 * This function use a hash map to store GID to ibv_ah map,
 * and re-use ibv_ah for same GID
 *
 * @param[in]	av	address vector
 * @param[in]	gid	GID
 */
static
struct efa_ah *efa_ah_alloc(struct efa_av *av, const uint8_t *gid)
{
	struct ibv_pd *ibv_pd = av->domain->ibv_pd;
	struct efa_ah *efa_ah;
	struct ibv_ah_attr ibv_ah_attr = { 0 };
	struct efadv_ah_attr efa_ah_attr = { 0 };
	int err;

	HASH_FIND(hh, av->ah_map, gid, EFA_GID_LEN, efa_ah);
	if (efa_ah) {
		efa_ah->used += 1;
		return efa_ah;
	}

	efa_ah = malloc(sizeof(struct efa_ah));
	if (!efa_ah) {
		errno = FI_ENOMEM;
		EFA_WARN(FI_LOG_AV, "cannot allocate memory for efa_ah");
		return NULL;
	}

	ibv_ah_attr.port_num = 1;
	ibv_ah_attr.is_global = 1;
	memcpy(ibv_ah_attr.grh.dgid.raw, gid, EFA_GID_LEN);
	efa_ah->ibv_ah = ibv_create_ah(ibv_pd, &ibv_ah_attr);
	if (!efa_ah->ibv_ah) {
		EFA_WARN(FI_LOG_AV, "ibv_create_ah failed! errno: %d\n", errno);
		goto err_free_efa_ah;
	}

	err = efadv_query_ah(efa_ah->ibv_ah, &efa_ah_attr, sizeof(efa_ah_attr));
	if (err) {
		errno = err;
		EFA_WARN(FI_LOG_AV, "efadv_query_ah failed! err: %d\n", err);
		goto err_destroy_ibv_ah;
	}

	efa_ah->used = 1;
	efa_ah->ahn = efa_ah_attr.ahn;
	memcpy(efa_ah->gid, gid, EFA_GID_LEN);
	HASH_ADD(hh, av->ah_map, gid, EFA_GID_LEN, efa_ah);
	return efa_ah;

err_destroy_ibv_ah:
	ibv_destroy_ah(efa_ah->ibv_ah);
err_free_efa_ah:
	free(efa_ah);
	return NULL;
}

/**
 * @brief release an efa_ah object
 *
 * @param[in]	av	address vector
 * @param[in]	ah	efa_ah object pointer
 */
static
void efa_ah_release(struct efa_av *av, struct efa_ah *ah)
{
	int err;
#if ENABLE_DEBUG
	struct efa_ah *tmp;

	HASH_FIND(hh, av->ah_map, ah->gid, EFA_GID_LEN, tmp);
	assert(tmp == ah);
#endif
	assert(ah->used > 0);
	ah->used -= 1;
	if (ah->used == 0) {
		HASH_DEL(av->ah_map, ah);
		err = ibv_destroy_ah(ah->ibv_ah);
		if (err)
			EFA_WARN(FI_LOG_AV, "ibv_destroy_ah failed! err=%d\n", err);
		free(ah);
	}
}

static
void efa_conn_release(struct efa_av *av, struct efa_conn *conn);

/**
 * @brief initialize the rdm related resources of an efa_conn object
 *
 * This function setup rdm_peer and shm address for an efa_conn.
 * If shm transfer is enabled and the addr comes from local peer,
 *  1. convert addr to format 'gid_qpn', which will be set as shm's ep name later.
 *  2. insert gid_qpn into shm's av
 *  3. store returned fi_addr from shm into the hash table
 *
 * @param[in]	av	address vector
 * @param[in]	conn	efa_conn object
 * @return	On success return 0, otherwise return a negative error code
 */
static
int efa_conn_rdm_init(struct efa_av *av, struct efa_conn *conn)
{
	int err, ret;
	char smr_name[NAME_MAX];
	struct rxr_ep *rxr_ep;
	struct rdm_peer *peer;

	assert(av->ep_type == FI_EP_RDM);

	/* currently multiple EP bind to same av is not supported */
	rxr_ep = container_of(av->util_av.ep_list.next, struct rxr_ep, util_ep.av_entry);

	peer = &conn->rdm_peer;
	efa_rdm_peer_init(rxr_ep, peer, conn);

	/* If peer is local, insert the address into shm provider's av */
	if (rxr_ep->use_shm && efa_is_local_peer(av, &conn->ep_addr)) {
		if (av->shm_used >= rxr_env.shm_av_size) {
			EFA_WARN(FI_LOG_AV,
				 "Max number of shm AV entry (%d) has been reached.\n",
				 rxr_env.shm_av_size);
			return -FI_ENOMEM;
		}

		err = rxr_ep_efa_addr_to_str(&conn->ep_addr, smr_name);
		if (err != FI_SUCCESS) {
			EFA_WARN(FI_LOG_AV,
				 "rxr_ep_efa_addr_to_str() failed! err=%d\n", err);
			return err;
		}

		ret = fi_av_insert(av->shm_rdm_av, smr_name, 1, &peer->shm_fiaddr, 0, NULL);
		if (OFI_UNLIKELY(ret != 1)) {
			EFA_WARN(FI_LOG_AV,
				 "Failed to insert address to shm provider's av: %s\n",
				 fi_strerror(-ret));
			return err;
		}

		EFA_INFO(FI_LOG_AV,
			"Successfully inserted %s to shm provider's av. efa_fiaddr: %ld shm_fiaddr = %ld\n",
			smr_name, conn->fi_addr, peer->shm_fiaddr);

		assert(peer->shm_fiaddr < rxr_env.shm_av_size);
		av->shm_used++;
		av->shm_rdm_addr_map[peer->shm_fiaddr] = conn->fi_addr;
		peer->is_local = 1;
	}

	return 0;
}

/**
 * @brief release the rdm related resources of an efa_conn object
 *
 * this function release the shm av entry and rdm peer;
 *
 * @param[in]	av	address vector
 * @param[in]	conn	efa_conn object
 */
static
void efa_conn_rdm_deinit(struct efa_av *av, struct efa_conn *conn)
{
	int err;
	struct rdm_peer *peer;
	struct rxr_ep *ep;

	assert(av->ep_type == FI_EP_RDM);

	peer = &conn->rdm_peer;
	if (peer->is_local) {
		err = fi_av_remove(av->shm_rdm_av, &peer->shm_fiaddr, 1, 0);
		if (err) {
			EFA_WARN(FI_LOG_AV, "remove address from shm av failed! err=%d\n", err);
		} else {
			av->shm_used--;
			assert(peer->shm_fiaddr < rxr_env.shm_av_size);
			av->shm_rdm_addr_map[peer->shm_fiaddr] = FI_ADDR_UNSPEC;
		}
	}

	ep = container_of(av->util_av.ep_list.next, struct rxr_ep, util_ep.av_entry);
	assert(ep);
	efa_rdm_peer_clear(ep, peer);
}

/**
 * @brief allocate an efa_conn object
 * caller of this function must obtain av->util_av.lock
 *
 * @param[in]	av		efa address vector
 * @param[in]	raw_addr	raw efa address
 * @param[in]	flags		flags application passed to fi_av_insert
 * @param[in]	context		context application passed to fi_av_insert
 * @return	on success, return a pointer to an efa_conn object
 *		otherwise, return NULL. errno will be set to a positive error code.
 */
static
struct efa_conn *efa_conn_alloc(struct efa_av *av, struct efa_ep_addr *raw_addr,
				uint64_t flags, void *context)
{
	struct efa_reverse_av *reverse_av_entry;
	struct util_av_entry *util_av_entry;
	struct efa_av_entry *efa_av_entry;
	struct efa_conn *conn, *prev_conn;
	fi_addr_t util_av_fi_addr;
	struct efa_ah_qpn key;
	int err;

	if (flags & FI_SYNC_ERR)
		memset(context, 0, sizeof(int));

	if (!efa_av_is_valid_address(raw_addr)) {
		EFA_WARN(FI_LOG_AV, "Failed to insert bad addr");
		errno = FI_EINVAL;
		return NULL;
	}

	err = ofi_av_insert_addr(&av->util_av, raw_addr, &util_av_fi_addr);
	if (err) {
		EFA_WARN(FI_LOG_AV, "ofi_av_insert_addr failed! Error message: %s\n",
			 fi_strerror(err));
		return NULL;
	}

	util_av_entry = ofi_bufpool_get_ibuf(av->util_av.av_entry_pool,
					     util_av_fi_addr);
	efa_av_entry = (struct efa_av_entry *)util_av_entry->data;
	conn = &efa_av_entry->conn;

	memcpy(&conn->ep_addr, raw_addr, sizeof(*raw_addr));
	assert(av->type == FI_AV_MAP || av->type == FI_AV_TABLE);
	conn->fi_addr = (av->type == FI_AV_MAP) ? (uintptr_t)(void *)conn : util_av_fi_addr;
	conn->util_av_fi_addr = util_av_fi_addr;

	conn->ah = efa_ah_alloc(av, raw_addr->raw);
	if (!conn->ah)
		goto err_release;

	if (av->ep_type == FI_EP_RDM) {
		err = efa_conn_rdm_init(av, conn);
		if (err)
			goto err_release;
	}

	key.ahn = conn->ah->ahn;
	key.qpn = raw_addr->qpn;
	HASH_FIND(hh, av->reverse_av, &key, sizeof(key), reverse_av_entry);
	if (reverse_av_entry) {
		/*
		 * If we found an existing entry in reverse_av, the peer must
		 * have the same GID and qpn, but different qkey, which means
		 * the old peer has been destroied. so we have to release the
		 * corresponding efa_conn object
		 */
		prev_conn = reverse_av_entry->conn;
		assert(prev_conn);
		assert(memcmp(prev_conn->ep_addr.raw, conn->ep_addr.raw, EFA_GID_LEN)==0);
		assert(prev_conn->ep_addr.qpn == conn->ep_addr.qpn);
		assert(prev_conn->ep_addr.qkey != conn->ep_addr.qkey);
		EFA_WARN(FI_LOG_AV, "QP reuse detected! Prev qkey: %d Prev addr: %d\n",
			 prev_conn->ep_addr.qkey, (int)prev_conn->util_av_fi_addr);
		if (av->ep_type == FI_EP_RDM)
			conn->rdm_peer.prev_qkey = prev_conn->ep_addr.qkey;
		efa_conn_release(av, prev_conn);
	}

	reverse_av_entry = malloc(sizeof(*reverse_av_entry));
	if (!reverse_av_entry) {
		errno = FI_ENOMEM;
		goto err_release;
	}

	memcpy(&reverse_av_entry->key, &key, sizeof(key));
	reverse_av_entry->conn = conn;
	HASH_ADD(hh, av->reverse_av, key,
		 sizeof(reverse_av_entry->key),
		 reverse_av_entry);

	av->used++;
	return conn;

err_release:
	if (reverse_av_entry) {
		HASH_DEL(av->reverse_av, reverse_av_entry);
		free(reverse_av_entry);
	}

	if (conn->ah)
		efa_ah_release(av, conn->ah);

	err = ofi_av_remove_addr(&av->util_av, util_av_fi_addr);
	if (err)
		EFA_WARN(FI_LOG_AV, "While processing previous failure, ofi_av_remove_addr failed! err=%d\n",
			 err);
	return NULL;
}

/**
 * @brief release an efa conn object
 * Caller of this function must obtain av->util_av.lock
 *
 * @param[in]	av	address vector
 * @param[in]	conn	efa_conn object pointer
 */
static
void efa_conn_release(struct efa_av *av, struct efa_conn *conn)
{
	struct efa_reverse_av *reverse_av_entry;
	struct util_av_entry *util_av_entry;
	struct efa_av_entry *efa_av_entry;
	struct efa_ah_qpn key;
	char gidstr[INET6_ADDRSTRLEN];

	if (av->ep_type == FI_EP_RDM)
		efa_conn_rdm_deinit(av, conn);

	key.ahn = conn->ah->ahn;
	key.qpn = conn->ep_addr.qpn;
	HASH_FIND(hh, av->reverse_av, &key, sizeof(key), reverse_av_entry);
	assert(reverse_av_entry);
	HASH_DEL(av->reverse_av, reverse_av_entry);
	free(reverse_av_entry);

	efa_ah_release(av, conn->ah);

	util_av_entry = ofi_bufpool_get_ibuf(av->util_av.av_entry_pool, conn->util_av_fi_addr);
	assert(util_av_entry);
	efa_av_entry = (struct efa_av_entry *)util_av_entry->data;

	ofi_av_remove_addr(&av->util_av, conn->util_av_fi_addr);
	memset(efa_av_entry->ep_addr, 0, EFA_EP_ADDR_LEN);

	inet_ntop(AF_INET6, conn->ep_addr.raw, gidstr, INET6_ADDRSTRLEN);
	EFA_INFO(FI_LOG_AV, "efa_conn released! conn[%p] GID[%s] QP[%u]\n",
		 conn, gidstr, conn->ep_addr.qpn);

	av->used--;
}

/**
 * @brief insert one address into address vector (AV)
 *
 * @param[in]	av	address vector
 * @param[in]	addr	raw address, in the format of gid:qpn:qkey
 * @param[out]	fi_addr pointer the output fi address. This addres is used by fi_send
 * @param[in]	flags	flags user passed to fi_av_insert.
 * @param[in]	context	context user passed to fi_av_insert
 * @return	0 on success, a negative error code on failure
 */
int efa_av_insert_one(struct efa_av *av, struct efa_ep_addr *addr,
		      fi_addr_t *fi_addr, uint64_t flags, void *context)
{
	struct efa_conn *conn;
	char raw_gid_str[INET6_ADDRSTRLEN];
	fi_addr_t efa_fiaddr;
	int ret = 0;

	fastlock_acquire(&av->util_av.lock);
	memset(raw_gid_str, 0, sizeof(raw_gid_str));
	if (!inet_ntop(AF_INET6, addr->raw, raw_gid_str, INET6_ADDRSTRLEN)) {
		EFA_WARN(FI_LOG_AV, "cannot convert address to string. errno: %d", errno);
		ret = -FI_EINVAL;
		*fi_addr = FI_ADDR_NOTAVAIL;
		goto out;
	}

	EFA_INFO(FI_LOG_AV, "Inserting address GID[%s] QP[%u] QKEY[%u] to RDM AV ....\n",
		 raw_gid_str, addr->qpn, addr->qkey);

	/*
	 * Check if this address already has been inserted, if so return that
	 * fi_addr_t.
	 */
	efa_fiaddr = ofi_av_lookup_fi_addr_unsafe(&av->util_av, addr);
	if (efa_fiaddr != FI_ADDR_NOTAVAIL) {
		*fi_addr = efa_fiaddr;
		EFA_INFO(FI_LOG_AV, "Found existing AV entry pointing to this address! fi_addr: %ld\n", *fi_addr);
		ret = 0;
		goto out;
	}

	conn = efa_conn_alloc(av, addr, flags, context);
	if (!conn) {
		*fi_addr = FI_ADDR_NOTAVAIL;
		ret = -FI_EADDRNOTAVAIL;
		goto out;
	}

	*fi_addr = conn->fi_addr;
	EFA_INFO(FI_LOG_AV, "Successfully inserted address GID[%s] QP[%u] QKEY[%u] to RDM AV. fi_addr: %ld\n",
		 raw_gid_str, addr->qpn, addr->qkey, *fi_addr);
	ret = 0;
out:
	fastlock_release(&av->util_av.lock);
	return ret;
}

int efa_av_insert(struct fid_av *av_fid, const void *addr,
			 size_t count, fi_addr_t *fi_addr,
			 uint64_t flags, void *context)
{
	struct efa_av *av = container_of(av_fid, struct efa_av, util_av.av_fid);
	int ret = 0, success_cnt = 0;
	size_t i = 0;
	struct efa_ep_addr *addr_i;
	fi_addr_t fi_addr_res;

	if (av->util_av.flags & FI_EVENT)
		return -FI_ENOEQ;

	if ((flags & FI_SYNC_ERR) && (!context || (flags & FI_EVENT)))
		return -FI_EINVAL;

	/*
	 * Providers are allowed to ignore FI_MORE.
	 */
	flags &= ~FI_MORE;
	if (flags)
		return -FI_ENOSYS;

	for (i = 0; i < count; i++) {
		addr_i = (struct efa_ep_addr *) ((uint8_t *)addr + i * EFA_EP_ADDR_LEN);

		ret = efa_av_insert_one(av, addr_i, &fi_addr_res, flags, context);
		if (ret) {
			EFA_WARN(FI_LOG_AV, "insert raw_addr to av failed! ret=%d\n",
				 ret);
			break;
		}

		if (fi_addr)
			fi_addr[i] = fi_addr_res;
		success_cnt++;
	}

	/* cancel remaining request and log to event queue */
	for (; i < count ; i++) {
		if (av->util_av.eq)
			ofi_av_write_event(&av->util_av, i, FI_ECANCELED,
					context);
		if (fi_addr)
			fi_addr[i] = FI_ADDR_NOTAVAIL;
	}

	/* update success to event queue */
	if (av->util_av.eq)
		ofi_av_write_event(&av->util_av, success_cnt, 0, context);

	return success_cnt;
}

static int efa_av_lookup(struct fid_av *av_fid, fi_addr_t fi_addr,
			 void *addr, size_t *addrlen)
{
	struct efa_av *av = container_of(av_fid, struct efa_av, util_av.av_fid);
	struct efa_conn *conn = NULL;

	if (av->type != FI_AV_MAP && av->type != FI_AV_TABLE)
		return -FI_EINVAL;

	if (fi_addr == FI_ADDR_NOTAVAIL)
		return -FI_EINVAL;

	conn = efa_av_addr_to_conn(av, fi_addr);
	if (!conn)
		return -FI_EINVAL;

	memcpy(addr, (void *)&conn->ep_addr, MIN(sizeof(conn->ep_addr), *addrlen));
	if (*addrlen > sizeof(conn->ep_addr))
		*addrlen = sizeof(conn->ep_addr);
	return 0;
}

static int efa_av_remove(struct fid_av *av_fid, fi_addr_t *fi_addr,
			 size_t count, uint64_t flags)
{
	int err = 0;
	size_t i;
	struct efa_av *av;
	struct efa_conn *conn;

	if (!fi_addr)
		return -FI_EINVAL;

	av = container_of(av_fid, struct efa_av, util_av.av_fid);
	if (av->type != FI_AV_MAP && av->type != FI_AV_TABLE)
		return -FI_EINVAL;

	fastlock_acquire(&av->util_av.lock);
	for (i = 0; i < count; i++) {
		conn = efa_av_addr_to_conn(av, fi_addr[i]);
		if (!conn) {
			err = -FI_EINVAL;
			break;
		}

		if (av->ep_type == FI_EP_RDM && efa_peer_in_use(&conn->rdm_peer)) {
			err = -FI_EBUSY;
			break;
		}

		efa_conn_release(av, conn);
	}

	if (i < count) {
		/* something went wrong, so err cannot be zero */
		assert(err);
		if (av->util_av.eq) {
			for (; i < count; ++i)
				ofi_av_write_event(&av->util_av, i, FI_ECANCELED, NULL);
		}
	}

	fastlock_release(&av->util_av.lock);
	return err;
}

static const char *efa_av_straddr(struct fid_av *av_fid, const void *addr,
				  char *buf, size_t *len)
{
	return ofi_straddr(buf, len, FI_ADDR_EFA, addr);
}

static struct fi_ops_av efa_av_ops = {
	.size = sizeof(struct fi_ops_av),
	.insert = efa_av_insert,
	.insertsvc = fi_no_av_insertsvc,
	.insertsym = fi_no_av_insertsym,
	.remove = efa_av_remove,
	.lookup = efa_av_lookup,
	.straddr = efa_av_straddr
};

static void efa_av_close_reverse_av(struct efa_av *av)
{
	struct efa_reverse_av *reverse_av_entry, *tmp;

	fastlock_acquire(&av->util_av.lock);

	HASH_ITER(hh, av->reverse_av, reverse_av_entry, tmp) {
		efa_conn_release(av, reverse_av_entry->conn);
	}

	fastlock_release(&av->util_av.lock);
}

static int efa_av_close(struct fid *fid)
{
	struct efa_av *av;
	int ret = 0;
	int err = 0;

	av = container_of(fid, struct efa_av, util_av.av_fid.fid);

	efa_av_close_reverse_av(av);

	if (av->ep_type == FI_EP_RDM) {
		if (rxr_env.enable_shm_transfer && av->shm_rdm_av &&
		    &av->shm_rdm_av->fid) {
			ret = fi_close(&av->shm_rdm_av->fid);
			if (ret) {
				err = ret;
				EFA_WARN(FI_LOG_AV, "Failed to close shm av: %s\n",
					fi_strerror(ret));
			}
		}
		ret = ofi_av_close(&av->util_av);
		if (ret) {
			err = ret;
			EFA_WARN(FI_LOG_AV, "Failed to close av: %s\n",
				fi_strerror(ret));
		}
	}
	free(av);
	return err;
}

static int efa_av_bind(struct fid *fid, struct fid *bfid, uint64_t flags)
{
	return ofi_av_bind(fid, bfid, flags);
}

static struct fi_ops efa_av_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = efa_av_close,
	.bind = efa_av_bind,
	.control = fi_no_control,
	.ops_open = fi_no_ops_open,
};

int efa_av_open(struct fid_domain *domain_fid, struct fi_av_attr *attr,
		struct fid_av **av_fid, void *context)
{
	struct efa_domain *efa_domain;
	struct util_domain *util_domain;
	struct rxr_domain *rxr_domain;
	struct efa_domain_base *efa_domain_base;
	struct efa_av *av;
	struct util_av_attr util_attr;
	size_t universe_size;
	struct fi_av_attr av_attr;
	int i, ret, retv;

	if (!attr)
		return -FI_EINVAL;

	if (attr->name)
		return -FI_ENOSYS;

	/* FI_EVENT, FI_READ, and FI_SYMMETRIC are not supported */
	if (attr->flags)
		return -FI_ENOSYS;

	/*
	 * TODO: remove me once RxR supports resizing members tied to the AV
	 * size.
	 */
	if (!attr->count)
		attr->count = EFA_MIN_AV_SIZE;
	else
		attr->count = MAX(attr->count, EFA_MIN_AV_SIZE);

	av = calloc(1, sizeof(*av));
	if (!av)
		return -FI_ENOMEM;

	util_domain = container_of(domain_fid, struct util_domain,
				   domain_fid);
	efa_domain_base = container_of(util_domain, struct efa_domain_base,
				       util_domain.domain_fid);
	attr->type = FI_AV_TABLE;

	/*
	 * An rxr_domain fid was passed to the user if this is an RDM
	 * endpoint, otherwise it is an efa_domain fid.  This will be
	 * removed once the rxr and efa domain structures are combined.
	 */
	if (efa_domain_base->type == EFA_DOMAIN_RDM) {
		rxr_domain = (struct rxr_domain *)efa_domain_base;
		efa_domain = container_of(rxr_domain->rdm_domain, struct efa_domain,
						util_domain.domain_fid);
		av->ep_type = FI_EP_RDM;

		if (fi_param_get_size_t(NULL, "universe_size",
					&universe_size) == FI_SUCCESS)
			attr->count = MAX(attr->count, universe_size);

		util_attr.addrlen = EFA_EP_ADDR_LEN;
		util_attr.context_len = sizeof(struct efa_av_entry) - EFA_EP_ADDR_LEN;
		util_attr.flags = 0;
		ret = ofi_av_init(&efa_domain->util_domain, attr, &util_attr,
					&av->util_av, context);
		if (ret)
			goto err;
		av_attr = *attr;
		if (rxr_env.enable_shm_transfer) {
			/*
			 * shm av supports maximum 256 entries
			 * Reset the count to 128 to reduce memory footprint and satisfy
			 * the need of the instances with more CPUs.
			 */
			if (rxr_env.shm_av_size > EFA_SHM_MAX_AV_COUNT) {
				ret = -FI_ENOSYS;
				EFA_WARN(FI_LOG_AV, "The requested av size is beyond"
					 " shm supported maximum av size: %s\n",
					 fi_strerror(-ret));
				goto err_close_util_av;
			}
			av_attr.count = rxr_env.shm_av_size;
			assert(av_attr.type == FI_AV_TABLE);
			ret = fi_av_open(efa_domain->shm_domain, &av_attr,
					&av->shm_rdm_av, context);
			if (ret)
				goto err_close_util_av;

			for (i = 0; i < EFA_SHM_MAX_AV_COUNT; ++i)
				av->shm_rdm_addr_map[i] = FI_ADDR_UNSPEC;
		}
	} else {
		efa_domain = (struct efa_domain *)efa_domain_base;
		av->ep_type = FI_EP_DGRAM;
	}

	EFA_INFO(FI_LOG_AV, "fi_av_attr:%" PRId64 "\n",
			av_attr.flags);

	av->domain = efa_domain;
	av->type = attr->type;
	av->used = 0;
	av->shm_used = 0;

	*av_fid = &av->util_av.av_fid;
	(*av_fid)->fid.fclass = FI_CLASS_AV;
	(*av_fid)->fid.context = context;
	(*av_fid)->fid.ops = &efa_av_fi_ops;
	(*av_fid)->ops = &efa_av_ops;

	return 0;

err_close_util_av:
	retv = ofi_av_close(&av->util_av);
	if (retv)
		EFA_WARN(FI_LOG_AV,
			 "Unable to close util_av: %s\n", fi_strerror(-retv));
err:
	free(av);
	return ret;
}
