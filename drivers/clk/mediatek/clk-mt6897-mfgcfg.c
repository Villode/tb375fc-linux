// SPDX-License-Identifier: GPL-2.0
//
// MT6897 mfgcfg clock controller + MFG bring-up.
//
// Ported from the working MT6895-Mainline `clk-mt6895-mfgcfg.c`
// (drivers/clk/mediatek/clk-mt6895-mfgcfg.c). MT6895 and MT6897 share the
// same MFG register layout (mfgcfg @0x13fbf000, mfg_rpc @0x13f90000,
// SPM/sleep @0x1c001000), so the bring-up sequence is identical.
//
// The GPU node's "coregroup" clock is the MFGCFG BG3D gate.  Without it
// clk_disable_unused() switches the gate off while the GPU runs and the
// resulting external abort shows up as an SError in panthor.
//
// mt6897_mfg_bringup_enable() is the AOC2.0 / HWDCM / ACP / GPM part of the
// downstream gpufreq power-on.  It must run after the MFG MTCMOS is enabled;
// on this port we call it from probe (best effort) and export it so a
// consumer can re-apply it after power-on.

#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <asm/pgtable.h>

#include "clk-mtk.h"
#include "clk-gate.h"

#define MT6897_MFGCFG_BASE	0x13fbf000
#define MT6897_MFG_RPC_BASE	0x13f90000
#define MT6897_SLEEP_BASE	0x1c001000

/* GPUEB blocks (vendor DT /soc/gpueb@13c00000 reg-names) */
#define MT6897_GPUEB_TCM_BASE	0x13c00000
#define MT6897_GPUEB_CFG_BASE	0x13c60000
#define MT6897_GPUEB_MBOX_BASE	0x13c62000

/*
 * Is the GPUEB (RV33 tinysys) firmware running?
 *
 * GPUEB_CFGREG_WDT_CON (0x13c60618) is set to 0x800f0000 by the firmware's
 * own wdt_init; GPUEB_MBOX_IPI_GPUEB (0x13c62000) is a live register once it
 * talks to the AP.  Both read 0 when GPUEB is not up.
 *
 * Sampling this at several initcall levels answers the only question that
 * matters: did the bootloader start GPUEB (then something killed it), or was
 * it never started at all?
 */
static void mt6897_gpueb_state(const char *tag)
{
	void __iomem *cfg, *tcm, *mb;
	u32 boot_ctl = 0xffffffff, wdt_con = 0xffffffff, irq_en = 0xffffffff;
	u32 t0 = 0xffffffff, t1 = 0xffffffff, m0 = 0xffffffff, m1 = 0xffffffff;

	cfg = ioremap(MT6897_GPUEB_CFG_BASE, 0x2000);
	if (cfg) {
		boot_ctl = readl(cfg + 0x600);
		wdt_con = readl(cfg + 0x618);
		irq_en = readl(cfg + 0x300);
		iounmap(cfg);
	}
	tcm = ioremap(MT6897_GPUEB_TCM_BASE, 0x10);
	if (tcm) {
		t0 = readl(tcm);
		t1 = readl(tcm + 0x4);
		iounmap(tcm);
	}
	mb = ioremap(MT6897_GPUEB_MBOX_BASE, 0x10);
	if (mb) {
		m0 = readl(mb);
		m1 = readl(mb + 0x4);
		iounmap(mb);
	}

	pr_info("MT6897-GPUEB[%s]: WDT_CON=%08X boot_ctl=%08X IRQ_EN=%08X TCM=%08X/%08X MBOX=%08X/%08X %s\n",
		tag, wdt_con, boot_ctl, irq_en, t0, t1, m0, m1,
		wdt_con == 0x800f0000 ? "*** ALIVE ***" : "(dead)");

	/*
	 * Kernel-side view of the MFG blocks.  READ-ONLY on purpose:
	 * revision 8 of this probe also wrote 0x13F91070 |= 4 / |= 8 (the
	 * vendor's MFG1 RPC power-on) and 0x13F91030 = 0x1F00 here, and GPUEB
	 * went from *** ALIVE *** at postcore (0.932 s) to (dead) at arch
	 * (0.950 s) - i.e. one of those writes killed the co-processor.
	 * The lethal write is isolated separately, at 20 s.
	 */
	{
		void __iomem *rpc, *cfg;

		rpc = ioremap(MT6897_MFG_RPC_BASE, 0x2000);
		if (rpc) {
			pr_info("MT6897-RPC[%s]: 1030=%08X 1034=%08X 1070=%08X\n",
				tag, readl(rpc + 0x1030), readl(rpc + 0x1034),
				readl(rpc + 0x1070));
			iounmap(rpc);
		}

		cfg = ioremap(MT6897_MFGCFG_BASE, 0x1000);
		if (cfg) {
			pr_info("MT6897-CFG[%s]: 000=%08X 004=%08X 008=%08X 168=%08X 500=%08X\n",
				tag, readl(cfg + 0x0), readl(cfg + 0x4),
				readl(cfg + 0x8), readl(cfg + 0x168),
				readl(cfg + 0x500));
			iounmap(cfg);
		}
	}
}

