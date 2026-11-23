/* SPDX-License-Identifier: GPL-2.0-only */
#include <stddef.h>
#include "sgx545_abi.h"

_Static_assert(sizeof(struct sgx545_bridge32) == 28, "i386 envelope size");
_Static_assert(offsetof(struct sgx545_bridge32, param_in) == 8, "input pointer");
_Static_assert(offsetof(struct sgx545_bridge32, in_size) == 12, "1.7 input size");
_Static_assert(offsetof(struct sgx545_bridge32, param_out) == 16, "1.7 output pointer");
_Static_assert(offsetof(struct sgx545_bridge32, services) == 24, "services handle");
_Static_assert(sizeof(struct sgx545_connect_out) == 12, "i386 CONNECT output");
_Static_assert(sizeof(struct sgx545_enumerate_out) == 200, "i386 ENUM output");
_Static_assert(sizeof(struct sgx545_handle_out) == 8, "i386 handle output");
_Static_assert(sizeof(struct sgx545_heap32) == 24, "i386 heap layout");
_Static_assert(sizeof(struct sgx545_init_info_out) == 776, "i386 initializer info");
