# Mainline Linux for Lenovo Xiaoxin Pad Pro 12.7 (TB375FC / MT6897)

基于 Linux 7.2 主线内核的
联想小新 Pad Pro 12.7（TB375FC，MT6897 / Dimensity 8300）移植。

> ⚠️ WIP。仅供学习研究，刷写有风险。

## 分支

GitHub 上只有 **`main`** 一个分支，全部内核历史与全部自有改动都在上面。

| 改动 | commit |
|---|---|
| 工作树 checkpoint（首批底层改动） | `aef99312c` |
| MT6897 / TB375FC 设备树 | `4de7a0a87` |
| MT6897 mfgcfg 时钟 | `a85293bd3` |
| MT6897 scpsys 电源域 + GPU EB | `c5c79f641` |
| mt6685 / mt6375 充电修复 | `cbae621ce` |
| nt36xxx 触摸屏（BOE / TM 双面板） | `0f7d7c570` |
| connectivity MT6897 (soc7_0) 平台支持 | `15a563c84` |
| MT6897 WiFi / BT 独立开发线 | `375cfe902`、`0ab6b6cc3`（见下文合并说明） |
| 文档 | `4592bf5f1`、`89ceed2c9`、`a887fcc34` |

`aef99312c` 是动手前的状态存档，`df71e0fda` 是 WiFi/BT 那条线的合并点。

## 来源与谱系：为什么文件页面上还有别人的名字

本仓库导入的是**完整 git 历史**（`main` 可达 1,465,250 个 commit），不是"空仓库 + 我们几个补丁"。
所以在 GitHub 上点开任意文件，顶部提示的、以及 History / Blame 里出现的作者名，
是**上一次改动该文件的上游内核开发者**，Insights → Contributors 也会列出大量内核社区名字。
那属于代码的历史归属，不是本项目的维护者。任何基于 Linux 的内核树都是这样；
要让它不显示只能 `git filter-repo` 重写历史，那会同时丢掉与上游 diff / rebase 的能力和
GPL-2.0 要求的来源可追溯性，本项目不做。

谱系分三层：

| 层 | 来源 |
|---|---|
| Linux 主线 | `torvalds/linux`，v7.2.0（见 `Makefile`） |
| MT6895 平台分支 | `MT6895-Mainline/linux` 的 `7.2-mt6895-xiaomi-xaga`，起点 commit `33f2f15a3` |
| 本项目 | `33f2f15a3` 之后的 13 个可到达 commit（12 个非 merge + 1 个合并点 `df71e0fda`；first-parent 主线 11 个），全部作者为本项目 |

与 `MT6895-Mainline/linux` 的关系**只到 `33f2f15a3` 为止**：自那以后本树独立演进，
不跟随该分支（截至 2026-09-23 它已另行推进到 `b1138393d`），也没有从它合并过任何 commit。
本项目的设备树、电源域、mfgcfg 时钟、充电、触摸屏、connectivity 平台支持都是独立实现，
表中列出的这些 commit 即可全部核对归属（合并点 `df71e0fda` 见下文）。

配套的用户态与调试工具在另一个仓库 `tb375fc-linux-utils`，与本仓库历史完全分离，
不含对方的任何 commit。

## WiFi / BT 那条线为什么是"合并进历史但内容不变"

`375cfe902` + `0ab6b6cc3` 是一条独立的 WiFi/BT 开发线：它从同一个 base 出发，
但起步方式是把当时的工作态**拷贝**成一个 commit，因此在 git 图上与 `main` 没有祖先关系，
合并时 14 个文件（`mt6897.dtsi`、`nt36xxx.c`、`mt6897_pos_gen.c` 等）全部以 add/add 形式冲突，
两侧各自都有实质改动。

`df71e0fda` 用 `git merge -s ours` 把它并入历史：**`main` 的文件内容一字未改**
（`git diff 89ceed2c9 df71e0fda` 为空），但它成为祖先，改动随时可查：

```sh
git log --stat df71e0fda^2                    # 那条线本身做了什么
git diff 89ceed2c9 df71e0fda^2                # 相对 main 的净差异（待集成的部分）
```

后续把 WiFi/BT 真正落进代码时，以这个 diff 为准逐个文件裁决，不做机械取侧。

## 设备树

- `arch/arm64/boot/dts/mediatek/mt6897.dtsi` — SoC 级
- `arch/arm64/boot/dts/mediatek/mt6897-lenovo-tb375fc.dts` — 板级

## 构建

```sh
make ARCH=arm64 CROSS_COMPILE=aarch64-buildroot-linux-gnu- \
     O=build/tb375fc defconfig   # 或从设备 /proc/config.gz 还原
make ARCH=arm64 CROSS_COMPILE=aarch64-buildroot-linux-gnu- \
     O=build/tb375fc -j$(nproc) Image.lz4 dtbs modules
```

打包 boot.img 时需附带 initramfs（挂载 nvdata 以取得 WiFi 校准数据），
见配套仓库 `tb375fc-linux-utils/initramfs`。

## 硬件状态

| 部件 | 状态 | 说明 |
|---|---|---|
| UART 串口 | ✅ | 调试 console |
| eMMC / 存储 | ✅ | |
| 触摸屏 (Novatek nt36xxx) | 🔧 | BOE/TM 双供应商固件，见 `0f7d7c570` |
| WiFi / BT (conninfra, soc7_0) | 🔧 | 平台支持见 `15a563c84`；独立开发线见上文 |
| GPU (Mali, panthor) | 🔧 | 依赖 gpueb + mfgcfg，见 `c5c79f641`、`a85293bd3` |
| 充电 (mt6375 + mt6685) | 🔧 | 基本可用，见 `cbae621ce` |
| 显示 / 摄像头 / 音频 | ❌ | 未开始 |

## License

GPL-2.0，与上游内核一致。完整历史保留了每个文件的上游版权与作者信息。
