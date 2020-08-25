#ifndef EFA_GDRCOPY_H
#define EFA_GDRCOPY_H

#include <gdrapi.h>

struct efa_mr;

struct efa_gdrcopy_info {
	gdr_mh_t mh; /* memory handler */
	void *cuda_ptr; /* page aligned gpu pointer */
	void *user_ptr; /* user space ptr mapped to GPU memory */
	size_t length; /* page aligned length */
};

ssize_t efa_gdrcopy_reg(void *addr, size_t len, struct efa_mr *efa_mr);

ssize_t efa_gdrcopy_dereg(struct efa_mr *efa_mr);

void efa_gdrcopy_to_device(struct efa_mr *efa_mr, void *devptr, void *hostptr, size_t len);


#endif
