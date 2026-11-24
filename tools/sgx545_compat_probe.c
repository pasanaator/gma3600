// SPDX-License-Identifier: GPL-2.0-only
/* Genuine i386 Linux client, without libc or proprietary userspace libraries.
 * Build with tools/sgx545_compat_probe_build.sh. Only the diagnostic subset runs.
 */
#define SGX545_FREESTANDING
#include "sgx545_abi.h"
#include "sgx545_heap_validate.h"

_Static_assert(sizeof(void *) == 4, "Build this client with -m32");
_Static_assert(sizeof(struct sgx545_bridge32) == 28, "i386 envelope");
_Static_assert(sizeof(struct sgx545_connect_out) == 12, "i386 CONNECT");
_Static_assert(sizeof(struct sgx545_enumerate_out) == 200, "i386 ENUMERATE");
_Static_assert(sizeof(struct sgx545_handle_out) == 8, "i386 ACQUIRE/DISCONNECT");
_Static_assert(sizeof(struct sgx545_heap32) == 24, "i386 heap");
_Static_assert(sizeof(struct sgx545_init_info_out) == 776, "i386 initializer info");
_Static_assert(sizeof(struct sgx545_client_mem32) == 48, "i386 client memory");
_Static_assert(sizeof(struct sgx545_alloc_shared_out) == 56, "i386 shared allocation");
_Static_assert(sizeof(struct sgx545_map_info_out) == 20, "i386 mmap metadata");
_Static_assert(sizeof(struct sgx545_alloc_device_out) == 76, "i386 device allocation");
_Static_assert(sizeof(struct sgx545_device_check) == 24, "device diagnostic message");
_Static_assert(sizeof(struct sgx545_mmu_check) == 40, "MMU diagnostic message");

_Static_assert(sizeof(struct sgx545_init2_in32) == 2068, "Intel initializer wire model");
static int original_intel_abi;

/* mmap2 needs all six i386 syscall registers, including saved EBP. */
extern unsigned probe_mmap2(unsigned fd, unsigned size, unsigned prot, unsigned flags, unsigned offset);

static int sys3(unsigned nr, unsigned a, unsigned b, unsigned c)
{
	int result;
	__asm__ volatile("int $0x80" : "=a"(result) : "0"(nr), "b"(a), "c"(b), "d"(c)
			 : "memory", "cc");
	return result;
}
static unsigned ptr(const void *p) { return (unsigned)p; }
static void say(const char *s)
{
	unsigned n = 0;
	while (s[n]) n++;
	sys3(4, 1, ptr(s), n);
}
static void number(int value)
{
	char buffer[12];
	unsigned n = 0, v = value < 0 ? 0U - (unsigned)value : (unsigned)value;
	do { buffer[n++] = '0' + v % 10; v /= 10; } while (v);
	if (value < 0) buffer[n++] = '-';
	while (n) sys3(4, 1, ptr(&buffer[--n]), 1);
}
static void hex(unsigned value)
{
	char buffer[8];
	unsigned i;
	for (i = 0; i < 8; i++) buffer[i] = "0123456789abcdef"[(value >> (28 - 4 * i)) & 15];
	sys3(4, 1, ptr(buffer), 8);
}
static void fail(const char *label, int value)
{
	say("FAIL: "); say(label); say(" (result="); number(value); say(")\n");
	sys3(1, 1, 0, 0);
	__builtin_unreachable();
}
static void expect(const char *label, int result, int wanted)
{
	if (result != wanted) fail(label, result);
}
static int same(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return *a == *b;
}
static struct sgx545_bridge32 package(unsigned cmd, unsigned services,
	const void *in, unsigned in_size, void *out, unsigned out_size)
{
	return (struct sgx545_bridge32){ SGX545_COMMAND(cmd), 28, ptr(in), in_size,
		ptr(out), out_size, services };
}
static int call(int fd, struct sgx545_bridge32 *wire)
{
	return sys3(54, fd, SGX545_IOCTL_SERVICES, ptr(wire));
}
static int open_bridge(const char *path)
{
	int fd = sys3(5, ptr(path), 2 | 0x80000, 0);
	if (fd < 0) fail("open diagnostic bridge (load prepare=1 bridge=1 first)", fd);
	return fd;
}
static unsigned connect_bridge(int fd)
{
	unsigned in[2] = {0};
	struct sgx545_connect_out out = {0};
	struct sgx545_bridge32 w = package(12, 0, in, sizeof(in), &out, original_intel_abi ? 8 : sizeof(out));

	expect("CONNECT ioctl", call(fd, &w), 0);
	if (out.error || !out.services_lo || out.services_hi)
		fail("CONNECT payload", out.error);
	return out.services_lo;
}
static void disconnect_bridge(int fd, unsigned services)
{
	struct sgx545_handle_out out = {0};
	struct sgx545_bridge32 w = package(13, services, 0, 0, &out, sizeof(out));
	expect("DISCONNECT ioctl", call(fd, &w), 0);
	expect("DISCONNECT payload", out.error, 0);
}

