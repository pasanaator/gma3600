// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include "sgx545_heap_validate.h"

int main(void)
{
	/* First nine records from the D2500HN transcript, followed by the
	 * disabled shared PB and active per-context PB from pinned sgxconfig.h.
	 */
	const struct sgx545_heap32 heaps[] = {
		{0x07000000, 0, 0x10000000, 0xc1fff000, 0x02014200, 0},
		{0x07000001, 0, 0xe2000000, 0x0cfff000, 0x02024200, 0},
		{0x07000002, 12, 0xf2000000, 0x0007f000, 0x02024200, 0},
		{0x07000003, 13, 0xf4000000, 0x04fff000, 0x02024200, 0},
		{0x07000004, 0, 0xf9000000, 0x004ff000, 0x02014200, 0},
		{0x07000005, 0, 0xfe000000, 0x001ff000, 0x02014200, 0},
		{0x07000006, 0, 0xf0000000, 0x01fff000, 0x02014200, 0},
		{0x07000007, 0, 0xf2400000, 0x01bff000, 0x02014200, 0},
		{0x07000008, 14, 0xef000000, 0x00fff000, 0x02024200, 0},
		{0x07000009, 0, 0xd2000000, 0, 0x02024200, 0},
		{0x0700000a, 0, 0xd2000000, 0x0ffff000, 0x02014200, 0},
	};
	struct sgx545_heap32 heap;
	unsigned i;

	for (i = 0; i < sizeof(heaps) / sizeof(heaps[0]); i++)
		assert(sgx545_heap_valid(&heaps[i]));
	heap = heaps[9]; heap.handle = 99;
	assert(!sgx545_heap_valid(&heap)); /* disabled heaps cannot own buffers */
	heap = heaps[0]; heap.base = 0xfffff000; heap.size = 4096;
	assert(sgx545_heap_valid(&heap)); /* end exactly at 4 GiB */
	heap.size++;
	assert(!sgx545_heap_valid(&heap)); /* do not wrap in 32-bit arithmetic */
	heap = (struct sgx545_heap32){.id = SGX545_UNDEFINED_HEAP};
	assert(sgx545_heap_valid(&heap));
	heap.attributes = 1;
	assert(!sgx545_heap_valid(&heap));
	puts("SGX heap validation tests passed: Atom ranges, disabled PB heap, overflow and undefined slots");
	return 0;
}
