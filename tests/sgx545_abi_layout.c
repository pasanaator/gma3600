/* SPDX-License-Identifier: GPL-2.0-only */
#include <stddef.h>
#include "sgx545_abi.h"

_Static_assert(sizeof(struct sgx545_bridge32) == 28, "i386 envelope size");
_Static_assert(offsetof(struct sgx545_bridge32, param_in) == 8, "input pointer");
_Static_assert(offsetof(struct sgx545_bridge32, in_size) == 12, "1.7 input size");
_Static_assert(offsetof(struct sgx545_bridge32, param_out) == 16, "1.7 output pointer");
_Static_assert(offsetof(struct sgx545_bridge32, services) == 24, "services handle");
