// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6897 Pinctrl Driver — GPIO + EINT subset.
 *
 * Register layout of the gpio bank (0x10005000) matches mt6895:
 *   DIR  @ 0x0000, 1 bit/pin, 32 pins per 0x10 step
 *   DOUT @ 0x0100, DIN @ 0x0200, MODE @ 0x0300, 4 bits/pin, 8 pins per 0x10
 * (verified on TB375FC hardware via direct MMIO).
 *
 * The per-pin iocfg tables (pull/drive/smt/ies) are not yet known — the
 * vendor pinctrl-mt6897.ko is binary-only — so only MODE/DIR/DI/DO ranges
 * are provided. Pinconf ops for those fields return -ENOTSUPP, which is
 * fine for GPIO and EINT consumers. Pinmux is handled by LK/preloader on
 * TB375FC anyway.
 *
 * EINT data (pin -> instance/index matrix, 5 banks, 250 lines) was taken
 * verbatim from the vendor DT's apirq@11ce0000 `mediatek,pins` table.
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>

#include "pinctrl-mtk-common-v2.h"
#include "pinctrl-mtk-mt6897.h"
#include "pinctrl-paris.h"
#include "mtk-eint.h"

#define PIN_FIELD_BASE(s_pin, e_pin, i_base, s_addr, x_addrs, s_bit, x_bits) \
	PIN_FIELD_CALC(s_pin, e_pin, i_base, s_addr, x_addrs, s_bit, x_bits, \
			32, 0)

static const struct mtk_pin_field_calc mt6897_pin_mode_range[] = {
	PIN_FIELD_BASE(0, 7, 0, 0x0300, 0x10, 0, 4),
	PIN_FIELD_BASE(8, 15, 0, 0x0310, 0x10, 0, 4),
	PIN_FIELD_BASE(16, 23, 0, 0x0320, 0x10, 0, 4),
	PIN_FIELD_BASE(24, 31, 0, 0x0330, 0x10, 0, 4),
	PIN_FIELD_BASE(32, 39, 0, 0x0340, 0x10, 0, 4),
	PIN_FIELD_BASE(40, 47, 0, 0x0350, 0x10, 0, 4),
	PIN_FIELD_BASE(48, 55, 0, 0x0360, 0x10, 0, 4),
	PIN_FIELD_BASE(56, 63, 0, 0x0370, 0x10, 0, 4),
	PIN_FIELD_BASE(64, 71, 0, 0x0380, 0x10, 0, 4),
	PIN_FIELD_BASE(72, 79, 0, 0x0390, 0x10, 0, 4),
	PIN_FIELD_BASE(80, 87, 0, 0x03a0, 0x10, 0, 4),
	PIN_FIELD_BASE(88, 95, 0, 0x03b0, 0x10, 0, 4),
	PIN_FIELD_BASE(96, 103, 0, 0x03c0, 0x10, 0, 4),
	PIN_FIELD_BASE(104, 111, 0, 0x03d0, 0x10, 0, 4),
	PIN_FIELD_BASE(112, 119, 0, 0x03e0, 0x10, 0, 4),
	PIN_FIELD_BASE(120, 127, 0, 0x03f0, 0x10, 0, 4),
	PIN_FIELD_BASE(128, 135, 0, 0x0400, 0x10, 0, 4),
	PIN_FIELD_BASE(136, 143, 0, 0x0410, 0x10, 0, 4),
	PIN_FIELD_BASE(144, 151, 0, 0x0420, 0x10, 0, 4),
	PIN_FIELD_BASE(152, 159, 0, 0x0430, 0x10, 0, 4),
	PIN_FIELD_BASE(160, 167, 0, 0x0440, 0x10, 0, 4),
	PIN_FIELD_BASE(168, 175, 0, 0x0450, 0x10, 0, 4),
	PIN_FIELD_BASE(176, 183, 0, 0x0460, 0x10, 0, 4),
	PIN_FIELD_BASE(184, 191, 0, 0x0470, 0x10, 0, 4),
	PIN_FIELD_BASE(192, 199, 0, 0x0480, 0x10, 0, 4),
	PIN_FIELD_BASE(200, 207, 0, 0x0490, 0x10, 0, 4),
	PIN_FIELD_BASE(208, 215, 0, 0x04a0, 0x10, 0, 4),
	PIN_FIELD_BASE(216, 223, 0, 0x04b0, 0x10, 0, 4),
	PIN_FIELD_BASE(224, 231, 0, 0x04c0, 0x10, 0, 4),
};

static const struct mtk_pin_field_calc mt6897_pin_dir_range[] = {
	PIN_FIELD_BASE(0, 31, 0, 0x0000, 0x10, 0, 1),
	PIN_FIELD_BASE(32, 63, 0, 0x0010, 0x10, 0, 1),
	PIN_FIELD_BASE(64, 95, 0, 0x0020, 0x10, 0, 1),
	PIN_FIELD_BASE(96, 127, 0, 0x0030, 0x10, 0, 1),
	PIN_FIELD_BASE(128, 159, 0, 0x0040, 0x10, 0, 1),
	PIN_FIELD_BASE(160, 191, 0, 0x0050, 0x10, 0, 1),
	PIN_FIELD_BASE(192, 223, 0, 0x0060, 0x10, 0, 1),
	PIN_FIELD_BASE(224, 231, 0, 0x0070, 0x10, 0, 1),
};

