// SPDX-License-Identifier: GPL-2.0-only
/* Private libdrm substitute for the unmodified Intel initializer. This is not
 * a DRM driver or a general libdrm implementation. It opens only our existing
 * prepare-only diagnostic bridge and never forwards a display/submit ioctl.
 */
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "sgx545_abi.h"

#define EXPORT __attribute__((visibility("default")))
#ifndef SGX545_TRANSPORT_TEST
_Static_assert(sizeof(void *) == 4, "Intel initializer transport must be i386");
#define transport_open open
#define transport_close close
#define transport_ioctl ioctl
#endif

/* Each drmOpen gets its own bridge file: do not merge legacy connections. */
static int descriptors[32];
static unsigned descriptor_count;
static unsigned requests;

static int owned(int fd)
{
	unsigned i;
	for (i = 0; i < descriptor_count; i++)
		if (descriptors[i] == fd) return 1;
	return 0;
}

EXPORT const char *gma3600_sgx_transport_version(void) { return "0.1.2"; }

EXPORT int gma3600_sgx_transport_accepts_device(const char *name, const char *busid)
{
	/* Intel selects the integrated GPU by PCI address, with a NULL name.
	 * Named opens are also supported, but neither selector may identify a
	 * different driver/device. Never turn this into general DRM discovery. */
	return (name || busid) &&
		(!name || !strcmp(name, "psb") || !strcmp(name, "pvrsrvkm")) &&
		(!busid || !strcmp(busid, "pci:0000:00:02.0"));
}

EXPORT int drmOpen(const char *name, const char *busid)
{
	int fd;
	fprintf(stderr, "TRANSPORT open name=%s busid=%s\n", name ? name : "(null)", busid ? busid : "(null)");
	if (!gma3600_sgx_transport_accepts_device(name, busid)) {
		fprintf(stderr, "BLOCK open selector\n");
		return -EINVAL;
	}
	if (descriptor_count == 32) return -EMFILE;
	fd = transport_open("/dev/gma3600-sgx-bridge", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		int error = errno;
		fprintf(stderr, "TRANSPORT open_failed errno=%d\n", error);
		return -error;
	}
	descriptors[descriptor_count++] = fd;
	fprintf(stderr, "TRANSPORT opened fd=%d\n", fd);
	return fd;
}

EXPORT int drmClose(int fd)
{
	unsigned i;
	int result;
	for (i = 0; i < descriptor_count; i++) {
		if (descriptors[i] != fd) continue;
		/* Linux releases the fd even when close reports EINTR. */
		descriptors[i] = descriptors[--descriptor_count];
		result = transport_close(fd);
		if (result < 0) return -errno;
		return 0;
	}
	return -EBADF;
}

EXPORT int drmDropMaster(int fd)
{
	/* There is no DRM master on this misc-device transport. */
	return owned(fd) ? 0 : -EBADF;
}

EXPORT int drmCommandNone(int fd, unsigned long command)
{
	fprintf(stderr, "TRANSPORT none command=%lu fd=%d\n", command, fd);
	if (!owned(fd)) return -EBADF;
	/* Original PVR_DRM_IS_MASTER_CMD. The kernel still checks initializer
	 * capability/ownership; no KMS master is acquired or released here. */
	return command == 0x15 ? 0 : -EOPNOTSUPP;
}

EXPORT int drmCommandWrite(int fd, unsigned long command, void *data, unsigned long size)
{
	struct sgx545_bridge32 *wire = data;
	unsigned number;
	int result;

	if (!owned(fd)) return -EBADF;
	if (++requests > 4096) return -E2BIG;
	/* PVR_DRM_UNPRIV_INIT_SUCCESFUL: prepare-only never reports success. */
	if (command == 0x16 && size == 4 && data && !*(unsigned *)data) {
		*(unsigned *)data = 0;
		fprintf(stderr, "TRANSPORT init_successful=0\n");
		return 0;
	}
	if (command != 0x12 || size != sizeof(*wire) || !data) {
		fprintf(stderr, "BLOCK transport command=%lu bytes=%lu\n", command, size);
		return -EOPNOTSUPP;
	}
	if (wire->size != sizeof(*wire) || (wire->bridge_id & ~255U) != SGX545_COMMAND_BASE ||
	    wire->in_size > SGX545_BRIDGE_BUFFER_MAX || wire->out_size > SGX545_BRIDGE_BUFFER_MAX ||
	    (wire->in_size && (!wire->param_in || wire->in_size - 1 > ~wire->param_in)) ||
	    (wire->out_size && (!wire->param_out || wire->out_size - 1 > ~wire->param_out)))
		return -EINVAL;
	number = wire->bridge_id & 255;
	fprintf(stderr, "BRIDGE command=%u in=%u out=%u\n", number, wire->in_size, wire->out_size);
	if (number == SGX545_CMD_CONNECT && wire->in_size == 8) {
		const unsigned *in = (void *)(unsigned long)wire->param_in;
		fprintf(stderr, "CONNECT reserved=%08x service_flags=%08x\n", in[0], in[1]);
	} else if (wire->in_size >= 4) {
		fprintf(stderr, "INPUT command=%u reserved=%08x\n", number,
			*(unsigned *)(unsigned long)wire->param_in);
	}
	/* Record allocation metadata, never firmware or buffer contents. */
	if (number == SGX545_CMD_ALLOC_DEVICE && wire->in_size == 24) {
		unsigned *in = (void *)(unsigned long)wire->param_in;
		fprintf(stderr, "ALLOC heap=%08x flags=%08x bytes=%u alignment=%u\n", in[2], in[3], in[4], in[5]);
	}
	switch (number) {
	case SGX545_CMD_ENUMERATE: case SGX545_CMD_ACQUIRE:
	case SGX545_CMD_CONNECT: case SGX545_CMD_DISCONNECT:
	case SGX545_CMD_INIT_CONNECT: case SGX545_CMD_INIT_INFO:
	case SGX545_CMD_ALLOC_SHARED: case SGX545_CMD_FREE_SHARED:
	case SGX545_CMD_MAP_INFO: case SGX545_CMD_ALLOC_DEVICE: case SGX545_CMD_FREE_DEVICE:
		break;
	case SGX545_CMD_INIT_DISCONNECT:
		if (wire->in_size != 8 || !wire->param_in) return -EINVAL;
		if (((unsigned *)(unsigned long)wire->param_in)[1]) {
			fprintf(stderr, "BLOCK successful initializer finalization\n");
			return -EOPNOTSUPP;
		}
		break;
	default:
		/* Includes command 99. Do not reinterpret or execute scripts. */
		fprintf(stderr, "BLOCK bridge command=%u\n", number);
		return -EOPNOTSUPP;
	}
	result = transport_ioctl(fd, SGX545_IOCTL_SERVICES, wire);
	if (result < 0) {
		result = -errno;
		fprintf(stderr, "RESULT command=%u errno=%d\n", number, -result);
	} else if (wire->out_size >= 4) {
		fprintf(stderr, "RESULT command=%u payload_error=%u\n", number,
			*(unsigned *)(unsigned long)wire->param_out);
	}
	return result;
}
