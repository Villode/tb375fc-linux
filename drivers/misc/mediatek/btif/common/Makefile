#
# Copyright (C) 2015 MediaTek Inc.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#

# BTIF driver for AD DIE
# If KERNELRELEASE is defined, we've been invoked from the
# kernel build system and can use its language.
ifneq ($(KERNELRELEASE),)
	# ⚠️ 这里原本是 -I$(srctree)/$(src)/inc。本树用 O= 出树编译时，
	# kbuild 传进来的 $(srctree) 与 $(src) **都是绝对路径**，拼出来是
	#   -I/home/.../linux-wifi-bt//home/.../linux-wifi-bt/drivers/.../inc
	# 一个不存在的路径 ⇒ mtk_btif.c 找不到 btif_pub.h，直接 fatal error。
	# 改成相对 srctree 的字面量，两种情形都对。
	ccflags-y += -I$(srctree)/drivers/misc/mediatek/btif/common/inc
	ccflags-y += -I$(srctree)/drivers/misc/mediatek/btif/common/plat_inc

	obj-$(CONFIG_MTK_BTIF) += btif_drv.o
	btif_drv-objs := mtk_btif.o mtk_btif_exp.o btif_dma_plat.o btif_plat.o

# Otherwise we were called directly from the command
# line; invoke the kernel build system.
else
    KERNELDIR ?= /lib/modules/$(shell uname -r)/build
    PWD  := $(shell pwd)
default:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules
endif