static const struct mtk_pin_field_calc mt6897_pin_di_range[] = {
	PIN_FIELD_BASE(0, 31, 0, 0x0200, 0x10, 0, 1),
	PIN_FIELD_BASE(32, 63, 0, 0x0210, 0x10, 0, 1),
	PIN_FIELD_BASE(64, 95, 0, 0x0220, 0x10, 0, 1),
	PIN_FIELD_BASE(96, 127, 0, 0x0230, 0x10, 0, 1),
	PIN_FIELD_BASE(128, 159, 0, 0x0240, 0x10, 0, 1),
	PIN_FIELD_BASE(160, 191, 0, 0x0250, 0x10, 0, 1),
	PIN_FIELD_BASE(192, 223, 0, 0x0260, 0x10, 0, 1),
	PIN_FIELD_BASE(224, 231, 0, 0x0270, 0x10, 0, 1),
};

static const struct mtk_pin_field_calc mt6897_pin_do_range[] = {
	PIN_FIELD_BASE(0, 31, 0, 0x0100, 0x10, 0, 1),
	PIN_FIELD_BASE(32, 63, 0, 0x0110, 0x10, 0, 1),
	PIN_FIELD_BASE(64, 95, 0, 0x0120, 0x10, 0, 1),
	PIN_FIELD_BASE(96, 127, 0, 0x0130, 0x10, 0, 1),
	PIN_FIELD_BASE(128, 159, 0, 0x0140, 0x10, 0, 1),
	PIN_FIELD_BASE(160, 191, 0, 0x0150, 0x10, 0, 1),
	PIN_FIELD_BASE(192, 223, 0, 0x0160, 0x10, 0, 1),
	PIN_FIELD_BASE(224, 231, 0, 0x0170, 0x10, 0, 1),
};

static const struct mtk_pin_reg_calc mt6897_reg_cals[PINCTRL_PIN_REG_MAX] = {
	[PINCTRL_PIN_REG_MODE] = MTK_RANGE(mt6897_pin_mode_range),
	[PINCTRL_PIN_REG_DIR] = MTK_RANGE(mt6897_pin_dir_range),
	[PINCTRL_PIN_REG_DI] = MTK_RANGE(mt6897_pin_di_range),
	[PINCTRL_PIN_REG_DO] = MTK_RANGE(mt6897_pin_do_range),
};

static const char * const mt6897_pinctrl_register_base_names[] = {
	"gpio", "iocfg_rt", "iocfg_rm", "iocfg_bl", "iocfg_bm",
	"iocfg_br", "iocfg_lb", "iocfg_lt", "iocfg_tm", "iocfg_tl",
};

/*
 * MT6897 EINT is a separate multi-instance block (apirq@11ce0000) with five
 * banks (eint-e/s/w/n/c, vendor reg-name order). The EINT number ->
 * (instance, index) matrix comes from the stock DT's `mediatek,pins` array:
 * EINTs 0-122 map 1:1 to GPIOs, 232-249 are virtual lines (PMIC/SPMI
 * domain) living on bank 4 (eint-c). Missing entries are marked 0xff so
 * the core skips them.
 */
static const struct mtk_eint_hw mt6897_eint_hw = {
	.port_mask = 0xf,
	.ports     = 5,
	.ap_num    = 250,
	.db_cnt    = 32,
	.db_time   = debounce_time_mt6878,
};

static struct mtk_eint_pin mt6897_eint_pin[] = {
#include "mt6897-eint-pin.inc"
};

static const struct mtk_pin_soc mt6897_data = {
	.reg_cal = mt6897_reg_cals,
	.pins = mtk_pins_mt6897,
	.npins = ARRAY_SIZE(mtk_pins_mt6897),
	.ngrps = ARRAY_SIZE(mtk_pins_mt6897),
	.eint_hw = &mt6897_eint_hw,
	.eint_pin = mt6897_eint_pin,
	.nfuncs = 8,
	.gpio_m = 0,
	.base_names = mt6897_pinctrl_register_base_names,
	.nbase_names = ARRAY_SIZE(mt6897_pinctrl_register_base_names),
};

static const struct of_device_id mt6897_pinctrl_of_match[] = {
	{ .compatible = "mediatek,mt6897-pinctrl", .data = &mt6897_data },
	{ /* sentinel */ }
};

static struct platform_driver mt6897_pinctrl_driver = {
	.driver = {
		.name = "mt6897-pinctrl",
		.of_match_table = mt6897_pinctrl_of_match,
		.pm = pm_sleep_ptr(&mtk_paris_pinctrl_pm_ops),
	},
	.probe = mtk_paris_pinctrl_probe,
};

static int __init mt6897_pinctrl_init(void)
{
	return platform_driver_register(&mt6897_pinctrl_driver);
}
arch_initcall(mt6897_pinctrl_init);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MediaTek MT6897 Pinctrl Driver");
