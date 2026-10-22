# NootedRed（Radeon 780M 分支）

本项目是 [ChefKissInc/NootedRed](https://github.com/ChefKissInc/NootedRed) 的衍生分支，目的是为 AMD Radeon 780M（Phoenix / RDNA3，DCN 3.1.4 显示引擎）t提供 macOS 下的显示支持。

## 当前状态

**研究中的分支，尚不可用。** 设备识别已通过；驱动配置、上电、显示初始化、出画面与加速仍在推进。

## 安装

将 `Lilu.kext` 与 `NootedRed.kext` 放入 OpenCore 的 `EFI/OC/Kexts/`，并在 `config.plist` 的 `Kernel → Add` 中登记（Lilu 在前），然后重启。

本分支含大量 `-NRed*` 实验开关，默认关闭，普通使用者无需开启。


## 许可

上游以 Thou Shalt Not Profit License version 1.5 发布，本衍生作品沿用同一许可，详见 [LICENSE](LICENSE)，**仅限非商业用途分发**。本仓库未向上游提交，与 ChefKissInc 无从属或背书关系，亦与苹果、AMD、联想无关联。
