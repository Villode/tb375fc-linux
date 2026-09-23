/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __MT6895_GPUEB_H
#define __MT6895_GPUEB_H

#include <linux/errno.h>

#if IS_ENABLED(CONFIG_MTK_GPUEB)
int mt6895_gpueb_power_control(unsigned int power_on);
int mt6895_gpueb_power_on(void);
bool mt6895_gpueb_available(void);
int mt6895_gpueb_commit(unsigned int target, unsigned int oppidx);
#else
static inline int mt6895_gpueb_power_control(unsigned int power_on) { return -ENODEV; }
static inline int mt6895_gpueb_power_on(void) { return -ENODEV; }
static inline bool mt6895_gpueb_available(void) { return false; }
static inline int mt6895_gpueb_commit(unsigned int target, unsigned int oppidx) { return -ENODEV; }
#endif

#endif
