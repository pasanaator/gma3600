/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GMA_SGX_MEMORY_TEST_STUBS_H
#define GMA_SGX_MEMORY_TEST_STUBS_H
#include "memory_stubs.h"
#include <limits.h>
#include <stdatomic.h>
typedef atomic_long atomic_long_t;
#define ATOMIC_LONG_INIT(value) (value)
#define atomic_long_read(value) atomic_load(value)
#define atomic_long_inc(value) ((void)atomic_fetch_add(value, 1))
#define atomic_long_dec(value) ((void)atomic_fetch_sub(value, 1))
int set_pages_array_uc(struct page **pages, int n);
#endif
