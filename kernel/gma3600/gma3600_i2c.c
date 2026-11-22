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
				   struct i2c_msg *msg)
{
	u16 len = msg->len;
	u8 *buf = msg->buf;
	u32 cmd, val = 0;
	int ret;

	cmd = GMBUS_SW_RDY |
	      GMBUS_CYCLE_WAIT |
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
				    struct i2c_msg *msg)
{
	u16 len = msg->len;
	u8 *buf = msg->buf;
	u32 cmd, val = 0;
	int ret;

	cmd = GMBUS_SW_RDY |
	      GMBUS_CYCLE_WAIT |
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

	/* The first word is already queued. A one-byte EDID index write
	 * completes at WAIT_PHASE, not at another HW_RDY data request.
	 */
	buf += min_t(int, len, 4);
	len -= min_t(int, len, 4);
	while (len > 0) {
		int i, chunk = min_t(int, len, 4);

		val = 0;
		for (i = 0; i < chunk; i++)
			val |= (u32)buf[i] << (i * 8);
		gma3600_write(gdev, GMBUS3, val);
		ret = gma3600_gmbus_wait_ready(gdev);
		if (ret)
			return ret;
		buf += chunk;
		len -= chunk;
	}

	return 0;
}

static int gma3600_gmbus_xfer(struct i2c_adapter *adapter,
			      struct i2c_msg *msgs, int num)
{
	struct gma3600_gmbus *bus = container_of(adapter, struct gma3600_gmbus, adapter);
	struct gma3600_device *gdev = bus->gdev;
	int i, ret = 0;
	u32 val;

	for (i = 0; i < num; i++) {
		if (!msgs[i].len || msgs[i].len > GMBUS_BYTE_COUNT_MAX ||
		    msgs[i].addr > 0x7f ||
		    (msgs[i].flags & ~(I2C_M_RD | I2C_M_DMA_SAFE)))
			return -EOPNOTSUPP;
	}
	/* All pin pairs share the same hardware controller. */
	mutex_lock(&gdev->gmbus_lock);
	/* Select GMBUS pin pair and 100 kHz bus speed */
	gma3600_write(gdev, GMBUS0, bus->pin | GMBUS_RATE_100KHZ);

	for (i = 0; i < num; i++) {
		if (msgs[i].flags & I2C_M_RD)
			ret = gma3600_gmbus_xfer_read(gdev, &msgs[i]);
		else
			ret = gma3600_gmbus_xfer_write(gdev, &msgs[i]);

		if (!ret) {
			ret = read_poll_timeout(gma3600_read, val,
				val & (GMBUS_HW_WAIT_PHASE | GMBUS_SATOER),
				10, GMBUS_TIMEOUT_US, false, gdev, GMBUS2);
			if (!ret && (val & GMBUS_SATOER))
				ret = -ENXIO;
		}
		if (ret)
			break;
	}

	if (!ret) {
		gma3600_write(gdev, GMBUS1, GMBUS_CYCLE_STOP | GMBUS_SW_RDY);
		ret = read_poll_timeout(gma3600_read, val, !(val & GMBUS_ACTIVE),
			10, GMBUS_TIMEOUT_US, false, gdev, GMBUS2);
	}
	/* Toggle SW_CLR_INT; leaving it asserted breaks the next transfer. */
	gma3600_write(gdev, GMBUS1, GMBUS_SW_CLR_INT);
	gma3600_write(gdev, GMBUS1, 0);
	gma3600_write(gdev, GMBUS0, GMBUS_PIN_DISABLED);
	mutex_unlock(&gdev->gmbus_lock);

	return (ret == 0) ? num : ret;
}

static u32 gma3600_gmbus_functionality(struct i2c_adapter *adapter)
{
	return I2C_FUNC_I2C;
}

static const struct i2c_algorithm gma3600_gmbus_algo = {
	.master_xfer   = gma3600_gmbus_xfer,
	.functionality = gma3600_gmbus_functionality,
};

/* VGA DDC uses open-drain GPIOA, as in Linux gma500/intel_i2c.c.
 * A high level releases the line; never drive a monitor's DDC line high.
 */
static void gma3600_vga_set_line(void *data, int high, bool clock)
{
	struct gma3600_gmbus *bus = data;
	struct gma3600_device *gdev = bus->gdev;
	u32 reserved = gma3600_read(gdev, GPIOA) &
		(GPIO_CLOCK_PULLUP_DISABLE | GPIO_DATA_PULLUP_DISABLE);
	u32 mask = clock ? GPIO_CLOCK_DIR_MASK : GPIO_DATA_DIR_MASK;
	u32 drive = clock ? GPIO_CLOCK_DIR_OUT | GPIO_CLOCK_VAL_MASK :
		GPIO_DATA_DIR_OUT | GPIO_DATA_VAL_MASK;

	gma3600_write_post(gdev, GPIOA, reserved | mask | (high ? 0 : drive));
	udelay(20);
}

static void gma3600_vga_setscl(void *data, int high)
{
	gma3600_vga_set_line(data, high, true);
}

static void gma3600_vga_setsda(void *data, int high)
{
	gma3600_vga_set_line(data, high, false);
}

static int gma3600_vga_getscl(void *data)
{
	struct gma3600_gmbus *bus = data;

	return !!(gma3600_read(bus->gdev, GPIOA) & GPIO_CLOCK_VAL_IN);
}

static int gma3600_vga_getsda(void *data)
{
	struct gma3600_gmbus *bus = data;

	return !!(gma3600_read(bus->gdev, GPIOA) & GPIO_DATA_VAL_IN);
}

static int gma3600_vga_pre_xfer(struct i2c_adapter *adapter)
{
	struct gma3600_gmbus *bus = i2c_get_adapdata(adapter);

	mutex_lock(&bus->gdev->gmbus_lock);
	gma3600_write_post(bus->gdev, GMBUS0, GMBUS_PIN_DISABLED);
	gma3600_vga_setsda(bus, 1);
	gma3600_vga_setscl(bus, 1);
	return 0;
}

static void gma3600_vga_post_xfer(struct i2c_adapter *adapter)
{
	struct gma3600_gmbus *bus = i2c_get_adapdata(adapter);

	gma3600_vga_setsda(bus, 1);
	gma3600_vga_setscl(bus, 1);
	mutex_unlock(&bus->gdev->gmbus_lock);
}

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

	mutex_init(&gdev->gmbus_lock);

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

		if (pin == GMA3600_GMBUS_VGADDC) {
			bus->bit.data = bus;
			bus->bit.setsda = gma3600_vga_setsda;
			bus->bit.setscl = gma3600_vga_setscl;
			bus->bit.getsda = gma3600_vga_getsda;
			bus->bit.getscl = gma3600_vga_getscl;
			bus->bit.pre_xfer = gma3600_vga_pre_xfer;
			bus->bit.post_xfer = gma3600_vga_post_xfer;
			bus->bit.udelay = 20;
			bus->bit.timeout = usecs_to_jiffies(2200);
			bus->adapter.algo_data = &bus->bit;
			ret = i2c_bit_add_bus(&bus->adapter);
		} else {
			ret = i2c_add_adapter(&bus->adapter);
		}
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
