// SPDX-License-Identifier: GPL-2.0
//
// MT6897 gpusys (gpu_eb_rpc) power domains — Mali GPU MFG1..MFG14.
//
// Reverse-engineered from the vendor mtk-scpsys-mt6897.ko (no source available).
// See reports/fixes/20260917-gpu-panthor-bringup-status.md.
//
// ⚠ 两个坑（都已修）：
//   * of_device_id 必须带 .data = &mt6897_gpusys_data，否则
//     of_device_get_match_data() 返回 NULL，probe 里 soc->domains 直接崩，
//     表现为开机卡在 boot logo（late_initcall 崩溃）。
//   * 用 late_initcall（arch_initcall 太早，init_scp 里的
//     devm_clk_get/devm_regulator_get_optional 会在子系统就绪前失败）。
//
// 数据：
//   base 0x13f91000, pwr_sta 0xFC0 / pwr_sta2nd 0xFC4, 12 个域 MFG1..MFG14,
//   ctl_offs 0x70/0xA0/0xA4/0xA8/0xB0/0xB4/0xBC/0xC0/0xC4/0xC8/0xCC/0xD0,
//   caps=BYPASS_INIT_ON|IS_PWR_CON_ON, sram_pdn=0x100/ack=0x1000,
//   子域链 mfg1->mfg2->...->mfg14。
//   ⚠ bp_table 暂空（总线保护类型映射未定死），故**尚不能给 GPU 挂 power-domains**。

#include <linux/clk.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mfd/syscon.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_opp.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>

#include "scpsys.h"
#include "mtk-scpsys.h"

#ifndef MTK_SCPD_BYPASS_INIT_ON
#define MTK_SCPD_BYPASS_INIT_ON	BIT(7)
#endif
#ifndef MTK_SCPD_IS_PWR_CON_ON
#define MTK_SCPD_IS_PWR_CON_ON	BIT(8)
#endif

#define MFG_CAPS	(MTK_SCPD_BYPASS_INIT_ON | MTK_SCPD_IS_PWR_CON_ON)

/*
 * 总线保护表（每个 mfg 域相同），反推自厂商 mtk-scpsys-mt6897.ko：
 * 厂商 bp 条目 stride=0x20，字段 {type, set, clr, en, sta, mask, mask_ack, ignore}；
 * 我们用主线 struct bus_prot {type,set,clr,en,sta,mask,ignore_clr_ack}
 * （厂商 mask_ack == mask，可合并）。type 枚举与主线一致：IFR=1 / SMI=2。
 *   type=IFR  -> scp->infracfg（DT 指向 ifr_bus 0x1002c000）
 *   type=SMI  -> scp->smi_common（DT 指向 gpu_eb_rpc 0x13f91000）
 *
 * ⚠ 两个此前写错的点：
 *   1) 顺序：厂商是 [SMI 0x1A4, IFR 0x40, SMI 0x124, SMI 0x104]，
 *      我们原先把 IFR 放最前。clear 是倒序遍历，顺序会影响时序。
 *   2) ignore_clr_ack：厂商 4 条全是 1（BUS_PROT_IGN 语义）。我们原先默认
 *      false，clear_bus_protection() 会去轮询 sta 位，导致超时甚至挂死。
 */
#define MFG_BP_TABLE {						\
		{ SMI_TYPE, 0x1A4, 0x1A8, 0x1A0, 0x1AC, 0xF,      true }, \
		{ IFR_TYPE, 0x40,  0x44,  0x40,  0x48,  0xF0000,  true }, \
		{ SMI_TYPE, 0x124, 0x128, 0x120, 0x12C, 0x180000, true }, \
		{ SMI_TYPE, 0x104, 0x108, 0x100, 0x10C, 0x180000, true }, \
	}

/*
 * ⚠ sta_mask 必须是 0，不能抄 mt6895 的 GENMASK(31,30)。
 *   厂商 mtk-scpsys-mt6897.ko 的 gpu_eb_rpc 域数据里该字段为 0，caps=0x180
 *   (=BYPASS_INIT_ON|IS_PWR_CON_ON)。而 scpsys_power_on() 在
 *   IS_PWR_CON_ON 分支用 scpsys_pwr_con_is_on() 判定：
 *        status = readl(ctl_addr) & sta_mask;  return status == sta_mask;
 *   gpusys 的 ctl(0x13f91070) 读回 0x3312，& GENMASK(31,30) 恒为 0
 *   ⇒ 轮询永不成立 ⇒ "Failed to power on mtcmos mfg1(-110)"。
 *   sta_mask=0 时 0==0 恒真，与厂商行为一致。
 */
#define MFG_DOMAIN(_name, _ctl) {			\
		.name = _name,				\
		.sta_mask = 0,				\
		.ctl_offs = _ctl,			\
		.sram_pdn_bits = GENMASK(8, 8),		\
		.sram_pdn_ack_bits = GENMASK(12, 12),	\
		.caps = MFG_CAPS,			\
		.bp_table = MFG_BP_TABLE,		\
	}