static int __init mt6897_gpueb_state_early(void)
{
	mt6897_gpueb_state("postcore");
	return 0;
}
postcore_initcall(mt6897_gpueb_state_early);

static int __init mt6897_gpueb_state_late(void)
{
	mt6897_gpueb_state("late");
	return 0;
}
late_initcall(mt6897_gpueb_state_late);

/*
 * Sample again well after the GPUEB driver has sent its first IPIs
 * (gpueb probe ~1.18 s, panthor's CMD_POWER_CONTROL ~1.38 s), then try to
 * open the MFG clock gate from the kernel side.
 *
 * Rationale: at postcore/arch/late (0.93-1.30 s) mfgcfg AND mfg_rpc both read
 * 0 from the kernel, while /dev/mem reads LK's values out of mfg_rpc later.
 * A clock-gated block returns 0 but keeps its contents; that fits "MFG clock
 * comes up only after GPUEB is asked to power the GPU".
 */
static void mt6897_gpueb_late_probe(struct work_struct *w)
{
	void __iomem *cfg, *rpc, *wt;
	u32 sta;

	mt6897_gpueb_state("20s");

	/*
	 * Same physical word through two different mapping types.  /dev/mem
	 * maps MMIO as Normal-NonCacheable; ioremap() gives Device-nGnRE.
	 * If only one of them returns LK's value, the driver must use that
	 * mapping type for the MFG blocks.
	 */
	rpc = ioremap(MT6897_MFG_RPC_BASE, 0x2000);
	if (rpc) {
		pr_info("MT6897-MAPTYPE[dev ]: 070=%08X 030=%08X 034=%08X\n",
			readl(rpc + 0x70), readl(rpc + 0x30), readl(rpc + 0x34));
		iounmap(rpc);
	}
	wt = memremap(MT6897_MFG_RPC_BASE, 0x2000, MEMREMAP_WT);
	if (wt) {
		pr_info("MT6897-MAPTYPE[wt  ]: 070=%08X 030=%08X 034=%08X\n",
			readl(wt + 0x70), readl(wt + 0x30), readl(wt + 0x34));
		memunmap(wt);
	} else {
		pr_info("MT6897-MAPTYPE[wt  ]: memremap failed\n");
	}

	cfg = ioremap(MT6897_MFGCFG_BASE, 0x1000);
	if (!cfg)
		return;

	sta = readl(cfg + 0x0);
	writel(0x1, cfg + 0x4);			/* BG3D gate: set bit0 */
	udelay(10);
	pr_info("MT6897-MFGCFG[gate]: sta_before=%08X sta_after=%08X (bit0=%d) 004=%08X 168=%08X 500=%08X\n",
		sta, readl(cfg + 0x0), readl(cfg + 0x0) & 1,
		readl(cfg + 0x4), readl(cfg + 0x168), readl(cfg + 0x500));
	iounmap(cfg);

	/*
	 * topckgen only - the earlier mapping-type A/B block is gone: it read
	 * 0x13F90070 instead of 0x13F91070, so its "all mapping types read 0"
	 * result was an artefact of the wrong offset.  The corrected MFG checks
	 * live in mt6897_gpueb_state().
	 */
	/*
	 * ★ 隔离结果（2026-09-18，probe9 实测）：
	 *     20s-B  0x13F91070 |= 8            -> GPUEB *** ALIVE ***
	 *     20s-C  0x13F91030  = 0x1F00       -> GPUEB (dead)     ← 致命
	 *   ⇒ 0x13F91030 是 GPUEB 的停机/放核控制。LK 的加载器在拷固件前
	 *     正是写 0x1F00（= 按住），之后才放核。系统起来后再写 0x1F00
	 *     等于把协处理器停掉（WDT_CON/boot_ctl/IRQ_EN 全归零）。
	 *   0x13F91070 的 bit2/3（厂商 RPC 的 PWR_ON/PWR_ON_2ND）无害。
	 *   0x13FBF000/004/500 的写也不影响 GPUEB（mfgcfg 本身是死的）。
	 *
	 * ⛔ 因此这里**只读**，不再做任何写实验。
	 */
	/*
	 * ★ MFG_SEL_0 试验（2026-09-18）
	 *
	 * 推断：mfgcfg(0x13FBF000) 的寄存器写不进 ⇒ 它的 APB 时钟没开。
	 * 时钟树（照 MT6895 的 clk 驱动）：
	 *     mfg_ao_mfgpll -> mfgpll_ck ┐
	 *                                ├─ mfg_sel_0_sel (topckgen CLK_CFG_30 bit16)
	 *     mfg_ref_sel -> mfg_ref_ck ┘      -> mfg_sel_0_ck = GPU "core"
	 *     mfg_ref_ck -> mfgcfg BG3D gate = GPU "coregroup"
	 * 现在 bit16 = 0 ⇒ 选 mfg_ref_ck（来源不明，可能没跑）；
	 * 改成 1 就选 mfgpll_ck（LK 的 meter 实测 MFGPLL ≈ 265 MHz，肯定在跑）。
	 *
	 * 只碰 topckgen 和 mfgcfg，**绝不碰 0x13F91030 / 0x13C60600**（会停 GPUEB）。
	 */
	{
		void __iomem *tc, *cfg;
		u32 sel_before, sel_after, sta;

		tc = ioremap(0x10000000, 0x1000);
		cfg = ioremap(MT6897_MFGCFG_BASE, 0x1000);

		if (tc) {
			sel_before = readl(tc + 0x1f0);
			/* 只读：不再改 MFG_SEL（已试过 bit16=1 指向 mfgpll_ck，无效果） */
			sel_after = readl(tc + 0x1f0);
			pr_info("MT6897-SEL: 1F0 = %08X (MFG_SEL_0 bit16=%d)\n",
				sel_after, (sel_after >> 16) & 1);
			(void)sel_before;
		}
		if (cfg) {
			sta = readl(cfg + 0x0);
			writel(0x1, cfg + 0x4);		/* BG3D gate set */
			udelay(10);
			pr_info("MT6897-SEL: cfg sta %08X -> %08X (bit0=%d)  004=%08X 168=%08X\n",
				sta, readl(cfg + 0x0), readl(cfg + 0x0) & 1,
				readl(cfg + 0x4), readl(cfg + 0x168));
			iounmap(cfg);
		}
		if (tc) {
			pr_info("MT6897-SEL: 1F0 now %08X\n", readl(tc + 0x1f0));
			iounmap(tc);
		}
	}

	/*
	 * ★★ 厂商 __gpufreq_pdca_config(1) —— 已实测：写 mfgcfg 不落地，RPC 仍不应答。
	 * 序列记录（从 mtk_gpufreq_mt6897.ko 反汇编逐条解出，基址 .bss+0x138 =
	 * g_mfg_top_base = 0x13FBF000）：
	 *     0x98 |= 1   0xc0 |= 1   0x100 |= 1   0x120 |= 1   0x140 |= 1
	 *     0x400 |= 1   0x404 |= 0x80000000    0x418 |= 1   0x41c |= 0x80000000
	 *     0x430 |= 1   0x434 |= 0x80000000    0x448 |= 1   0x44c |= 0x80000000
	 *     0x460 |= 1   0x464 |= 0x80000000    0x478 |= 1   0x47c |= 0x80000000
	 * 这里不再重做，只读回看状态。
	 */
	{
		void __iomem *cfg, *rpc;
		u32 v;

		cfg = ioremap(MT6897_MFGCFG_BASE, 0x1000);
		rpc = ioremap(MT6897_MFG_RPC_BASE, 0x2000);

		if (cfg) {
			pr_info("MT6897-PDCA: cfg 098=%08X 0c0=%08X 100=%08X 120=%08X 140=%08X 168=%08X\n",
				readl(cfg + 0x98), readl(cfg + 0xc0),
				readl(cfg + 0x100), readl(cfg + 0x120),
				readl(cfg + 0x140), readl(cfg + 0x168));
			iounmap(cfg);
		}
		if (rpc) {
			v = readl(rpc + 0x1070);
			pr_info("MT6897-PDCA: rpc 1070=%08X bit2=%d bit3=%d bit30=%d bit31=%d\n",
				v, (v >> 2) & 1, (v >> 3) & 1,
				(v >> 30) & 1, (v >> 31) & 1);
			iounmap(rpc);
		}
	}

	/*
	 * ★★ mfg_ref_sel 实验
	 *
	 * MFG_TOP(mfgcfg) 的时钟链（照 MT6895 的 clk 驱动）：
	 *     mfg_ref_sel (topckgen CLK_CFG_4 = +0x50, lsb 24, width 2,
	 *                  upd = topckgen+0x4 bit19)
	 *         -> mfg_ref_ck (FACTOR 1/1)
	 *             -> mfgcfg BG3D gate  (GPU "coregroup")
	 *             -> 也是 mfg_sel_0_sel 的父钟之一
	 *     父钟：0=tck_26m_mx9_ck  1=univpll_d6  2=mainpll_d5_d2
	 *
	 * 本内核从没配过这个 mux。如果它选到没跑起来的源，mfgcfg 就没时钟
	 * ⇒ 寄存器读 0 / 写不落地。这里读出来，切到 1 和 2 各试一次，
	 * 每次都按 MTK 语义往 CLK_CFG_UPDATE(+0x4) 写 BIT(19) 触发更新。
	 *
	 * 只碰 topckgen；绝不碰 0x13F91030 / 0x13C60600。
	 */
	{
		void __iomem *tc, *cfg, *rpc;
		u32 cfg4, upd, sel, ref, v;
		int t;

		tc = ioremap(0x10000000, 0x1000);
		cfg = ioremap(MT6897_MFGCFG_BASE, 0x1000);
		rpc = ioremap(MT6897_MFG_RPC_BASE, 0x2000);

		if (tc) {
			cfg4 = readl(tc + 0x50);
			upd = readl(tc + 0x4);
			sel = readl(tc + 0x1f0);
			pr_info("MT6897-REF: CLK_CFG_4=%08X (mfg_ref_sel=%u) UPDATE=%08X CLK_CFG_30=%08X (mfg_sel_0=%u mfg_sel_1=%u)\n",
				cfg4, (cfg4 >> 24) & 3, upd, sel,
				(sel >> 16) & 1, (sel >> 17) & 1);
			/*
			 * 实测（probe16）：mfg_ref_sel 本来就是 1 = univpll_d6（在跑），
			 * 改成 2 = mainpll_d5_d2 也能写进（CLK_CFG_4 05050505 -> 06050505），
			 * 但 mfgcfg 依旧全 0、RPC 依旧不应答 ⇒ **时钟源不是原因**。
			 * 因此这里只读，不再改 topckgen。
			 */
			pr_info("MT6897-REF: (read-only; topckgen 可写、MFG 时钟源在跑，但不是卡点)\n");
			(void)upd;
			(void)ref;
			(void)t;
		}
		if (cfg)
			pr_info("MT6897-REF: cfg 000=%08X 004=%08X 098=%08X 100=%08X 168=%08X\n",
				readl(cfg + 0x0), readl(cfg + 0x4),
				readl(cfg + 0x98), readl(cfg + 0x100),
				readl(cfg + 0x168));
		if (rpc) {
			v = readl(rpc + 0x1070);
			pr_info("MT6897-REF: rpc 1070=%08X bit30=%d bit31=%d\n",
				v, (v >> 30) & 1, (v >> 31) & 1);
			iounmap(rpc);
		}
		if (cfg)
			iounmap(cfg);
		iounmap(tc);
	}

	{
		void __iomem *tc;

		tc = ioremap(0x10000000, 0x1000);
		if (tc) {
			pr_info("MT6897-TOPCK: 000=%08X 004=%08X 008=%08X 00C=%08X 050=%08X 054=%08X 058=%08X 1F0=%08X\n",
				readl(tc + 0x0), readl(tc + 0x4),
				readl(tc + 0x8), readl(tc + 0xc),
				readl(tc + 0x50), readl(tc + 0x54),
				readl(tc + 0x58), readl(tc + 0x1f0));
			iounmap(tc);
		}
	}

	/* GPUEB 必须仍然活着 */
	mt6897_gpueb_state("20s-after-ref");
}

