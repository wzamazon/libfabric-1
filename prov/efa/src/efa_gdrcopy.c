#include <gdrapi.h>
#include "efa.h"

ssize_t efa_gdrcopy_reg(void *addr, size_t len, struct efa_mr *efa_mr)
{
	ssize_t err;
	uintptr_t regbgn = (uintptr_t)addr;
	uintptr_t regend = (uintptr_t)addr + len;
	size_t reglen = regend - regbgn;

	regbgn = regbgn & GPU_PAGE_MASK;
	regend = (regend & GPU_PAGE_MASK) + GPU_PAGE_SIZE;

	assert(efa_mr->domain->gdr);
	err = gdr_pin_buffer(efa_mr->domain->gdr, (CUdeviceptr)regbgn,
			     reglen, 0, 0, &efa_mr->gdrcopy.mh);
	if (err) {
		EFA_WARN(FI_LOG_MR, "gdr_pin_buffer failed! err=%ld", err);
		return err;
	}

	efa_mr->gdrcopy.cuda_ptr = (void *)regbgn;
	efa_mr->gdrcopy.length = reglen;

	err = gdr_map(efa_mr->domain->gdr, efa_mr->gdrcopy.mh,
		      &efa_mr->gdrcopy.user_ptr, efa_mr->gdrcopy.length);
	if (err) {
		EFA_WARN(FI_LOG_MR, "gdr_map failed! err=%ld", err);
		gdr_unpin_buffer(efa_mr->domain->gdr, efa_mr->gdrcopy.mh);
		return err;
	}

	return 0;
}

ssize_t efa_gdrcopy_dereg(struct efa_mr *efa_mr)
{
	ssize_t err;

	err = gdr_unmap(efa_mr->domain->gdr, efa_mr->gdrcopy.mh,
			efa_mr->gdrcopy.user_ptr, efa_mr->gdrcopy.length);
	if (err) {
		EFA_WARN(FI_LOG_MR, "gdr_unmap failed! err=%ld\n", err);
		return err;
	}

	err = gdr_unpin_buffer(efa_mr->domain->gdr, efa_mr->gdrcopy.mh);
	if (err) {
		EFA_WARN(FI_LOG_MR, "gdr_unmap failed! err=%ld\n", err);
		return err;
	}

	return 0;
}

void efa_gdrcopy_to_device(struct efa_mr *efa_mr, void *devptr, void *hostptr, size_t len)
{
	ssize_t off;
	void *gdrcopy_user_ptr;

	off = (char *)devptr - (char *)efa_mr->gdrcopy.cuda_ptr;
	assert(off >= 0 && off + len <= efa_mr->gdrcopy.length);
	gdrcopy_user_ptr = (char *)efa_mr->gdrcopy.user_ptr + off;
	gdr_copy_to_mapping(efa_mr->gdrcopy.mh, gdrcopy_user_ptr, hostptr, len);
}

