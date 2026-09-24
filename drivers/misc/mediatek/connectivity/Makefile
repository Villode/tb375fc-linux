# SPDX-License-Identifier: GPL-2.0
#
# MT6895 connectivity stack (downstream CONNAC2X2 SOC7_0 fullmac port).
# Built-in only for the xaga mainline bring-up.

obj-y += connadp/
obj-y += conninfra/
obj-y += connfem/
obj-$(CONFIG_MTK_BTIF) += bt/mt66xx/btif/
obj-y += wlan_adaptor/
obj-y += wlan_drv_gen4m/

export CONFIG_MTK_COMBO_WIFI_HIF := axi
export CONNAC_VER := 2_0
export MTK_COMBO_CHIP := CONNAC2X2_SOC7_0
ifeq ($(CONFIG_MTK_COMBO_CHIP_CONSYS_6897),y)
export WLAN_CHIP_ID := 6897
else
export WLAN_CHIP_ID := 6895
endif
export MTK_ANDROID_WMT := y
export MTK_ANDROID_EMI := y
export WIFI_IP_SET := 1
export MTK_WLAN_SERVICE := yes
# Match the stock Android user build; mode 0 is invalid for the downstream
# conninfra firmware interface and was being written into CONN SYSRAM.
# ★ 2026-09-20：原来钉死成 user，导致 wlan_drv_gen4m/Makefile 走
#   `ccflags-y += -DBUILD_QA_DBG=0` ⇒ gl_kal.h 里 `kalPrint` 是空宏
#   ⇒ **所有 DBGLOG 编译成空语句**，驱动 probe 路径完全静默。
#   改成 eng 后 DBGLOG 才会进 dmesg（日志量由 aucDebugModule[] 控制）。
#   只让 MT6897 构建用 eng；6895 构建保持 user，避免全量调试日志。
ifeq ($(CONFIG_MTK_COMBO_CHIP_CONSYS_6897),y)
export TARGET_BUILD_VARIANT := eng
export MODULE_NAME := wlan_drv_gen4m_6897
else
export TARGET_BUILD_VARIANT := user
export MODULE_NAME := wlan_drv_gen4m_6895
endif
export CONFIG_MTK_CONNSYS_DEDICATED_LOG_PATH := y
# Tell the downstream wlan Makefile this is a built-in driver so it does not
# define symbols that mainline cfg80211 already provides (e.g. is_world_regdom).
export KBUILD_SUBDIR_CCFLAGS += MTK_WCN_REMOVE_KERNEL_MODULE
