// SPDX-License-Identifier: GPL-2.0-only
/*
 * Intel GMA 3600 (Cedar Trail) DRM/KMS Driver — GMBUS I2C / DDC
 *
 * Copyright (C) 2026 Antigravity Project
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/iopoll.h>

#include "gma3600_drv.h"
#include "gma3600_reg.h"

#define GMBUS_TIMEOUT_US	50000

static int gma3600_gmbus_wait_ready(struct gma3600_device *gdev)
{
	u32 val;
	int ret;

	ret = read_poll_timeout_atomic(gma3600_read, val,
				       (val & (GMBUS_HW_RDY | GMBUS_SATOER)),
				       10, GMBUS_TIMEOUT_US, false,
				       gdev, GMBUS2);
	if (ret)
		return ret;

	if (val & GMBUS_SATOER)
		return -ENXIO;

	return 0;
}

static int gma3600_gmbus_xfer_read(struct gma3600_device *gdev,
				   struct i2c_msg *msg, bool is_last)
{
	u16 len = msg->len;
	u8 *buf = msg->buf;
	u32 cmd, val = 0;
	int ret;

	cmd = GMBUS_SW_RDY |
	      (is_last ? GMBUS_CYCLE_STOP : GMBUS_CYCLE_WAIT) |
	      ((len & 0x1FF) << GMBUS_BYTE_COUNT_SHIFT) |
	      ((msg->addr & 0x7F) << GMBUS_SLAVE_ADDR_SHIFT) |
	      GMBUS_SLAVE_READ;

	gma3600_write(gdev, GMBUS1, cmd);

	while (len > 0) {
		int i;

		ret = gma3600_gmbus_wait_ready(gdev);
		if (ret)
			return ret;

		val = gma3600_read(gdev, GMBUS3);
		for (i = 0; i < 4 && len > 0; i++, len--) {
			*buf++ = val & 0xFF;
			val >>= 8;
		}
	}

	return 0;
}

static int gma3600_gmbus_xfer_write(struct gma3600_device *gdev,
				    struct i2c_msg *msg, bool is_last)
{
	u16 len = msg->len;
	u8 *buf = msg->buf;
	u32 cmd, val = 0;
	int ret;

	cmd = GMBUS_SW_RDY |
	      (is_last ? GMBUS_CYCLE_STOP : GMBUS_CYCLE_WAIT) |
	      ((len & 0x1FF) << GMBUS_BYTE_COUNT_SHIFT) |
	      ((msg->addr & 0x7F) << GMBUS_SLAVE_ADDR_SHIFT) |
	      GMBUS_SLAVE_WRITE;

	/* Preload first 4 bytes of data if present */
	if (len > 0) {
		int i;
		for (i = 0; i < 4 && i < len; i++)
			val |= ((u32)buf[i]) << (i * 8);
		gma3600_write(gdev, GMBUS3, val);
	}

	gma3600_write(gdev, GMBUS1, cmd);

	while (len > 0) {
		int chunk = min_t(int, len, 4);
		int i;

		ret = gma3600_gmbus_wait_ready(gdev);
		if (ret)
			return ret;

		buf += chunk;
		len -= chunk;

		if (len > 0) {
			val = 0;
			for (i = 0; i < 4 && i < len; i++)
				val |= ((u32)buf[i]) << (i * 8);
			gma3600_write(gdev, GMBUS3, val);
		}
	}

	return 0;
}

static int gma3600_gmbus_xfer(struct i2c_adapter *adapter,
			      struct i2c_msg *msgs, int num)
{
	struct gma3600_gmbus *bus = container_of(adapter, struct gma3600_gmbus, adapter);
	struct gma3600_device *gdev = bus->gdev;
	int i, ret = 0;

	/* Select GMBUS pin pair and 100 kHz bus speed */
	gma3600_write(gdev, GMBUS0, bus->pin | GMBUS_RATE_100KHZ);

	for (i = 0; i < num; i++) {
		bool is_last = (i == num - 1);

		if (msgs[i].flags & I2C_M_RD)
			ret = gma3600_gmbus_xfer_read(gdev, &msgs[i], is_last);
		else
			ret = gma3600_gmbus_xfer_write(gdev, &msgs[i], is_last);

		if (ret)
			break;
	}

	/* Clear and reset GMBUS port to idle */
	gma3600_write(gdev, GMBUS0, GMBUS_PIN_DISABLED);
	gma3600_write(gdev, GMBUS1, GMBUS_SW_CLR_INT);

	return (ret == 0) ? num : ret;
}

static u32 gma3600_gmbus_functionality(struct i2c_adapter *adapter)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm gma3600_gmbus_algo = {
	.master_xfer   = gma3600_gmbus_xfer,
	.functionality = gma3600_gmbus_functionality,
};

static const char *gma3600_gmbus_name(enum gma3600_gmbus_pin pin)
{
	switch (pin) {
	case GMA3600_GMBUS_VGADDC:
		return "gma3600-vgaddc";
	case GMA3600_GMBUS_PANEL:
		return "gma3600-panel";
	case GMA3600_GMBUS_DPB:
		return "gma3600-dpb";
	case GMA3600_GMBUS_DPC:
		return "gma3600-dpc";
	case GMA3600_GMBUS_DPD:
		return "gma3600-dpd";
	default:
		return "gma3600-gmbus";
	}
}

int gma3600_i2c_init(struct gma3600_device *gdev)
{
	static const enum gma3600_gmbus_pin active_pins[] = {
		GMA3600_GMBUS_VGADDC,
		GMA3600_GMBUS_PANEL,
		GMA3600_GMBUS_DPB,
	};
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(active_pins); i++) {
		enum gma3600_gmbus_pin pin = active_pins[i];
		struct gma3600_gmbus *bus = &gdev->gmbus[pin];

		bus->pin = pin;
		bus->gdev = gdev;
		bus->adapter.owner = THIS_MODULE;
		bus->adapter.class = I2C_CLASS_HWMON;
		bus->adapter.algo = &gma3600_gmbus_algo;
		bus->adapter.dev.parent = &gdev->pdev->dev;
		strscpy(bus->adapter.name, gma3600_gmbus_name(pin),
			sizeof(bus->adapter.name));

		i2c_set_adapdata(&bus->adapter, bus);

		ret = i2c_add_adapter(&bus->adapter);
		if (ret) {
			dev_err(&gdev->pdev->dev, "Failed to add I2C adapter %s: %d\n",
				bus->adapter.name, ret);
			while (--i >= 0)
				i2c_del_adapter(&gdev->gmbus[active_pins[i]].adapter);
			return ret;
		}
	}

	return 0;
}

void gma3600_i2c_fini(struct gma3600_device *gdev)
{
	static const enum gma3600_gmbus_pin active_pins[] = {
		GMA3600_GMBUS_VGADDC,
		GMA3600_GMBUS_PANEL,
		GMA3600_GMBUS_DPB,
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(active_pins); i++) {
		enum gma3600_gmbus_pin pin = active_pins[i];
		if (gdev->gmbus[pin].adapter.dev.parent)
			i2c_del_adapter(&gdev->gmbus[pin].adapter);
	}
}

struct i2c_adapter *gma3600_i2c_get_adapter(struct gma3600_device *gdev,
					    enum gma3600_gmbus_pin pin)
{
	if (pin >= GMA3600_NUM_GMBUS_PORTS || !gdev->gmbus[pin].adapter.dev.parent)
		return NULL;

	return &gdev->gmbus[pin].adapter;
}