static void initializer_test(int fd, unsigned services, unsigned device,
	const char *path, int close_owner)
{
	struct sgx545_init_info_out info = {0}, again = {0};
	struct sgx545_handle_out result = {0};
	struct sgx545_bridge32 w;
	unsigned in[2] = {0, device}, other, i, n = 0, handles = 0;
	int fd2;

	w = package(98, services, in, 8, &info, sizeof(info));
	expect("initializer info requires ownership", call(fd, &w), -13);
	w = package(67, services, 0, 0, (void *)1, 8);
	expect("initializer CONNECT copyout rollback", call(fd, &w), -14);
	w.param_out = ptr(&result);
	expect("initializer CONNECT ioctl", call(fd, &w), 0);
	expect("initializer CONNECT payload (reload services after an earlier attempt)", result.error, 0);

	fd2 = open_bridge(path); other = connect_bridge(fd2);
	w = package(67, other, 0, 0, &result, 8);
	expect("second initializer CONNECT ioctl", call(fd2, &w), 0);
	if (!result.error) fail("second file acquired initializer ownership", result.error);
	w = package(98, other, in, 8, &info, sizeof(info));
	expect("other file cannot query initializer info", call(fd2, &w), -13);

	in[1] = device + 1;
	w = package(98, services, in, 8, &info, sizeof(info));
	expect("initializer rejects unknown device token", call(fd, &w), -2);
	in[1] = device; w.out_size += 4;
	expect("initializer rejects native-sized reply", call(fd, &w), -22);
	w.out_size = sizeof(info); w.param_out = 1;
	expect("initializer heap copyout rollback", call(fd, &w), -14);
	w.param_out = ptr(&info);
	expect("initializer INFO ioctl", call(fd, &w), 0);
	expect("initializer INFO payload", info.error, 0);
	if (!info.page_directory || (info.page_directory & 4095))
		fail("invalid kernel page-directory address", info.page_directory);
	say("SGX initializer page_directory=0x"); hex(info.page_directory); say("\n");
	for (i = 0; i < SGX545_MAX_HEAPS; i++) {
		const struct sgx545_heap32 *h = &info.heaps[i];
		if (h->id == SGX545_UNDEFINED_HEAP) {
			if (h->handle || h->base || h->size || h->attributes || h->tile_stride)
				fail("undefined heap contains data", i);
			continue;
		}
		if (!sgx545_heap_valid(h)) fail("invalid heap range", i);
		if (h->size) n++;
		if (h->handle) {
			handles++;
			if (h->handle == services || h->handle == device) fail("heap token collision", i);
		}
		say("heap id=0x"); hex(h->id); say(" handle=0x"); hex(h->handle);
		say(" base=0x"); hex(h->base); say(" size=0x"); hex(h->size);
		say(" attributes=0x"); hex(h->attributes); say(" tile_stride=0x"); hex(h->tile_stride);
		if (!h->size) say(" disabled");
		say("\n");
	}
	if (!n || !handles) fail("missing initializer heaps or shared handles", n);
	w.param_out = ptr(&again);
	expect("repeated initializer INFO ioctl", call(fd, &w), 0);
	for (i = 0; i < sizeof(info); i++)
		if (((const unsigned char *)&info)[i] != ((const unsigned char *)&again)[i])
			fail("repeated initializer INFO changed", i);
	in[1] = 1;
	w = package(68, services, in, 8, &result, 8);
	expect("successful GPU initialization blocked", call(fd, &w), -95);
	if (close_owner) {
		expect("close automatically aborts initializer", sys3(6, fd, 0, 0), 0);
	} else {
		in[1] = 0;
		expect("initializer abort ioctl", call(fd, &w), 0);
		expect("initializer abort payload", result.error, 0);
		in[1] = device;
		w = package(98, services, in, 8, &info, sizeof(info));
		expect("initializer INFO rejected after abort", call(fd, &w), -13);
	}
	w = package(67, other, 0, 0, &result, 8);
	expect("initializer attempt is one-shot ioctl", call(fd2, &w), 0);
	if (!result.error) fail("initializer restarted after terminal abort", result.error);
	expect("second initializer file close", sys3(6, fd2, 0, 0), 0);
	say("PASS: initializer ownership, 776-byte heap reply, repeat query, rollback and ");
	say(close_owner ? "automatic close abort\n" : "explicit abort\n");
}

static void check_words(volatile unsigned *address, unsigned size, unsigned seed)
{
	unsigned i;
	for (i = 0; i < size / 4; i++)
		if (address[i] != (seed ^ i)) fail("mapped buffer pattern mismatch", i);
}
static volatile unsigned *map_buffer(int fd, unsigned size, unsigned token)
{
	unsigned address = probe_mmap2(fd, size, 3, 1, token);
	if (address >= 0xfffff001U) fail("mmap2 buffer", address);
	return (volatile unsigned *)address;
}

