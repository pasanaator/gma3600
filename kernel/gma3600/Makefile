# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for the Intel GMA 3600 (Cedar Trail) DRM Driver
#

ifneq ($(KERNELRELEASE),)

obj-m := gma3600.o

gma3600-y := gma3600_drv.o \
             gma3600_device.o \
             gma3600_gtt.o \
             gma3600_gem.o \
             gma3600_memory.o \
             gma3600_display.o \
             gma3600_dpll.o \
             gma3600_clock.o \
             gma3600_lvds.o \
             gma3600_crt.o \
             gma3600_hdmi.o \
             gma3600_i2c.o \
             gma3600_irq.o \
             gma3600_pm.o \
             gma3600_backlight.o \
             gma3600_fb.o

ccflags-y := -I$(src) -Wall -Wextra

else

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

default:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

endif
