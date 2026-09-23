// SPDX-License-Identifier: GPL-2.0
/*
 * MT6897 (Dimensity 8300 / Lenovo TB375FC) SPM power domains.
 *
 *   compatible = "mediatek,mt6897-scpsys"   (厂商 DT: power-controller@1c001000)
 *
 * ============================================================================
 * 反推来源（全部离线、可复现；见 reports/20260918-wifi-bt-bringup-status.md §8）
 * ============================================================================
 *   build/modules/ko/mtk-scpsys-mt6897.ko          纯数据模块（.text 仅 248 B）
 *   build/vendor-modules-.../mtk-scpsys.ko          core（init_scp / 注册）
 *   tools/vendor-forensics/dump_scpsys_domains.py   解析器
 *   ref/mt6897-scpsys-domains.json                  33 + 12 个域的完整数据
 *
 * 关键结论（别再重新推一遍）：
 *
 *  1) 厂商域条目 stride = **712 B**（主线 struct scp_domain_data 只有 ~296 B），
 *     所以不能按主线布局去读厂商表。字段偏移：
 *         +0x000 name  +0x010 sta_mask  +0x014 ctl_offs
 *         +0x038 sram_pdn_bits  +0x03C sram_pdn_ack_bits
 *         +0x0B0 caps  +0x0B4 bp_table[]（stride 0x20）
 *     conn = SPM 表（33 域）第 **1** 项；厂商 DT 里
 *     `consys { power-domains = <&scpsys 1>; }` 正好对上。
 *
 *  2) ★★ bp 的 `type` 字段**不是**主线的 enum regmap_type，而是
 *     `init_scp()` 第 5 个参数 `bus_list[]` 的**下标**（6 项，下标 0 保留）：
 *         [0] NULL     [1] gpu-eb-rpc(0x13f91000)  [2] ifr-bus(0x1002c000)
 *         [3] vlpcfg   [4] nemi-bus                [5] semi-bus
 *     映射回主线 struct bus_prot 的 type：
 *         1 -> MFGRPC_TYPE   2 -> IFR_TYPE   3 -> VLP_TYPE
 *         4 -> NEMI_TYPE     5 -> SEMI_TYPE
 *     （证据：mtk-scpsys-mt6897.ko 的 `mt6897_scpsys_probe` 里
 *       `mov w5,#0x6` + `adrp x4, bus_list`；bus_list 的 6 个指针重定位可直接读出；
 *       core 的 scpsys_bus_protect_disable 里 `regmap = scp->regmaps[bp.type]`，
 *       且填表从下标 1 开始 ⇒ type=0 表示"不动总线保护"。）
 *
 *  3) conn 的 4 条 bp **全是 type=2** ⇒ 全部落在 **ifr-bus（0x1002c000）**
 *     ⇒ 主线 IFR_TYPE（配 `infracfg = <&ifr_bus>`）。
 *     注意：这与 MT6895 的 CONN 形似而实不同（MT6895 的偏移是 0xC44/0xC54/0xC94）。
 *
 *  4) conn：sta_mask = 0、ctl_offs = 0xE04、无 SRAM 位、无时钟、无子域，
 *     caps = BYPASS_INIT_ON | IS_PWR_CON_ON（= 0x180，与厂商一致）。
 *     sta_mask 必须是 0：scpsys_power_on() 在 IS_PWR_CON_ON 分支用
 *     `(readl(ctl) & sta_mask) == sta_mask` 判定，0 时恒真 —— 与厂商行为一致
 *     （gpueb 那次把这里写成 GENMASK(31,30) 就永远轮询不通过）。
 *
 *  5) SPM 描述符：base 0x1C001000、pwr_sta_offs = 0xFB0、pwr_sta2nd_offs = 0xFB4。
 *     （厂商 `struct scp_desc` 实测：+0x08 num_domains、+0x18 num_subdomains、
 *       +0x1C pwr_sta_offs、+0x20 pwr_sta2nd_offs。）
 *
 * ⚠️ 本驱动**只注册 conn**（唯一有消费者的域）。其余 32 个域的数据都在
 *    ref/mt6897-scpsys-domains.json 里，需要时按同样方式补进来。
 */

#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/slab.h>

#include "scpsys.h"
#include "mtk-scpsys.h"

enum {
	MT6897_POWER_DOMAIN_CONN = 0,
	MT6897_POWER_DOMAIN_NR,
};

/* 与厂商 caps = 0x180 对齐 */
#define MT6897_CONN_CAPS	(MTK_SCPD_BYPASS_INIT_ON | MTK_SCPD_IS_PWR_CON_ON)

/*
 * conn 的总线保护表 —— 厂商数据里 4 条 bp 全部 type=2 ⇒ ifr-bus。
 * 顺序即厂商表顺序，不能重排：clear 是倒序遍历，顺序影响时序。
 * 厂商字段 {type,set,clr,en,sta,mask,mask_ack,ignore}；mask_ack == mask，
 * 合并成主线的 struct bus_prot{type,set,clr,en,sta,mask,ignore_clr_ack}。
 * ignore_clr_ack = 1（厂商 4 条全是 1）—— 置 false 会让 clear 去轮询 sta 位，
 * 导致超时甚至挂死（gpusys 那次的教训）。
 */