static void mt6897_gpueb_late_probe2(struct work_struct *w)
{
	mt6897_gpueb_state("90s");
}

static DECLARE_DELAYED_WORK(mt6897_gpueb_late_work, mt6897_gpueb_late_probe);
static DECLARE_DELAYED_WORK(mt6897_gpueb_late_work2, mt6897_gpueb_late_probe2);

static int __init mt6897_gpueb_late_sched(void)
{
	schedule_delayed_work(&mt6897_gpueb_late_work, 20 * HZ);
	schedule_delayed_work(&mt6897_gpueb_late_work2, 90 * HZ);
	return 0;
}
late_initcall(mt6897_gpueb_late_sched);

/* dt-bindings/clock/mt6897-clk.h (private, single clock) */
#define CLK_MFGCFG_BG3D		0
#define CLK_MFGCFG_NR_CLK	1

static const struct mtk_gate_regs mfgcfg_cg_regs = {
	.set_ofs = 0x4,
	.clr_ofs = 0x8,
	.sta_ofs = 0x0,
};

#define GATE_MFGCFG(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &mfgcfg_cg_regs,		\
		.shift = _shift,			\
		.ops = &mtk_clk_gate_ops_setclr,	\
	}

static const struct mtk_gate mfgcfg_clks[] = {
	GATE_MFGCFG(CLK_MFGCFG_BG3D, "mfgcfg_bg3d", "mfg_ref_ck", 0),
};

