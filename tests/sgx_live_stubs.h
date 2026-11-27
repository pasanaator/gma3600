/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GMA_SGX_LIVE_TEST_STUBS_H
#define GMA_SGX_LIVE_TEST_STUBS_H
#define GMA3600_SGX_BRIDGE_H
#define __iomem
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
struct task_struct { unsigned int id; };
extern struct task_struct *test_current;
#define current test_current
extern int test_in_interrupt;
#define in_interrupt() test_in_interrupt
#define READ_ONCE(value) (value)
#define WRITE_ONCE(target,value) ((target)=(value))
uint64_t ktime_get_ns(void);
void usleep_range(unsigned int low, unsigned int high);
void wmb(void);
uint32_t readl(const void *address);
void writel(uint32_t value, void *address);
int gma3600_sgx_owner_hardware(void *identity, void **registers);
int gma3600_sgx_owner_seal(void *identity);
#endif
