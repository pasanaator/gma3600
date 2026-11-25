// SPDX-License-Identifier: GPL-2.0-only
/* Only this project-owned plugin is executed by cloud loader tests. */
#include <errno.h>
#include <pthread.h>
#include <time.h>
extern int drmCommandWrite(int, unsigned long, void *, unsigned long);
extern int drmCommandNone(int, unsigned long);
extern int drmDropMaster(int);
extern int drmClose(int);
extern int gma3600_sgx_transport_accepts_device(const char *, const char *);
int SrvInit(void)
{
	pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
	struct timespec now;
	if (drmCommandWrite(-1, 0x12, 0, 0) != -EBADF) return 1;
	if (drmCommandNone(-1, 0x15) != -EBADF) return 2;
	if (drmDropMaster(-1) != -EBADF || drmClose(-1) != -EBADF) return 3;
	if (pthread_mutex_lock(&mutex) || pthread_mutex_unlock(&mutex) || pthread_mutex_destroy(&mutex)) return 4;
	if (clock_gettime(CLOCK_MONOTONIC, &now)) return 5;
	/* Test the exact Intel selector in real i386 code without opening any
	 * device during the project-owned cloud/selftest path. */
	if (!gma3600_sgx_transport_accepts_device(NULL, "pci:0000:00:02.0")) return 6;
	if (gma3600_sgx_transport_accepts_device(NULL, "pci:0000:00:03.0") ||
	    gma3600_sgx_transport_accepts_device("i915", "pci:0000:00:02.0")) return 7;
	return 0;
}