static const struct mtk_clk_desc mfgcfg_mcd = {
	.clks = mfgcfg_clks,
	.num_clks = CLK_MFGCFG_NR_CLK,
};

static void mt6897_mfg_hwdcm_enable(void __iomem *top)
{
	void __iomem *rpc;
	u32 val;

	rpc = ioremap(MT6897_MFG_RPC_BASE, 0x10000);
	if (!rpc)
		return;

	/* downstream gpufreq __gpufreq_hw_dcm_control() */
	val = readl(top + 0x10);
	val |= BIT(0) | BIT(1) | BIT(2) | BIT(3) | BIT(4) | BIT(5);
	val &= ~BIT(6);
	val |= BIT(15);
	writel(val, top + 0x10);

	val = readl(top + 0x20);
	val |= BIT(23) | BIT(25);
	writel(val, top + 0x20);

	val = readl(top + 0xb0);
	val &= ~BIT(8);
	val &= ~BIT(10);
	val |= BIT(13) | BIT(14) | BIT(17) | BIT(18);
	val &= ~BIT(21);
	writel(val, top + 0xb0);

	val = readl(rpc + 0x1034);
	val &= ~BIT(0);
	writel(val, rpc + 0x1034);

	iounmap(rpc);
}

static void mt6897_mfg_acp_enable(void __iomem *top)
{
	u32 val;

	/* downstream gpufreq __gpufreq_acp_control() (MFG_TOP only) */
	val = readl(top + 0x168);
	val |= BIT(0) | BIT(1) | BIT(2) | BIT(3);
	writel(val, top + 0x168);

	val = readl(top + 0x8e0); val |= 0x855; writel(val, top + 0x8e0);
	val = readl(top + 0x8e8); val |= 0x855; writel(val, top + 0x8e8);
	val = readl(top + 0x910); val |= 0x855; writel(val, top + 0x910);
	val = readl(top + 0x918); val |= 0x855; writel(val, top + 0x918);
	val = readl(top + 0x900); val |= 0x055; writel(val, top + 0x900);
	val = readl(top + 0x908); val |= 0x055; writel(val, top + 0x908);
	val = readl(top + 0x920); val |= 0x055; writel(val, top + 0x920);
	val = readl(top + 0x928); val |= 0x055; writel(val, top + 0x928);
}

