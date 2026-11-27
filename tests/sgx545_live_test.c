// SPDX-License-Identifier: GPL-2.0-only
/* Actual live provider and boot controller. Only hardware/platform/native
 * boundaries are mocked; actual native bodies have separate source tests. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "gma3600_sgx_live.h"
#include "gma3600_sgx_live_policy.h"
#include "fixtures/sgx545_boot_capture.h"
static struct gma_sgx_live live;
static struct task_struct task={1}, other={2};
struct task_struct *test_current=&task;
int test_in_interrupt;
static uint32_t registers_[4096];
static uint64_t clock_us;
static unsigned held, sealed, writes, reads, arms, resets, published, polls, releases, seals;
static int fail_seal, fail_prepare, fail_arm, never_complete, fault_on_kick, gone, lose_device, bad_native, exhaust_native, wrong_base, allones, dead_reads;
uint64_t ktime_get_ns(void) { return clock_us*1000; }
void usleep_range(unsigned low, unsigned high) { assert(high>=low);clock_us+=low; }
void wmb(void) { __sync_synchronize(); }
static unsigned offset_of(const void *p)
{ assert((uintptr_t)p >= (uintptr_t)registers_ && (uintptr_t)p < (uintptr_t)registers_+sizeof(registers_));return (uintptr_t)p-(uintptr_t)registers_; }
uint32_t readl(const void *p) { assert(!gone && held && sealed);assert(!dead_reads);reads++;if(allones && published && offset_of(p)==0xc04){dead_reads=1;return ~0U;}return registers_[offset_of(p)/4]; }
void writel(uint32_t value, void *p)
{
    unsigned offset=offset_of(p);
    assert(!gone && !dead_reads && held && sealed && arms);writes++;
    assert(offset!=0x130 || !value);assert(offset!=0x110 || !value);
    registers_[offset/4]=value;
    if(offset==GMA_SGX_BOOT_KICK2) {
        assert(published && resets);
        if(fault_on_kick) { registers_[0xc04/4]=1;registers_[0xc08/4]=0xcafebabe; }
    }
}
int gma3600_sgx_owner_hardware(void *id, void **base)
{ assert(id==&live && held);if(gone)return -ENODEV;*base=registers_;return 0; }
int gma3600_sgx_owner_seal(void *id)
{ assert(id==&live && held && !sealed);seals++;if(fail_seal)return -EBUSY;sealed=1;return 0; }
int gma3600_sgx_native_live_prepare(void *runtime,void *base,struct gma_sgx_live_roots *r)
{ assert(runtime==&task && base==registers_ && held);*r=(struct gma_sgx_live_roots){0x12345000,0x22345000,0xc70,15,0};return fail_prepare ? -EINVAL : 0; }
int gma3600_sgx_native_live_arm(void *runtime)
{ assert(runtime==&task && sealed && !writes);arms++;return fail_arm ? -EINVAL : 0; }
int gma3600_sgx_native_live_reset(void *runtime,__u32 *pvr_error)
{
    unsigned value=0,i;
    assert(runtime==&task && sealed && arms);resets++;*pvr_error=0;
    assert(gma3600_sgx_live_write(registers_,0x80,0x3f)==1);
    assert(gma3600_sgx_live_write(registers_,0xc84,live.roots.reset_pd)==1);
    assert(gma3600_sgx_live_write(registers_,0xc70,live.roots.kernel_pd)==1);
    assert(gma3600_sgx_live_write(registers_,0xc78,live.roots.bank)==1);
    assert(gma3600_sgx_live_read(registers_,0xc04,&value)==1 && !value);
    test_current=&other;assert(!gma3600_sgx_live_write(registers_,0x80,0x3f));test_current=&task;
    test_in_interrupt=1;assert(!gma3600_sgx_live_write(registers_,0x80,0x3f));test_in_interrupt=0;
    if(wrong_base)assert(gma3600_sgx_live_write((char *)registers_+4,0x80,0x3f)==1);
    if(bad_native) assert(gma3600_sgx_live_write(registers_,0x7000,1)==1);
    if(exhaust_native)for(i=0;i<5000;i++)assert(gma3600_sgx_live_read(registers_,0xc04,&value)==1);
    assert(gma3600_sgx_live_write(registers_,0x80,0)==1);
    if(lose_device)gone=1;
    return 0;
}
int gma3600_sgx_native_live_publish(void *runtime)
{ assert(runtime==&task && sealed && resets);published++;return 0; }
int gma3600_sgx_native_live_status(void *runtime,__u32 *status)
{ assert(runtime==&task && sealed && published);polls++;*status=never_complete ? 0 : 1;return 0; }
static int acquire(void *owner) { assert(owner==&task && !held);held=1;return 0; }
static int verify(void *owner) { assert(owner==&task && held);return 0; }
static int release(void *owner) { assert(owner==&task && held && !sealed);held=0;releases++;return 0; }
static const struct gma_sgx_live_owner_ops ops={acquire,verify,release};
static void setup(void)
{
    /* Independent mock power cycles; production exposes no unseal operation. */
    memset(&live,0,sizeof(live));memset(registers_,0,sizeof(registers_));clock_us=1000;
    held=sealed=writes=reads=arms=resets=published=polls=releases=seals=0;
    fail_seal=fail_prepare=fail_arm=never_complete=fault_on_kick=gone=lose_device=bad_native=exhaust_native=wrong_base=allones=dead_reads=0;
}
static int start(void)
{ return gma_sgx_live_start(&live,&captured_info,captured_memory,15,&ops,&task,&live,&task); }
int main(void)
{
    unsigned before, value=77;
    setup();assert(!gma3600_sgx_live_read(registers_,0x80,&value) && value==77);
    assert(!start() && live.boot.state==GMA_SGX_BOOT_READY && live.boot.owned && live.sealed);
    assert(sealed && held && !releases && published && polls==1);
    assert(!gma3600_sgx_live_write(registers_,0x80,0)); /* Scoped reset ended. */
    assert(gma_sgx_boot_stop(&live.boot)==-EOPNOTSUPP && held && !releases);
    before=writes+reads;gma_sgx_live_detach(&live);
    assert(writes+reads==before && live.boot.owned && !live.attached);
    setup();fail_prepare=1;assert(start()==-EINVAL && !held && !writes && !sealed && releases==1);
    setup();fail_seal=1;assert(start()==-EBUSY && !held && !writes && !sealed && releases==1);
    setup();fail_arm=1;assert(start()==-EINVAL && held && sealed && !published && !releases);
    setup();never_complete=1;assert(start()==-ETIMEDOUT && held && sealed && !releases && live.boot.polls==10000);
    setup();fault_on_kick=1;assert(start()==-EIO && live.bif_fault==0xcafebabe && held && !releases);
    setup();bad_native=1;assert(start()==-EACCES && held && !published && !releases);
    setup();wrong_base=1;assert(start()==-EACCES && held && !published && !releases);
    setup();exhaust_native=1;assert(start()==-ETIMEDOUT && held && !published && !releases);
    setup();allones=1;assert(start()==-ENODEV && held && !releases && dead_reads);
    setup();lose_device=1;assert(start()==-ENODEV && held && !published && !releases);
    puts("SGX live provider tests passed: opt-in bridge boundary, seal-before-MMIO, thread/base/reset gate, status/fault/timeout, disconnect retention and lost-device containment (simulated hardware)");
    return 0;
}
