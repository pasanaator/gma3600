/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SGX545_HEAP_VALIDATE_H
#define SGX545_HEAP_VALIDATE_H
#include "sgx545_abi.h"

static inline int sgx545_heap_valid(const struct sgx545_heap32 *heap)
{
	if (heap->id == SGX545_UNDEFINED_HEAP)
		return !(heap->handle | heap->base | heap->size |
			 heap->attributes | heap->tile_stride);
	/* The DDK retains disabled parameter-buffer heaps with size zero.
	 * They have a valid heap ID/base/attributes, but no allocated handle.
	 */
	if (!heap->size)
		return !heap->handle;
	return (__u64)heap->base + heap->size <= (1ULL << 32);
}
#endif