/*
 * Debug: read GPUEB GPR (LK loader descriptor @0x13c2fd1c) and MFG1 ctl
 * via ioremap (device mapping) to find out whether LK actually loaded the
 * firmware and whether the MFG domain is powered.
 */
static void mt6897_probe_state(void)
{
	void __iomem *gpr, *rpc, *st;
	u32 g0, g4, g18, ctl, stv;

	gpr = ioremap(0x13c2fd1c, 0x100);
	if (gpr) {
		g0 = readl(gpr + 0x00);
		g4 = readl(gpr + 0x04);
		g18 = readl(gpr + 0x18);
		iounmap(gpr);
		pr_info("MT6897-STATE: GPUEB GPR: size=%08X +4=%08X +18=%08X\n",
			g0, g4, g18);
	} else {
		pr_info("MT6897-STATE: GPR ioremap failed\n");
	}

	rpc = ioremap(0x13f91000, 0x2000);
	if (rpc) {
		ctl = readl(rpc + 0x70);
		pr_info("MT6897-STATE: MFG1 ctl 13f91070 = %08X\n", ctl);
		iounmap(rpc);
	}

	/* MFG_0_14_PWR_STATUS lives at base+0xFC0 (may be inside MFG domain) */
	st = ioremap(0x13f91fc0, 0x1000);
	if (st) {
		stv = readl(st + 0x00);
		pr_info("MT6897-STATE: 13f91fc0 = %08X\n", stv);
		iounmap(st);
	}
}

