// SPDX-License-Identifier: GPL-2.0-only
/* First hybrid-port hardware checkpoint: two identification reads, no writes. */
#include <linux/module.h>
#include <linux/slab.h>
#include "gma3600_sgx.h"

struct sgx_snapshot {
	u32 core_id;
	u32 core_rev;
	resource_size_t regs_phys;
};

static ssize_t identity_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	const struct sgx_snapshot *s = dev_get_drvdata(dev);

	return sysfs_emit(buf,
		"bar0_offset=0x%05x regs_phys=%pa core_id=0x%08x core_revision=0x%08x revision=%u.%u.%u register_state=%s\n",
		GMA3600_SGX_REG_OFFSET, &s->regs_phys, s->core_id, s->core_rev,
		(s->core_rev >> 16) & 0xff, (s->core_rev >> 8) & 0xff,
		s->core_rev & 0xff,
		(!s->core_id || s->core_id == ~0U || !s->core_rev ||
		 s->core_rev == ~0U) ? "unavailable" : "unverified");
}
static DEVICE_ATTR_RO(identity);

static struct attribute *sgx_attrs[] = {
	&dev_attr_identity.attr,
	NULL,
};
ATTRIBUTE_GROUPS(sgx);

static int gma3600_sgx_probe(struct auxiliary_device *adev,
			    const struct auxiliary_device_id *id)
{
	struct gma3600_sgx_device *sgx = container_of(adev,
					struct gma3600_sgx_device, adev);
	struct sgx_snapshot *s;

	if (sgx->host_abi != 2 && sgx->host_abi != GMA3600_SGX_HOST_ABI)
		return -EPROTONOSUPPORT;
	/* Do not wake/reset the GPU just to obtain an identification snapshot. */
	if (pci_channel_offline(sgx->pdev) || sgx->pdev->current_state != PCI_D0)
		return -EAGAIN;
	s = devm_kzalloc(&adev->dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->regs_phys = sgx->regs_phys;
	s->core_id = readl(sgx->regs + GMA3600_SGX_CORE_ID);
	s->core_rev = readl(sgx->regs + GMA3600_SGX_CORE_REV);
	dev_set_drvdata(&adev->dev, s);
	dev_info(&adev->dev, "SGX545 identification snapshot: ID=%08x REV=%08x (no reset or power writes)\n",
		 s->core_id, s->core_rev);
	return 0;
}

static const struct auxiliary_device_id gma3600_sgx_ids[] = {
	{ .name = "gma3600.sgx" },
	{ }
};
MODULE_DEVICE_TABLE(auxiliary, gma3600_sgx_ids);

static struct auxiliary_driver gma3600_sgx_driver = {
	.name = "gma3600_sgx_probe",
	.probe = gma3600_sgx_probe,
	.id_table = gma3600_sgx_ids,
	.driver = { .dev_groups = sgx_groups },
};
module_auxiliary_driver(gma3600_sgx_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("GMA3600 hybrid SGX545 identification probe (no rendering)");
MODULE_VERSION("0.2.0");
