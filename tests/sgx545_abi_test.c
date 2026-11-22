// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "sgx545_abi.h"

static void check_zero(const struct sgx545_bridge_request *request)
{
	struct sgx545_bridge_request zero = {0};
	assert(!memcmp(request, &zero, sizeof(zero)));
}

int main(void)
{
	/* Byte order/field order fixture, not constructed through the C struct. */
	const unsigned char bytes[28] = {
		0x01,0x02,0x03,0x04, 28,0,0,0, 0x00,0x10,0x00,0xf0,
		8,0,0,0, 0x00,0x20,0x00,0xe0, 16,0,0,0, 0xef,0xbe,0xad,0xde
	};
	struct sgx545_bridge32 wire;
	struct sgx545_bridge_request request;
	memcpy(&wire, bytes, sizeof(wire));
	assert(!sgx545_bridge32_decode(&wire, sizeof(wire), &request));
	assert(request.bridge_id == 0x04030201);
	assert(request.param_in == 0xf0001000ULL); /* No sign extension. */
	assert(request.param_out == 0xe0002000ULL);
	assert(request.in_size == 8 && request.out_size == 16);
	assert(request.services == 0xdeadbeef);
	assert(sgx545_bridge32_decode(&wire, 27, &request) == -EINVAL);
	check_zero(&request);
	wire.size = 48;
	assert(sgx545_bridge32_decode(&wire, 28, &request) == -EINVAL);
	wire.size = 28;
	wire.in_size = 4097;
	assert(sgx545_bridge32_decode(&wire, 28, &request) == -E2BIG);
	wire.in_size = 2;
	wire.param_in = 0xffffffff;
	assert(sgx545_bridge32_decode(&wire, 28, &request) == -EFAULT);
	check_zero(&request);
	wire.in_size = 1; /* Final address in the 32-bit space is valid. */
	assert(!sgx545_bridge32_decode(&wire, 28, &request));
	wire.param_out = 0;
	assert(sgx545_bridge32_decode(&wire, 28, &request) == -EFAULT);
	wire.out_size = 0;
	assert(!sgx545_bridge32_decode(&wire, 28, &request));
	assert(sgx545_bridge32_decode(NULL, 28, &request) == -EINVAL);
	check_zero(&request);
	assert(sgx545_bridge32_decode(&wire, 28, NULL) == -EINVAL);
	puts("SGX545 i386 bridge envelope tests passed");
	return 0;
}