static const struct scp_domain_data scp_domain_data_mt6897_gpusys[] = {
	[0]  = MFG_DOMAIN("mfg1",  0x70),
	[1]  = MFG_DOMAIN("mfg2",  0xA0),
	[2]  = MFG_DOMAIN("mfg3",  0xA4),
	[3]  = MFG_DOMAIN("mfg4",  0xA8),
	[4]  = MFG_DOMAIN("mfg6",  0xB0),
	[5]  = MFG_DOMAIN("mfg7",  0xB4),
	[6]  = MFG_DOMAIN("mfg9",  0xBC),
	[7]  = MFG_DOMAIN("mfg10", 0xC0),
	[8]  = MFG_DOMAIN("mfg11", 0xC4),
	[9]  = MFG_DOMAIN("mfg12", 0xC8),
	[10] = MFG_DOMAIN("mfg13", 0xCC),
	[11] = MFG_DOMAIN("mfg14", 0xD0),
};

static const struct scp_subdomain scp_subdomain_mt6897_gpusys[] = {
	{0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 5}, {5, 6},
	{6, 7}, {7, 8}, {8, 9}, {9, 10}, {10, 11},
};

static const struct scp_soc_data mt6897_gpusys_data = {
	.domains = scp_domain_data_mt6897_gpusys,
	.num_domains = ARRAY_SIZE(scp_domain_data_mt6897_gpusys),
	.subdomains = scp_subdomain_mt6897_gpusys,
	.num_subdomains = ARRAY_SIZE(scp_subdomain_mt6897_gpusys),
	.regs = {
		.pwr_sta_offs = 0xFC0,
		.pwr_sta2nd_offs = 0xFC4,
	}
};

static int mt6897_gpusys_probe(struct platform_device *pdev)
{
	const struct scp_subdomain *sd;
	const struct scp_soc_data *soc;
	struct scp *scp;
	struct genpd_onecell_data *pd_data;
	int i, ret;

	soc = of_device_get_match_data(&pdev->dev);
	if (!soc) {
		dev_err(&pdev->dev, "MT6897 gpusys: no match data!\n");
		return -ENODEV;
	}

	dev_info(&pdev->dev, "MT6897 gpusys: probe start (num_domains=%d)\n",
		 soc->num_domains);

	scp = init_scp(pdev, soc->domains, soc->num_domains, &soc->regs);
	if (IS_ERR(scp)) {
		dev_err(&pdev->dev, "MT6897 gpusys: init_scp failed %ld\n",
			PTR_ERR(scp));
		return PTR_ERR(scp);
	}

	ret = mtk_register_power_domains(pdev, scp, soc->num_domains);
	if (ret) {
		dev_err(&pdev->dev, "MT6897 gpusys: register failed %d\n", ret);
		return ret;
	}

	pd_data = &scp->pd_data;
	for (i = 0, sd = soc->subdomains; i < soc->num_subdomains; i++, sd++) {
		ret = pm_genpd_add_subdomain(pd_data->domains[sd->origin],
					     pd_data->domains[sd->subdomain]);
		if (ret && IS_ENABLED(CONFIG_PM))
			dev_err(&pdev->dev, "Failed to add subdomain: %d\n", ret);
	}

	dev_info(&pdev->dev, "MT6897 gpusys: registered %d MFG power domains\n",
		 soc->num_domains);
	return 0;
}

static const struct of_device_id of_scpsys_mt6897_gpusys_match[] = {
	{ .compatible = "mediatek,mt6897-gpusys",
	  .data = &mt6897_gpusys_data },
	{ /* sentinel */ }
};

static struct platform_driver mt6897_gpusys_drv = {
	.probe = mt6897_gpusys_probe,
	.driver = {
		.name = "mtk-scpsys-mt6897-gpusys",
		.suppress_bind_attrs = true,
		.of_match_table = of_scpsys_mt6897_gpusys_match,
	},
};

static int __init mt6897_gpusys_init(void)
{
	return platform_driver_register(&mt6897_gpusys_drv);
}
late_initcall(mt6897_gpusys_init);

/*
 * 探针：开机最早时读 GPUEB(0x13c00000) 与共享内存(0x7fe70000)，
 * 判断 bootloader 是否已经把 GPUEB 固件跑起来（在 genpd 关域之前）。
 */
static int __init gpueb_early_probe(void)
{
	void __iomem *b, *s;
	b = ioremap(0x13c00000, 0x1000);
	if (b) {
		pr_emerg("GPUEB-EARLY: 13c00000: +0=%08X +4=%08X +8=%08X +C=%08X +80=%08X +90=%08X\n",
			 readl(b + 0x0), readl(b + 0x4), readl(b + 0x8),
			 readl(b + 0xC), readl(b + 0x80), readl(b + 0x90));
		iounmap(b);
	}
	s = ioremap(0x7fe70000, 0x1000);
	if (s) {
		pr_emerg("GPUEB-EARLY: shmem: +0=%08X +4=%08X +8=%08X +C=%08X +10=%08X\n",
			 readl(s + 0x0), readl(s + 0x4), readl(s + 0x8),
			 readl(s + 0xC), readl(s + 0x10));
		iounmap(s);
	}
	return 0;
}
early_initcall(gpueb_early_probe);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MT6897 gpusys (gpu_eb_rpc) MFG power domains");
