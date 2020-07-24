/*
 * (C) Copyright 2020 Hewlett Packard Enterprise Development LP
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

#ifndef _OFI_HMEM_H_
#define _OFI_HMEM_H_

#if HAVE_CONFIG_H
#include <config.h>
#endif

#include <rdma/fi_domain.h>
#include <stdbool.h>

#ifdef HAVE_LIBCUDA

#include <cuda.h>
#include <cuda_runtime.h>

/* Libfabric supported CUDA operations. */
cudaError_t ofi_cudaMemcpy(void* dst, const void* src, size_t count,
			   enum cudaMemcpyKind kind);
/* launch a kernel to copy data from device to device */
void ofi_cudaKernelMemcpy(void* dst, const void* src, size_t count, cudaStream_t stream);

const char *ofi_cudaGetErrorName(cudaError_t error);
const char *ofi_cudaGetErrorString(cudaError_t error);
CUresult ofi_cuPointerGetAttribute(void *data, CUpointer_attribute attribute,
				   CUdeviceptr ptr);

#endif /* HAVE_LIBCUDA */

#ifdef HAVE_ROCR

#include <hsa/hsa_ext_amd.h>

/* Libfabric support ROCr operations. */

hsa_status_t ofi_hsa_memory_copy(void *dst, const void *src, size_t size);
hsa_status_t ofi_hsa_amd_pointer_info(void *ptr, hsa_amd_pointer_info_t *info,
				      void *(*alloc)(size_t),
				      uint32_t *num_agents_accessible,
				      hsa_agent_t **accessible);
hsa_status_t ofi_hsa_init(void);
hsa_status_t ofi_hsa_shut_down(void);
hsa_status_t ofi_hsa_status_string(hsa_status_t status,
				   const char **status_string);
const char *ofi_hsa_status_to_string(hsa_status_t status);

#endif /* HAVE_ROCR */

int rocr_memcpy(void *dest, const void *src, size_t size);
int rocr_hmem_init(void);
int rocr_hmem_cleanup(void);
bool rocr_is_addr_valid(const void *addr);

int cuda_copy_to_dev(void *dev, const void *host, size_t size);
int cuda_copy_from_dev(void *host, const void *dev, size_t size);
int cuda_hmem_init(void);
int cuda_hmem_cleanup(void);
bool cuda_is_addr_valid(const void *addr);

static inline int ofi_memcpy(void *dest, const void *src, size_t size)
{
	memcpy(dest, src, size);
	return FI_SUCCESS;
}

static inline int ofi_hmem_init_noop(void)
{
	return FI_SUCCESS;
}

static inline int ofi_hmem_cleanup_noop(void)
{
	return FI_SUCCESS;
}

ssize_t ofi_copy_from_hmem_iov(void *dest, size_t size,
			       const struct iovec *hmem_iov,
			       enum fi_hmem_iface hmem_iface,
			       size_t hmem_iov_count, uint64_t hmem_iov_offset);

ssize_t ofi_copy_to_hmem_iov(const struct iovec *hmem_iov,
			     enum fi_hmem_iface hmem_iface,
			     size_t hmem_iov_count, uint64_t hmem_iov_offset,
			     void *src, size_t size);

void ofi_hmem_init(void);
void ofi_hmem_cleanup(void);
enum fi_hmem_iface ofi_get_hmem_iface(const void *addr);

#endif /* _OFI_HMEM_H_ */
