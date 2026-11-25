// SPDX-License-Identifier: GPL-2.0-only
/* Only this project-owned plugin is executed by cloud loader tests. */
#include <errno.h>
#include <pthread.h>
#include <time.h>
extern int drmCommandWrite(int, unsigned long, void *, unsigned long);
extern int drmCommandNone(int, unsigned long);
extern int drmDropMaster(int);
extern int drmClose(int);
int SrvInit(void)
{
	pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
	struct timespec now;
	if (drmCommandWrite(-1, 0x12, 0, 0) != -EBADF) return 1;
	if (drmCommandNone(-1, 0x15) != -EBADF) return 2;
	if (drmDropMaster(-1) != -EBADF || drmClose(-1) != -EBADF) return 3;
	if (pthread_mutex_lock(&mutex) || pthread_mutex_unlock(&mutex) || pthread_mutex_destroy(&mutex)) return 4;
	if (clock_gettime(CLOCK_MONOTONIC, &now)) return 5;
	return 0;
}
