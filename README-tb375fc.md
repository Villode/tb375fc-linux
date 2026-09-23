# Mainline Linux for Lenovo Xiaoxin Pad Pro 12.7 (TB375FC / MT6897)

基于 Linux 7.2 主线内核的
联想小新 Pad Pro 12.7（TB375FC，MT6897 / Dimensity 8300）移植。

> ⚠️ WIP。仅供学习研究，刷写有风险。

## 分支

| 分支 | 内容 |
|---|---|
| `power-fix-tb375fc` | 电源域（scpsys/gpusys）、mfgcfg 时钟、mt6685/mt6375 充电、GPU EB、设备树 |
| `touch-fix-tb375fc` | Novatek nt36xxx 触摸屏（BOE / TM 两种面板）probe 与固件更新修复 |
| `wifi-bt-fix-tb375fc` | conninfra / soc7_0 WLAN / BT 平台上电竞态与时序修复 |

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
| 触摸屏 (Novatek nt36xxx) | 🔧 | BOE/TM 双供应商固件，见 touch 分支 |
| WiFi / BT (conninfra, soc7_0) | 🔧 | 上电时序修复中，见 wifi-bt 分支 |
| GPU (Mali, panthor) | 🔧 | 依赖 gpueb + mfgcfg，见 power 分支 |
| 充电 (mt6375 + mt6685) | 🔧 | 基本可用 |
| 显示 / 摄像头 / 音频 | ❌ | 未开始 |

## License

GPL-2.0，与上游内核一致。