static void mmu_test(int fd, unsigned services, unsigned device, unsigned buffer)
{
	struct sgx545_init_info_out info = {0};
	struct sgx545_mmu_check check = {.services = services, .device = device, .buffer = buffer};
	struct sgx545_bridge32 w;
	unsigned in[2] = {0, device}, i, repeat, boundary, heaps = 0, crossings = 0, temporary;
	volatile struct sgx545_mmu_check *readonly;

	expect("MMU check requires delivered heap info", sys3(54, fd, SGX545_IOCTL_MMU_CHECK, ptr(&check)), -13);
	w = package(98, services, in, 8, &info, sizeof(info));
	expect("MMU heap info ioctl", call(fd, &w), 0);
	expect("MMU heap info payload", info.error, 0);
	for (i = 0; i < SGX545_MAX_HEAPS; i++) if (info.heaps[i].handle) break;
	if (i == SGX545_MAX_HEAPS) fail("no shared MMU heap", i);
	check.heap = info.heaps[i].handle; check.device++;
	expect("MMU bad device token", sys3(54, fd, SGX545_IOCTL_MMU_CHECK, ptr(&check)), -2);
	check.device--; check.heap = 0xffffffffU;
	expect("MMU bad heap token", sys3(54, fd, SGX545_IOCTL_MMU_CHECK, ptr(&check)), -2);
	check.heap = info.heaps[i].handle; check.flags = 2;
	expect("MMU unsupported flags", sys3(54, fd, SGX545_IOCTL_MMU_CHECK, ptr(&check)), -22);
	check.flags = 0; check.buffer++;
	expect("MMU bad buffer token", sys3(54, fd, SGX545_IOCTL_MMU_CHECK, ptr(&check)), -2);
	check.buffer--;
	temporary = probe_mmap2(~0U, 4096, 3, 0x22, 0);
	if (temporary >= 0xfffff001U) fail("MMU output-fault test mmap", temporary);
	readonly = (volatile struct sgx545_mmu_check *)temporary;
	*readonly = check;
	expect("MMU output-fault mprotect", sys3(125, temporary, 4096, 1), 0);
	expect("MMU output copy fault after cleanup", sys3(54, fd, SGX545_IOCTL_MMU_CHECK, temporary), -14);
	expect("MMU output-fault munmap", sys3(91, temporary, 4096, 0), 0);
	for (i = 0; i < SGX545_MAX_HEAPS; i++) {
		const struct sgx545_heap32 *h = &info.heaps[i];
		if (!h->handle) continue;
		heaps++;
		for (boundary = 0; boundary <= 1; boundary++) {
			if (boundary && (h->size < 0x800000 || (h->base & 0x3fffff))) continue;
			for (repeat = 0; repeat < 2; repeat++) {
				check.heap = h->handle; check.flags = boundary;
				expect("offline MMU map/walk/free ioctl", sys3(54, fd, SGX545_IOCTL_MMU_CHECK, ptr(&check)), 0);
				if (check.checked_pages != 4 || check.pt_after || check.pt_peak != (boundary ? 2U : 1U) ||
				    check.scatter_breaks != 3 || (check.device_address & 4095) ||
				    check.device_address < h->base || (__u64)check.device_address + 16384 > (__u64)h->base + h->size)
					fail("offline MMU result", check.device_address);
				if (boundary && (check.device_address >> 22) == ((check.device_address + 16383) >> 22))
					fail("MMU allocation did not cross a PT boundary", check.device_address);
				say("MMU heap=0x"); hex(h->id); say(" expired_va=0x"); hex(check.device_address);
				say(" pages=4 pt_peak="); number(check.pt_peak); say(" pt_after=0 scatter_breaks=3\n");
				crossings += boundary;
			}
		}
	}
	if (!heaps || !crossings) fail("MMU heap/boundary coverage missing", heaps);
	say("PASS: native DDK offline MMU scatter mappings, PT boundary crossings, repeated free and zero remaining page tables\n");
}