static void mt6897_mfg_gpm_enable(void __iomem *base)
{
	writel(0x20300316, base + 0xF60);
	writel(0x1800000C, base + 0xF64);
	writel(0x01010802, base + 0xF68);
	writel(0x000227F3, base + 0xFA8);
	udelay(1);
	writel(0x20300317, base + 0xF60);
}

/*
 * Debug: dump bootloader (LK/gpueb) strings from the log_store reserved
 * region.  /dev/mem cannot reach DRAM reserved regions under STRICT_DEVMEM,
 * but the kernel can ioremap them.
 *
 * Ranges are guesses (MTK log_store); scan a few and print any "[GPUEB]",
 * "boot-up", "NOT power-on" hits so we can see what LK did with GPUEB.
 */
static void mt6897_dump_lklog(void)
{
	static const u32 bases[] = { 0x7FFBF000, 0x7FFC0000, 0x7FFA0000 };
	int b;

	for (b = 0; b < ARRAY_SIZE(bases); b++) {
		void __iomem *m;
		int i;

		m = ioremap(bases[b], 0x40000);
		if (!m)
			continue;

		for (i = 0; i < 0x40000; i++) {
			char buf[256];
			int n = 0, j;

			/* find a printable run of >= 12 chars */
			while (i < 0x40000 && n < (int)sizeof(buf) - 1) {
				u8 c = readb(m + i);
				if (c >= 32 && c < 127) {
					buf[n++] = c;
					i++;
				} else {
					i++;
					break;
				}
			}
			if (n < 12)
				continue;
			buf[n] = 0;
			for (j = 0; j < n; j++) {
				if (buf[j] == '[' || (j + 5 < n &&
				    !strncmp(buf + j, "GPUEB", 5)) ||
				    (j + 6 < n && !strncmp(buf + j, "boot-up", 7)) ||
				    (j + 12 < n && !strncmp(buf + j, "NOT power-on", 12))) {
					pr_info("LKLOG 0x%08X: %s\n",
						bases[b] + i - n, buf);
					break;
				}
			}
		}
		iounmap(m);
	}
}

