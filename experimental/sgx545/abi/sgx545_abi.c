// SPDX-License-Identifier: GPL-2.0-only
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif
#include "sgx545_abi.h"

static int sgx545_user_range32(__u32 address, __u32 length)
{
	if (!length)
		return 1;
	return address && (__u64)address + length <= (1ULL << 32);
}

int sgx545_bridge32_decode(const struct sgx545_bridge32 *wire,
			 unsigned int copied_size,
			 struct sgx545_bridge_request *request)
{
	if (!request)
		return -EINVAL;
	memset(request, 0, sizeof(*request));
	if (!wire || copied_size != SGX545_BRIDGE32_SIZE ||
	    wire->size != SGX545_BRIDGE32_SIZE)
		return -EINVAL;
	if (wire->in_size > SGX545_BRIDGE_BUFFER_MAX ||
	    wire->out_size > SGX545_BRIDGE_BUFFER_MAX)
		return -E2BIG;
	if (!sgx545_user_range32(wire->param_in, wire->in_size) ||
	    !sgx545_user_range32(wire->param_out, wire->out_size))
		return -EFAULT;

	request->param_in = wire->param_in;
	request->param_out = wire->param_out;
	request->bridge_id = wire->bridge_id;
	request->in_size = wire->in_size;
	request->out_size = wire->out_size;
	request->services = wire->services;
	return 0;
}