static void buffer_test(int fd, unsigned services, unsigned device, const char *path, int close_only, int mmu_mode)
{
	struct sgx545_handle_out result = {0};
	struct sgx545_alloc_shared_out allocation = {0};
	struct sgx545_map_info_out mapping = {0};
	struct sgx545_buffer_check check;
	struct { unsigned flags, token; struct sgx545_client_mem32 mem; } free_in = {0};
	struct sgx545_bridge32 w;
	unsigned in[3] = {0, SGX545_BUFFER_WC, 3 * 4096 + 17};
	unsigned token, other, size, i, vector[4] = {0};
	volatile unsigned *first, *second;
	int fd2, child, status;

	w = package(63, services, in, 12, &allocation, 56);
	expect("allocation requires initializer", call(fd, &w), -13);
	w = package(67, services, 0, 0, &result, 8);
	expect("buffer initializer CONNECT ioctl", call(fd, &w), 0);
	expect("buffer initializer CONNECT payload (reload services for a fresh attempt)", result.error, 0);
	w = package(63, services, in, 12, &allocation, 56);
	in[1] = 0;
	expect("unsupported buffer cache flags", call(fd, &w), -95);
	in[1] = SGX545_BUFFER_WC; in[2] = 0;
	expect("zero buffer allocation", call(fd, &w), -22);
	in[2] = SGX545_BUFFER_MAX_BYTES + 1;
	expect("oversized buffer allocation", call(fd, &w), -22);
	in[2] = 3 * 4096 + 17; w.param_out = 1;
	expect("buffer allocation copyout rollback", call(fd, &w), -14);
	w.param_out = ptr(&allocation);
	expect("shared buffer allocation ioctl", call(fd, &w), 0);
	expect("shared buffer allocation payload", allocation.error, 0);
	token = allocation.kernel_mem;
	if (!token || allocation.mem.kernel_mem != token || allocation.mem.mapping != token ||
	    allocation.mem.alloc_size != in[2] || allocation.mem.flags != (SGX545_BUFFER_WC | SGX545_BUFFER_MULTI) ||
	    allocation.mem.linear || allocation.mem.kernel_linear || allocation.mem.device_address ||
	    allocation.mem.physical_address || allocation.mem.client_flags || allocation.mem.sync_info || allocation.mem.resource || allocation.mem.next)
		fail("shared buffer reply", token);
	in[1] = token;
	w = package(11, services, in, 8, &mapping, 20);
	expect("buffer map metadata ioctl", call(fd, &w), 0);
	if (mapping.error || mapping.offset != token || mapping.byte_offset || mapping.user_address || mapping.real_size != 4 * 4096)
		fail("buffer map metadata payload", mapping.error);
	size = mapping.real_size;
	expect("private mapping rejected", probe_mmap2(fd, size, 3, 2, token), -13);
	expect("executable mapping rejected", probe_mmap2(fd, size, 5, 1, token), -13);
	expect("wrong mapping length rejected", probe_mmap2(fd, size + 4096, 3, 1, token), -22);
	expect("unknown mapping token rejected", probe_mmap2(fd, size, 3, 1, token + 1000), -2);
	fd2 = open_bridge(path); other = connect_bridge(fd2);
	w.services = other;
	expect("cross-file map-info token rejected", call(fd2, &w), -2);
	expect("cross-file mmap token rejected", probe_mmap2(fd2, size, 3, 1, token), -2);
	disconnect_bridge(fd2, other); expect("close second buffer file", sys3(6, fd2, 0, 0), 0);
	first = map_buffer(fd, size, token); second = map_buffer(fd, size, token);
	for (i = 0; i < size / 4; i++) {
		if (first[i] || second[i]) fail("buffer or padding not zeroed", i);
		first[i] = 0x13579bdfU ^ i;
	}
	__asm__ volatile("sfence" ::: "memory");
	check_words(second, size, 0x13579bdfU);
	check = (struct sgx545_buffer_check){services, token, 0, 0};
	expect("kernel rejects incorrect expected pattern", sys3(54, fd, SGX545_IOCTL_BUFFER_CHECK, ptr(&check)), -5);
	check.expected = 0x13579bdfU; check.fill = 0x2468ace0U;
	expect("kernel WC mapping roundtrip", sys3(54, fd, SGX545_IOCTL_BUFFER_CHECK, ptr(&check)), 0);
	check_words(first, size, check.fill); check_words(second, size, check.fill);
	if (mmu_mode) {
		mmu_test(fd, services, device, token);
		check_words(first, size, check.fill); check_words(second, size, check.fill);
	}
	expect("split VMA with read-only middle page", sys3(125, ptr((const void *)first) + 4096, 4096, 1), 0);
	expect("mprotect cannot enable execution", sys3(125, ptr((const void *)first), 4096, 5), -13);
	check.expected = check.fill; check.fill = 0xabcdef01U;
	expect("kernel roundtrip across split VMAs", sys3(54, fd, SGX545_IOCTL_BUFFER_CHECK, ptr(&check)), 0);
	check_words(first, size, check.fill); check_words(second, size, check.fill);
	child = sys3(2, 0, 0, 0);
	if (child < 0) fail("buffer fork", child);
	if (!child) {
		/* VM_DONTCOPY must leave no mapping in the child. */
		sys3(1, sys3(218, ptr((const void *)first), size, ptr(vector)) == -12 ? 0 : 1, 0, 0);
		return;
	}
	expect("buffer fork waitpid", sys3(7, child, ptr(&status), 0), child);
	expect("buffer mapping is not inherited by fork", status, 0);
	if (!close_only) {
		free_in.token = token; free_in.mem = allocation.mem;
		w = package(64, services, &free_in, sizeof(free_in), &result.error, 4);
		expect("free buffer while mapped ioctl", call(fd, &w), 0);
		expect("free buffer while mapped payload", result.error, 0);
		expect("stale buffer free rejected", call(fd, &w), -2);
		w = package(11, services, in, 8, &mapping, 20);
		expect("freed buffer map-info rejected", call(fd, &w), -2);
		expect("freed buffer mmap rejected", probe_mmap2(fd, size, 3, 1, token), -2);
		disconnect_bridge(fd, services);
	}
	expect("close fd while mapped", sys3(6, fd, 0, 0), 0);
	check_words(first, size, check.fill); check_words(second, size, check.fill);
	expect("unmap first buffer including split VMAs", sys3(91, ptr((const void *)first), size, 0), 0);
	check_words(second, size, check.fill);
	expect("unmap last buffer VMA", sys3(91, ptr((const void *)second), size, 0), 0);
	say("PASS: i386 shared buffers, zeroed padding, user/kernel WC roundtrip, map isolation, VMA splits and ");
	say(close_only ? "deferred close cleanup\n" : "free/disconnect with live mappings\n");
	say("Check sysfs: buffers=0 buffer_pages=0 buffer_maps=0 buffer_errors=0 files=0 connections=0 init_running=0\n");
}

