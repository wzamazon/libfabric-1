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

#if HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include "ofi_hmem.h"
#include "ofi.h"

#ifdef HAVE_LIBCUDA

#include <cuda.h>
#include <cuda_runtime.h>

struct cuda_ops {
	cudaError_t (*cudaMemcpy)(void *dst, const void *src, size_t count,
				  enum cudaMemcpyKind kind);
	const char *(*cudaGetErrorName)(cudaError_t error);
	const char *(*cudaGetErrorString)(cudaError_t error);
	CUresult (*cuPointerGetAttribute)(void *data,
					  CUpointer_attribute attribute,
					  CUdeviceptr ptr);
};

#ifdef ENABLE_CUDA_DLOPEN

#include <dlfcn.h>

static void *cudart_handle;
static void *cuda_handle;
static struct cuda_ops cuda_ops;

#else

static struct cuda_ops cuda_ops = {
	.cudaMemcpy = cudaMemcpy,
	.cudaGetErrorName = cudaGetErrorName,
	.cudaGetErrorString = cudaGetErrorString,
	.cuPointerGetAttribute = cuPointerGetAttribute,
};

#endif /* ENABLE_CUDA_DLOPEN */

cudaError_t ofi_cudaMemcpy(void *dst, const void *src, size_t count,
			   enum cudaMemcpyKind kind)
{
	return cuda_ops.cudaMemcpy(dst, src, count, kind);
}

const char *ofi_cudaGetErrorName(cudaError_t error)
{
	return cuda_ops.cudaGetErrorName(error);
}

const char *ofi_cudaGetErrorString(cudaError_t error)
{
	return cuda_ops.cudaGetErrorString(error);
}

CUresult ofi_cuPointerGetAttribute(void *data, CUpointer_attribute attribute,
				   CUdeviceptr ptr)
{
	return cuda_ops.cuPointerGetAttribute(data, attribute, ptr);
}

int cuda_copy_to_dev(void *dev, const void *host, size_t size)
{
	cudaError_t cuda_ret;
	cuda_ret = ofi_cudaMemcpy(dev, host, size, cudaMemcpyHostToDevice);

	if (cuda_ret != cudaSuccess) {
		FI_WARN(&core_prov, FI_LOG_CORE,
			"Failed to perform cudaMemcpy: %s:%s\n",
			ofi_cudaGetErrorName(cuda_ret),
			ofi_cudaGetErrorString(cuda_ret));

		return -FI_EIO;
	}

	return 0;
}

int cuda_copy_from_dev(void *host, const void *dev, size_t size)
{
	cudaError_t cuda_ret;

	cuda_ret = ofi_cudaMemcpy(host, dev, size, cudaMemcpyDeviceToHost);
	if (cuda_ret == cudaSuccess)
		return 0;

	FI_WARN(&core_prov, FI_LOG_CORE,
		"Failed to perform cudaMemcpy: %s:%s\n",
		ofi_cudaGetErrorName(cuda_ret),
		ofi_cudaGetErrorString(cuda_ret));

	return -FI_EIO;
}

int cuda_hmem_init(void)
{
#ifdef ENABLE_CUDA_DLOPEN
	cudart_handle = dlopen("libcudart.so", RTLD_NOW);
	if (!cudart_handle) {
		FI_WARN(&core_prov, FI_LOG_CORE,
			"Failed to dlopen libcudart.so\n");
		goto err;
	}

	cuda_handle = dlopen("libcuda.so", RTLD_NOW);
	if (!cuda_handle) {
		FI_WARN(&core_prov, FI_LOG_CORE,
			"Failed to dlopen libcuda.so\n");
		goto err_dlclose_cudart;
	}

	cuda_ops.cudaMemcpy = dlsym(cudart_handle, "cudaMemcpy");
	if (!cuda_ops.cudaMemcpy) {
		FI_WARN(&core_prov, FI_LOG_CORE, "Failed to find cudaMemcpy\n");
		goto err_dlclose_cuda;
	}

	cuda_ops.cudaGetErrorName = dlsym(cudart_handle, "cudaGetErrorName");
	if (!cuda_ops.cudaGetErrorName) {
		FI_WARN(&core_prov, FI_LOG_CORE,
			"Failed to find cudaGetErrorName\n");
		goto err_dlclose_cuda;
	}

	cuda_ops.cudaGetErrorString = dlsym(cudart_handle,
					    "cudaGetErrorString");
	if (!cuda_ops.cudaGetErrorString) {
		FI_WARN(&core_prov, FI_LOG_CORE,
			"Failed to find cudaGetErrorString\n");
		goto err_dlclose_cuda;
	}

	cuda_ops.cuPointerGetAttribute = dlsym(cuda_handle,
					       "cuPointerGetAttribute");
	if (!cuda_ops.cuPointerGetAttribute) {
		FI_WARN(&core_prov, FI_LOG_CORE,
			"Failed to find cuPointerGetAttribute\n");
		goto err_dlclose_cuda;
	}

	return FI_SUCCESS;

err_dlclose_cuda:
	dlclose(cuda_handle);
err_dlclose_cudart:
	dlclose(cudart_handle);
err:
	return -FI_ENODATA;
#else
	return FI_SUCCESS;
#endif /* ENABLE_CUDA_DLOPEN */
}

int cuda_hmem_cleanup(void)
{
#ifdef ENABLE_CUDA_DLOPEN
	dlclose(cuda_handle);
	dlclose(cudart_handle);
#endif

	return FI_SUCCESS;
}

bool cuda_is_addr_valid(const void *addr)
{
	CUresult cuda_ret;
	unsigned int data;

	cuda_ret = ofi_cuPointerGetAttribute(&data,
					     CU_POINTER_ATTRIBUTE_MEMORY_TYPE,
					     (CUdeviceptr)addr);
	switch (cuda_ret) {
	case CUDA_SUCCESS:
		if (data == CU_MEMORYTYPE_DEVICE)
			return true;
		break;

	/* Returned if the buffer is not associated with the CUcontext support
	 * unified virtual addressing. Since host buffers may fall into this
	 * category, this is not treated as an error.
	 */
	case CUDA_ERROR_INVALID_VALUE:
		break;

	/* Returned if cuInit() has not been called. This can happen if support
	 * for CUDA is enabled but the user has not made a CUDA call. This is
	 * not treated as an error.
	 */
	case CUDA_ERROR_NOT_INITIALIZED:
		break;

	/* Returned if the CUcontext does not support unified virtual
	 * addressing.
	 */
	case CUDA_ERROR_INVALID_CONTEXT:
		FI_WARN(&core_prov, FI_LOG_CORE,
			"CUcontext does not support unified virtual addressining\n");
		break;

	default:
		FI_WARN(&core_prov, FI_LOG_CORE,
			"Unhandle cuPointerGetAttribute return code: ret=%d\n",
			cuda_ret);
		break;
	}

	return false;
}

#else

int cuda_copy_to_dev(void *dev, const void *host, size_t size)
{
	return -FI_ENOSYS;
}

int cuda_copy_from_dev(void *host, const void *dev, size_t size)
{
	return -FI_ENOSYS;
}

int cuda_hmem_init(void)
{
	return -FI_ENOSYS;
}

int cuda_hmem_cleanup(void)
{
	return -FI_ENOSYS;
}

bool cuda_is_addr_valid(const void *addr)
{
	return false;
}

#endif /* HAVE_LIBCUDA */