void mt6897_mfg_bringup_enable(void)
{
	void __iomem *base, *sleep;
	u32 val;

	base = ioremap(MT6897_MFGCFG_BASE, 0x1000);
	if (!base)
		return;

	/* AOC2.0: release VGPU SRAM isolation via the SPM HW semaphore. */
	sleep = ioremap(MT6897_SLEEP_BASE, 0x1000);
	if (sleep) {
		do {
			val = readl(sleep + 0x6ac);
			val |= BIT(0);
			writel(val, sleep + 0x6ac);
		} while ((readl(sleep + 0x6ac) & BIT(0)) != BIT(0));

		val = readl(sleep + 0xf30);
		val &= ~(BIT(9) | BIT(10));
		writel(val, sleep + 0xf30);

		val = readl(sleep + 0x6ac);
		val |= BIT(0);
		writel(val, sleep + 0x6ac);

		iounmap(sleep);
	}

	mt6897_mfg_hwdcm_enable(base);
	mt6897_mfg_acp_enable(base);
	mt6897_mfg_gpm_enable(base);

	writel(0, base + 0x500);
	writel(0, base + 0x80);

	pr_info_once("MT6897-MFG: bringup AOC/HWDCM/ACP/GPM applied (PDC0=%#x ACP0=%#x)\n",
		     readl(base + 0x400), readl(base + 0x168));
	iounmap(base);
}
EXPORT_SYMBOL_GPL(mt6897_mfg_bringup_enable);

static int clk_mt6897_mfgcfg_probe(struct platform_device *pdev)
{
	void __iomem *base;
	int r;

	base = of_iomap(pdev->dev.of_node, 0);
	if (!base)
		return -ENOMEM;
	iounmap(base);

	mt6897_gpueb_state("arch");

	/* Pre-power best-effort (MFG_TOP may not be powered yet; re-apply
	 * later via mt6897_mfg_bringup_enable()). */
	mt6897_mfg_bringup_enable();

	r = mtk_clk_simple_probe(pdev);
	if (r)
		dev_err(&pdev->dev, "could not register clock provider: %s: %d\n",
			pdev->name, r);

	return r;
}

static const struct of_device_id of_match_clk_mt6897_mfgcfg[] = {
	{ .compatible = "mediatek,mt6897-mfg", .data = &mfgcfg_mcd },
	{}
};

static struct platform_driver clk_mt6897_mfgcfg_drv = {
	.probe = clk_mt6897_mfgcfg_probe,
	.driver = {
		.name = "clk-mt6897-mfgcfg",
		.of_match_table = of_match_clk_mt6897_mfgcfg,
	},
};

static int __init clk_mt6897_mfgcfg_init(void)
{
	return platform_driver_register(&clk_mt6897_mfgcfg_drv);
}
arch_initcall(clk_mt6897_mfgcfg_init);
MODULE_LICENSE("GPL");