static void device_reply(const struct sgx545_alloc_device_out *allocation,
	const struct sgx545_heap32 *heap, unsigned alignment)
{
	unsigned i;
	const struct sgx545_client_mem32 *m = &allocation->mem;
	if (allocation->error || !allocation->kernel_mem || m->kernel_mem != allocation->kernel_mem ||
	    m->mapping != allocation->kernel_mem || m->alloc_size != 3 * 4096 + 17 ||
	    m->flags != (SGX545_DEVICE_FLAGS | SGX545_RAM_BACKED | SGX545_BUFFER_MULTI) ||
	    m->linear || m->kernel_linear || m->physical_address || m->client_flags ||
	    m->sync_info || m->resource || m->next || (m->device_address & (alignment - 1)) ||
	    m->device_address < heap->base || (__u64)m->device_address + 16384 > (__u64)heap->base + heap->size)
		fail("device allocation reply", allocation->error);
	for (i = 0; i < 5; i++) if (allocation->sync[i]) fail("unexpected sync object", i);
}
static void device_walk(int fd, unsigned services, unsigned device,
	const struct sgx545_alloc_device_out *allocation)
{
	struct sgx545_device_check check = {services, device, allocation->kernel_mem, 0, 0, 0};
	expect("persistent device PTE readback", sys3(54, fd, SGX545_IOCTL_DEVICE_CHECK, ptr(&check)), 0);
	if (check.address != allocation->mem.device_address || check.pages != 4 || !check.page_tables)
		fail("persistent device mapping result", check.address);
}
static volatile unsigned *device_mapping(int fd, unsigned services,
	const struct sgx545_alloc_device_out *allocation, unsigned seed)
{
	unsigned in[2] = {0, allocation->kernel_mem}, i;
	struct sgx545_map_info_out mapping = {0};
	struct sgx545_bridge32 w = package(11, services, in, 8, &mapping, 20);
	struct sgx545_buffer_check check = {services, allocation->kernel_mem, seed, seed ^ 0xffffffffU};
	volatile unsigned *memory;
	expect("device map-info", call(fd, &w), 0);
	if (mapping.error || mapping.offset != allocation->kernel_mem || mapping.real_size != 16384 ||
	    mapping.byte_offset || mapping.user_address) fail("device map metadata", mapping.error);
	memory = map_buffer(fd, 16384, allocation->kernel_mem);
	for (i = 0; i < 4096; i++) {
		if (memory[i]) fail("device buffer/padding zeroing", i);
		memory[i] = seed ^ i;
	}
	__asm__ volatile("sfence" ::: "memory");
	expect("device WC user/kernel roundtrip", sys3(54, fd, SGX545_IOCTL_BUFFER_CHECK, ptr(&check)), 0);
	check_words(memory, 16384, check.fill);
	return memory;
}
static void device_test(int fd, unsigned services, unsigned device, const char *path, int close_only)
{
	struct sgx545_handle_out result = {0};
	struct sgx545_init_info_out info = {0};
	struct sgx545_alloc_device_out a = {0}, b = {0}, c = {0};
	struct sgx545_device_check check;
	struct sgx545_bridge32 w;
	struct { unsigned flags, token; struct sgx545_client_mem32 mem; } cpu_free = {0};
	unsigned in[6] = {0, device, 0, SGX545_DEVICE_FLAGS, 3 * 4096 + 17, 0};
	unsigned free_in[15] = {0, device, 0}, i, heaps = 0, other, other_device;
	volatile unsigned *first = 0, *second = 0;
	int fd2;

	w = package(6, services, in, 24, &a, 76);
	expect("device allocation requires initializer", call(fd, &w), -13);
	w = package(67, services, 0, 0, &result, 8);
	expect("device initializer CONNECT ioctl", call(fd, &w), 0);
	expect("device initializer CONNECT payload (reload services for a fresh attempt)", result.error, 0);
	w = package(6, services, in, 24, &a, 76);
	expect("device allocation requires delivered heap info", call(fd, &w), -13);
	w = package(98, services, in, 8, &info, sizeof(info));
	expect("device initializer INFO", call(fd, &w), 0);
	expect("device initializer INFO payload", info.error, 0);
	for (i = 0; i < SGX545_MAX_HEAPS; i++) if (info.heaps[i].handle) break;
	if (i == SGX545_MAX_HEAPS) fail("no device allocation heaps", i);
	in[2] = info.heaps[i].handle;
	w = package(6, services, in, 24, &a, 76);
	in[1]++;
	expect("device allocation bad device", call(fd, &w), -2); in[1]--;
	in[2] = 0xffffffffU;
	expect("device allocation bad heap", call(fd, &w), -2); in[2] = info.heaps[i].handle;
	in[3] &= ~8U;
	expect("sync objects remain unsupported", call(fd, &w), -95); in[3] = SGX545_DEVICE_FLAGS;
	in[4] = SGX545_BUFFER_MAX_BYTES + 1;
	expect("device allocation bound", call(fd, &w), -22); in[4] = 3 * 4096 + 17;
	in[5] = 4097;
	expect("device alignment validation", call(fd, &w), -22); in[5] = 0;
	w.out_size = 80;
	expect("native-sized device reply rejected", call(fd, &w), -22); w.out_size = 76;
	w.param_out = 1;
	expect("device allocation copyout rollback", call(fd, &w), -14);
	for (i = 0; i < SGX545_MAX_HEAPS; i++) {
		const struct sgx545_heap32 *heap = &info.heaps[i];
		if (!heap->handle) continue;
		heaps++; in[2] = heap->handle; in[5] = 0;
		w = package(6, services, in, 24, &a, 76);
		expect("first persistent allocation", call(fd, &w), 0); device_reply(&a, heap, 4096);
		in[5] = 65536; in[3] |= SGX545_RAM_BACKED;
		w.param_out = ptr(&b);
		expect("second aligned persistent allocation", call(fd, &w), 0); device_reply(&b, heap, 65536);
		if ((__u64)a.mem.device_address < (__u64)b.mem.device_address + 16384 &&
		    (__u64)b.mem.device_address < (__u64)a.mem.device_address + 16384)
			fail("overlapping live device addresses", b.mem.device_address);
		first = device_mapping(fd, services, &a, 0x13579bdfU);
		second = device_mapping(fd, services, &b, 0x2468ace0U);
		device_walk(fd, services, device, &a); device_walk(fd, services, device, &b);
		cpu_free.token = a.kernel_mem; cpu_free.mem = a.mem;
		w = package(64, services, &cpu_free, sizeof(cpu_free), &result.error, 4);
		expect("CPU-only free cannot strand GPU PTEs", call(fd, &w), -22);
		fd2 = open_bridge(path); other = connect_bridge(fd2);
		free_in[1] = 0; free_in[2] = 7;
		{
			struct sgx545_enumerate_out devices = {0};
			w = package(0, other, 0, 0, &devices, sizeof(devices));
			expect("other file device enumeration", call(fd2, &w), 0);
			if (devices.error || devices.count != 1) fail("other file device count", devices.error);
			free_in[1] = devices.devices[0].index;
		}
		w = package(1, other, free_in, 12, &result, 8);
		expect("other file device acquire", call(fd2, &w), 0);
		if (result.error || !result.handle) fail("other file device acquire payload", result.error);
		other_device = result.handle; free_in[1] = other_device; free_in[2] = a.kernel_mem;
		w = package(7, other, free_in, original_intel_abi ? 60 : 12, &result.error, original_intel_abi ? 8 : 4);
		expect("cross-file device free rejected", call(fd2, &w), -2);
		check = (struct sgx545_device_check){other, other_device, a.kernel_mem, 0, 0, 0};
		expect("cross-file device PTE check rejected", sys3(54, fd2, SGX545_IOCTL_DEVICE_CHECK, ptr(&check)), -2);
		disconnect_bridge(fd2, other); expect("close other device file", sys3(6, fd2, 0, 0), 0);
		free_in[1] = device;
		w = package(7, services, free_in, original_intel_abi ? 60 : 12, &result.error, original_intel_abi ? 8 : 4);
		expect("free device allocation with live CPU mapping", call(fd, &w), 0);
		expect("device free payload", result.error, 0);
		expect("stale device free rejected", call(fd, &w), -2);
		check = (struct sgx545_device_check){services, device, a.kernel_mem, 0, 0, 0};
		expect("freed device PTE check rejected", sys3(54, fd, SGX545_IOCTL_DEVICE_CHECK, ptr(&check)), -2);
		check_words(first, 16384, 0xeca86420U);
		device_walk(fd, services, device, &b);
		in[5] = 4096;
		w = package(6, services, in, 24, &c, 76);
		expect("allocation after freeing neighbor", call(fd, &w), 0); device_reply(&c, heap, 4096);
		if (c.kernel_mem == a.kernel_mem || c.kernel_mem == b.kernel_mem)
			fail("device allocation reused stale token", c.kernel_mem);
		device_walk(fd, services, device, &b); device_walk(fd, services, device, &c);
		free_in[2] = c.kernel_mem; w = package(7, services, free_in, original_intel_abi ? 60 : 12, &result.error, original_intel_abi ? 8 : 4);
		expect("free replacement allocation", call(fd, &w), 0); expect("free replacement payload", result.error, 0);
		device_walk(fd, services, device, &b);
		say("DEVICE heap=0x"); hex(heap->id); say(" pages=4 aligned_va=0x"); hex(b.mem.device_address);
		say(" sibling_free=preserved CPU_mapping=preserved\n");
		/* Keep one live allocation and both VMAs for final disconnect/close. */
		if (heaps == 3) break;
		free_in[2] = b.kernel_mem; w = package(7, services, free_in, original_intel_abi ? 60 : 12, &result.error, original_intel_abi ? 8 : 4);
		expect("free second allocation", call(fd, &w), 0); expect("free second payload", result.error, 0);
		expect("unmap first device buffer", sys3(91, ptr((const void *)first), 16384, 0), 0);
		expect("unmap second device buffer", sys3(91, ptr((const void *)second), 16384, 0), 0);
	}
	if (heaps != 3) fail("expected three shared initializer heaps", heaps);
	if (!close_only) disconnect_bridge(fd, services);
	expect("close device fd with live VMAs", sys3(6, fd, 0, 0), 0);
	check_words(first, 16384, 0xeca86420U); check_words(second, 16384, 0xdb97531fU);
	expect("unmap freed device backing", sys3(91, ptr((const void *)first), 16384, 0), 0);
	check_words(second, 16384, 0xdb97531fU);
	expect("unmap final device backing", sys3(91, ptr((const void *)second), 16384, 0), 0);
	say("PASS: i386 persistent device allocations, aligned VA, CPU-order PTEs, WC roundtrip, sibling lifetime, rollback and ");
	say(close_only ? "deferred close cleanup\n" : "disconnect with live mappings\n");
	say("Check sysfs: device_allocations=0 device_pages=0 device_errors=0 buffers=0 buffer_pages=0 buffer_maps=0 buffer_errors=0 files=0 connections=0 init_running=0\n");
}

