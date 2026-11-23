// SPDX-License-Identifier: GPL-2.0-only
/* Genuine i386 Linux client, without libc or proprietary userspace libraries.
 * Build with tools/sgx545_compat_probe_build.sh. Only the diagnostic subset runs.
 */
#define SGX545_FREESTANDING
#include "sgx545_abi.h"

_Static_assert(sizeof(void *) == 4, "Build this client with -m32");
_Static_assert(sizeof(struct sgx545_bridge32) == 28, "i386 envelope");
_Static_assert(sizeof(struct sgx545_connect_out) == 12, "i386 CONNECT");
_Static_assert(sizeof(struct sgx545_enumerate_out) == 200, "i386 ENUMERATE");
_Static_assert(sizeof(struct sgx545_handle_out) == 8, "i386 ACQUIRE/DISCONNECT");
_Static_assert(sizeof(struct sgx545_heap32) == 24, "i386 heap");
_Static_assert(sizeof(struct sgx545_init_info_out) == 776, "i386 initializer info");

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
	struct sgx545_bridge32 w = package(12, 0, in, sizeof(in), &out, sizeof(out));

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
		n++;
		if (!h->size || (__u64)h->base + h->size > (1ULL << 32)) fail("invalid heap range", i);
		if (h->handle) {
			handles++;
			if (h->handle == services || h->handle == device) fail("heap token collision", i);
		}
		say("heap id=0x"); hex(h->id); say(" handle=0x"); hex(h->handle);
		say(" base=0x"); hex(h->base); say(" size=0x"); hex(h->size);
		say(" attributes=0x"); hex(h->attributes); say(" tile_stride=0x"); hex(h->tile_stride); say("\n");
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
		say("PASS: ELF32 i386 executable; envelope=28 CONNECT=12 ENUMERATE=200 handle=8 heap=24 init_info=776\n");
		return 0;
	}
	if (argc > 2) { say("Usage: sgx545-compat-probe [--selftest | --init-info | --init-close | device-path]\n"); return 2; }
	if (argc == 2) {
		if (same(argv[1], "--init-info")) init_mode = 1;
		else if (same(argv[1], "--init-close")) init_mode = 2;
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
	"call probe_main\nmov %eax,%ebx\nmov $1,%eax\nint $0x80\n");
