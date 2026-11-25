// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#define SGX545_TRANSPORT_TEST
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
static unsigned opens, closes, ioctls;
static int fail_open, fail_close, fail_ioctl;
static int transport_open(const char *path, int flags)
{
	assert(!strcmp(path, "/dev/gma3600-sgx-bridge") && flags == (O_RDWR | O_CLOEXEC));
	opens++;
	if (fail_open) { errno = EACCES; return -1; }
	return (int)opens;
}
static int transport_close(int fd)
{
	assert(fd > 0); closes++;
	if (fail_close) { errno = EINTR; return -1; }
	return 0;
}
static int transport_ioctl(int fd, unsigned long command, void *arg)
{
	assert(fd > 0 && command == 0xc01c6440U && arg); ioctls++;
	if (fail_ioctl) { errno = EOPNOTSUPP; return -1; }
	return 0;
}
#include "../tools/sgx545_initializer_transport.c"

int main(void)
{
	struct sgx545_bridge32 wire = {SGX545_COMMAND(0), 28, 0, 0, 0, 0, 1};
	unsigned before, i;
	int fd, other;
	unsigned *input = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
	assert(input != MAP_FAILED && (unsigned long)input <= 0xfffff000UL);
	assert(drmOpen("i915", NULL) == -EINVAL && !opens);
	assert(drmOpen("psb", "pci:0:2:0") == -EINVAL && !opens);
	assert(drmOpen(NULL, NULL) == -EINVAL && !opens);
	assert(drmOpen(NULL, "pci:0000:00:03.0") == -EINVAL && !opens);
	assert(drmOpen(NULL, "pci:0001:00:02.0") == -EINVAL && !opens);
	assert(drmOpen(NULL, "pci:0000:00:02.1") == -EINVAL && !opens);
	assert(drmOpen("i915", "pci:0000:00:02.0") == -EINVAL && !opens);
	/* Reproduce the observed Intel selector, then its master check and first
	 * bridge request on the returned file. This failed before the PCI fix. */
	fd = drmOpen(NULL, "pci:0000:00:02.0");
	assert(fd > 0 && !drmCommandNone(fd, 21));
	assert(!drmCommandWrite(fd, 0x12, &wire, 28) && ioctls == 1);
	assert(!drmClose(fd) && !descriptor_count);
	ioctls = 0;
	fd = drmOpen("psb", "pci:0000:00:02.0");
	assert(fd > 0 && !drmClose(fd));
	fail_open = 1;
	assert(drmOpen("psb", NULL) == -EACCES);
	fail_open = 0; fd = drmOpen("psb", NULL); other = drmOpen("pvrsrvkm", NULL);
	assert(fd > 0 && other > 0 && fd != other);
	assert(!drmDropMaster(fd) && drmDropMaster(-1) == -EBADF);
	assert(!drmCommandNone(fd, 0x15) && drmCommandNone(fd, 0x12) == -EOPNOTSUPP);
	assert(drmCommandWrite(-1, 0x12, &wire, 28) == -EBADF);
	assert(drmCommandWrite(fd, 0x12, &wire, 24) == -EOPNOTSUPP && !ioctls);
	assert(drmCommandWrite(fd, 0x13, &wire, 28) == -EOPNOTSUPP && !ioctls);
	assert(!drmCommandWrite(fd, 0x12, &wire, 28) && ioctls == 1);
	fail_ioctl = 1;
	assert(drmCommandWrite(fd, 0x12, &wire, 28) == -EOPNOTSUPP && ioctls == 2);
	fail_ioctl = 0; before = ioctls;
	wire.bridge_id = SGX545_COMMAND(99);
	assert(drmCommandWrite(fd, 0x12, &wire, 28) == -EOPNOTSUPP && ioctls == before);
	wire.bridge_id = SGX545_COMMAND(0) ^ (1U << 16);
	assert(drmCommandWrite(fd, 0x12, &wire, 28) == -EINVAL && ioctls == before);
	wire.bridge_id = SGX545_COMMAND(68); wire.in_size = 8; wire.param_in = (unsigned)(unsigned long)input;
	input[1] = 1;
	assert(drmCommandWrite(fd, 0x12, &wire, 28) == -EOPNOTSUPP && ioctls == before);
	input[1] = 0;
	assert(!drmCommandWrite(fd, 0x12, &wire, 28) && ioctls == before + 1);
	wire.bridge_id = SGX545_COMMAND(6); wire.in_size = 24;
	input[2] = 0x11; input[3] = 0x400b; input[4] = 8192; input[5] = 4096;
	assert(!drmCommandWrite(fd, 0x12, &wire, 28) && ioctls == before + 2);
	wire.bridge_id = SGX545_COMMAND(12); wire.in_size = 8;
	input[0] = 0xfefefefe; input[1] = SGX545_CONNECT_PDUMP_PERSIST;
	assert(!drmCommandWrite(fd, 0x12, &wire, 28) && ioctls == before + 3);
	assert(input[0] == 0xfefefefe && input[1] == SGX545_CONNECT_PDUMP_PERSIST); /* logged, never rewritten */
	wire.in_size = 4097;
	assert(drmCommandWrite(fd, 0x12, &wire, 28) == -EINVAL);
	wire.in_size = 8; wire.param_in = 0xfffffffc;
	assert(drmCommandWrite(fd, 0x12, &wire, 28) == -EINVAL);
	requests = 4096; before = ioctls;
	assert(drmCommandWrite(fd, 0x12, &wire, 28) == -E2BIG && ioctls == before);
	requests = 0;
	input[0] = 0;
	assert(!drmCommandWrite(fd, 0x16, input, 4) && !input[0]);
	input[0] = 1;
	assert(drmCommandWrite(fd, 0x16, input, 4) == -EOPNOTSUPP);
	assert(drmClose(-1) == -EBADF);
	assert(!drmClose(fd));
	fail_close = 1;
	assert(drmClose(other) == -EINTR && !descriptor_count);
	fail_close = 0;
	assert(drmCommandWrite(fd, 0x12, &wire, 28) == -EBADF);
	for (i = 0; i < 32; i++) assert(drmOpen("psb", NULL) > 0);
	before = opens;
	assert(drmOpen("psb", NULL) == -EMFILE && opens == before);
	while (descriptor_count) assert(!drmClose(descriptors[0]));
	assert(!munmap(input, 4096));
	puts("SGX initializer transport tests passed: private descriptors, exact routing, ABI bounds and hardware/finalization blocks");
	return 0;
}