int probe_main(unsigned *initial_stack)
{
	unsigned argc = initial_stack[0];
	char **argv = (char **)(initial_stack + 1);
	const char *path = "/dev/gma3600-sgx-bridge";
	unsigned in[3] = {0}, services, other, device, stale;
	struct sgx545_enumerate_out devices = {0};
	struct sgx545_connect_out connected = {0};
	struct sgx545_handle_out acquired = {0};
	struct sgx545_bridge32 w;
	int fd, fd2, child, status;
	int init_mode = 0;

	if (argc > 1 && same(argv[1], "--selftest")) {
		struct sgx545_heap32 disabled = {0x07000009, 0, 0xd2000000, 0, 0x02024200, 0};
		unsigned mapped = probe_mmap2(~0U, 4096, 3, 0x22, 0);
		volatile unsigned *memory;

		if (mapped >= 0xfffff001U) fail("selftest mmap2 syscall", mapped);
		memory = (volatile unsigned *)mapped;
		if (memory[0] || memory[1023]) fail("selftest anonymous page zeroing", mapped);
		memory[1023] = 0x12345678;
		if (memory[1023] != 0x12345678) fail("selftest mmap2 write", mapped);
		expect("selftest munmap syscall", sys3(91, mapped, 4096, 0), 0);
		if (!sgx545_heap_valid(&disabled)) fail("disabled PB heap rejected", 9);
		disabled.handle = 1;
		if (sgx545_heap_valid(&disabled)) fail("disabled heap handle accepted", 9);
		say("PASS: ELF32 i386 executable; envelope=28 CONNECT=8/12 FREE_DEVICE_IN=60/12 FREE_DEVICE_OUT=8/4 init2_wire=2068 ENUMERATE=200 handle=8 heap=24 init_info=776 client_mem=48 alloc=56 map_info=20 mmu_check=40 device_alloc=76 device_check=24 mmap2=passed disabled_heap=accepted\n");
		return 0;
	}
	if (argc > 2) { say("Usage: sgx545-compat-probe [--selftest | --init-info | --init-close | --buffers | --buffers-close | --mmu | --device | --device-close | --device-intel | device-path]\n"); return 2; }
	if (argc == 2) {
		if (same(argv[1], "--init-info")) init_mode = 1;
		else if (same(argv[1], "--init-close")) init_mode = 2;
		else if (same(argv[1], "--buffers")) init_mode = 3;
		else if (same(argv[1], "--buffers-close")) init_mode = 4;
		else if (same(argv[1], "--mmu")) init_mode = 5;
		else if (same(argv[1], "--device")) init_mode = 6;
		else if (same(argv[1], "--device-close")) init_mode = 7;
		else if (same(argv[1], "--device-intel")) { init_mode = 6; original_intel_abi = 1; }
		else path = argv[1];
	}
	fd = open_bridge(path);

	w = package(12, 0, in, 8, &connected, 12);
	expect("unknown outer ioctl", sys3(54, fd, 0, ptr(&w)), -25);
	expect("bad envelope address", sys3(54, fd, SGX545_IOCTL_SERVICES, 1), -14);
	w.size = 32;
	expect("bad envelope size", call(fd, &w), -22);
	w.size = 28; w.out_size = 16;
	expect("native padded CONNECT rejected", call(fd, &w), -22);
	w.out_size = 12; w.param_in = 1;
	expect("bad nested input", call(fd, &w), -14);
	w.param_in = ptr(in); in[1] = 1;
	expect("unsupported CONNECT flags", call(fd, &w), -95);
	in[1] = 0; w.param_out = 1;
	expect("CONNECT copyout rollback", call(fd, &w), -14);
	services = connect_bridge(fd);
	expect("repeat CONNECT reuses connection", connect_bridge(fd), services);

	w = package(0, services, 0, 0, &devices, sizeof(devices));
	expect("ENUMERATE alias ioctl", sys3(54, fd, SGX545_IOCTL_SERVICES_ALIAS, ptr(&w)), 0);
	if (devices.error || devices.count != 1 || devices.devices[0].type != 7 ||
	    devices.devices[0].device_class != 0)
		fail("SGX device enumeration", devices.error);
	say("PASS: real DDK device index="); number(devices.devices[0].index); say(" type=SGX\n");
	w.bridge_id = SGX545_COMMAND(2);
	expect("GPU command blocked", call(fd, &w), -95);
	w.bridge_id = SGX545_COMMAND(0); w.services = services + 1;
	expect("unknown service token", call(fd, &w), -2);
	w.services = services;
	child = sys3(2, 0, 0, 0);
	if (child < 0) fail("fork", child);
	if (!child) { sys3(1, call(fd, &w) == -1 ? 0 : 1, 0, 0); return 1; }
	expect("waitpid", sys3(7, child, ptr(&status), 0), child);
	expect("forked process cannot use inherited bridge session", status, 0);

	in[1] = devices.devices[0].index + 100; in[2] = 7;
	w = package(1, services, in, 12, &acquired, 8);
	expect("bad device index ioctl", call(fd, &w), 0);
	if (!acquired.error || acquired.handle) fail("bad device index accepted", acquired.error);
	in[1] = devices.devices[0].index; w.param_out = 1;
	expect("ACQUIRE copyout rollback", call(fd, &w), -14);
	w.param_out = ptr(&acquired);
	expect("ACQUIRE ioctl", call(fd, &w), 0);
	if (acquired.error || !acquired.handle) fail("ACQUIRE payload", acquired.error);
	device = acquired.handle;
	expect("repeat ACQUIRE ioctl", call(fd, &w), 0);
	expect("repeat ACQUIRE reuses handle", acquired.handle, device);

	fd2 = open_bridge(path);
	other = connect_bridge(fd2);
	if (other == services || other == device) fail("tokens repeat across files", other);
	w = package(0, services, 0, 0, &devices, sizeof(devices));
	expect("cross-file service rejected", call(fd2, &w), -2);
	w = package(1, other, in, 12, &acquired, 8);
	expect("second file ACQUIRE", call(fd2, &w), 0);
	if (acquired.error || !acquired.handle || acquired.handle == device)
		fail("second file device handle", acquired.error);
	expect("close implicitly disconnects second file", sys3(6, fd2, 0, 0), 0);
	if (init_mode) {
		if (init_mode >= 6) {
			device_test(fd, services, device, path, init_mode == 7);
			if (original_intel_abi) say("PASS: original Intel CONNECT=8 FREE_DEVICE_IN=60 FREE_DEVICE_OUT=8 payload forms\n");
			return 0;
		}
		if (init_mode >= 3) {
			buffer_test(fd, services, device, path, init_mode == 4, init_mode == 5);
			return 0;
		}
		initializer_test(fd, services, device, path, init_mode == 2);
		if (init_mode == 2) {
			say("Check sysfs: files=0 connections=0 heap_handles=0 init_running=0 init_ran=1 bridge_errors=0 blocked_hw_accesses=0\n");
			return 0;
		}
	}

	stale = services;
	disconnect_bridge(fd, services);
	in[1] = in[2] = 0;
	services = connect_bridge(fd);
	if (stale == services) fail("service token reused after disconnect", services);
	w = package(0, stale, 0, 0, &devices, sizeof(devices));
	expect("stale service rejected", call(fd, &w), -2);
	disconnect_bridge(fd, services);
	expect("close", sys3(6, fd, 0, 0), 0);
	say("PASS: i386 compat bridge, handles, validation, copyout rollback, fork isolation and close cleanup\n");
	say("Check sysfs: files=0 connections=0 bridge_errors=0 blocked_hw_accesses=0\n");
	return 0;
}

__asm__(".text\n.global _start\n_start:\n"
	"mov %esp,%eax\nand $-16,%esp\nsub $12,%esp\npush %eax\n"
	"call probe_main\nmov %eax,%ebx\nmov $1,%eax\nint $0x80\n"
	".global probe_mmap2\n.type probe_mmap2,@function\nprobe_mmap2:\n"
	"push %ebp\npush %edi\npush %esi\npush %ebx\n"
	"mov $192,%eax\nxor %ebx,%ebx\nmov 24(%esp),%ecx\nmov 28(%esp),%edx\n"
	"mov 32(%esp),%esi\nmov 20(%esp),%edi\nmov 36(%esp),%ebp\nint $0x80\n"
	"pop %ebx\npop %esi\npop %edi\npop %ebp\nret\n");