#define MT6897_CONN_BP_TABLE {						\
		BUS_PROT_IGN(IFR_TYPE, 0x0004, 0x0008, 0x0000, 0x000C,	\
			     0x02000000),					\
		BUS_PROT_IGN(IFR_TYPE, 0x01C4, 0x01C8, 0x01C0, 0x01CC,	\
			     0x00000002),					\
		BUS_PROT_IGN(IFR_TYPE, 0x0004, 0x0008, 0x0000, 0x000C,	\
			     0x04000000),					\
		BUS_PROT_IGN(IFR_TYPE, 0x01C4, 0x01C8, 0x01C0, 0x01CC,	\
			     0x00000001),					\
	}

static const struct scp_domain_data scp_domain_data_mt6897[] = {
	[MT6897_POWER_DOMAIN_CONN] = {
		.name = "conn",
		.sta_mask = 0,
		.ctl_offs = 0xE04,
		.sram_pdn_bits = 0,
		.sram_pdn_ack_bits = 0,
		.caps = MT6897_CONN_CAPS,
		.bp_table = MT6897_CONN_BP_TABLE,
	},
};

/*
 * 厂商 scp_subdomain_mt6897_spm 的 25 条里**没有 parent = 1**
 * ⇒ conn 是根域，不参与任何子域关系。
 */
static const struct scp_soc_data mt6897_data = {
	.domains = scp_domain_data_mt6897,
	.num_domains = MT6897_POWER_DOMAIN_NR,
	.subdomains = NULL,
	.num_subdomains = 0,
	.regs = {
		.pwr_sta_offs = 0xFB0,
		.pwr_sta2nd_offs = 0xFB4,
	},
};

static const struct of_device_id of_scpsys_match_tbl[] = {
	{
		.compatible = "mediatek,mt6897-scpsys",
		/* ⚠️ 必须带 .data！否则 of_device_get_match_data() 返回 NULL，
		 * probe 里 soc->domains 直接崩（gpusys 那次踩过，表现为开机卡 logo）。 */
		.data = &mt6897_data,
	}, {
		/* sentinel */
	}
};

static int mt6897_scpsys_probe(struct platform_device *pdev)
{
	const struct scp_soc_data *soc;
	struct scp *scp;
	int ret;

	soc = of_device_get_match_data(&pdev->dev);
	if (!soc) {
		dev_err(&pdev->dev, "no match data\n");
		return -EINVAL;
	}

	dev_info(&pdev->dev,
		 "MT6897-SCPSYS: probe enter (num_domains=%d)\n",
		 soc->num_domains);

	scp = init_scp(pdev, soc->domains, soc->num_domains, &soc->regs);
	if (IS_ERR(scp))
		return PTR_ERR(scp);

	ret = mtk_register_power_domains(pdev, scp, soc->num_domains);
	if (ret)
		return ret;

	dev_info(&pdev->dev, "MT6897-SCPSYS: registered %d power domains\n",
		 soc->num_domains);

	return 0;
}

static struct platform_driver mt6897_scpsys_drv = {
	.probe = mt6897_scpsys_probe,
	.driver = {
		.name = "mtk-scpsys-mt6897",
		.suppress_bind_attrs = true,
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(of_scpsys_match_tbl),
	},
};

/*
 * ★★ arch_initcall —— 不能是 late_initcall（2026-09-18 实测教训）
 *
 * 最初为规避 gpusys 的坑用了 late_initcall（"arch_initcall 太早"）。那个教训
 * 只适用于**域自带时钟/稳压器**的情况；conn 的域数据里没有任何 clk / supply，
 * init_scp 里那两个循环会直接跳过，所以 arch_initcall 完全安全。
 *
 * 而 late_initcall 有致命后果：它排在 **device_initcall 之后**，因而
 *    1) scpsys 注册 conn genpd 时，consys/wifi 这些 device_initcall 早跑完了；
 *    2) consys 的 probe 因为拿不到 power domain 返回 -EPROBE_DEFER 被推迟；
 *    3) **但 wifi 驱动（wlan_drv_gen4m 的 axi_probe）照常 probe，会立刻去访问
 *       0x18000000 的 WLAN CSR** —— 此时 CONN MTCMOS 还没上电
 *       ⇒ AXI 总线停摆 ⇒ 卡在 boot logo（8 企鹅）、无 USB/COM/ping。
 * 这与"连接性一开就卡、不开就正常"的实测现象完全吻合。
 *
 * 参考移植 mt6895 用的就是 arch_initcall(mt6895_scpsys_init)，此处对齐。
 */
static int __init mt6897_scpsys_init(void)
{
	int ret;

	pr_info("MT6897-SCPSYS: driver init enter\n");
	ret = platform_driver_register(&mt6897_scpsys_drv);
	pr_info("MT6897-SCPSYS: driver init ret=%d\n", ret);
	return ret;
}

static void __exit mt6897_scpsys_exit(void)
{
	platform_driver_unregister(&mt6897_scpsys_drv);
}

arch_initcall(mt6897_scpsys_init);
module_exit(mt6897_scpsys_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MediaTek MT6897 SPM power domains (conn)");
