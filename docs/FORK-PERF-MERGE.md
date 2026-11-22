# 从 fork 线取性能改动：评估与移植计划

评估日：2026-11-22。来源：`HighDelay/nvidia-macos-driver-nvk`（v3.0.2-nvk，2026-11-17 发行，11-21 仍在提交）。
补丁原文已归档在本目录，文件名带 commit 短号。**本目录不参与构建**，是待移植清单，不是 `kexts/NVRM/patches/`。

## 为什么这条 fork 的数字对我们有效

| 项 | fork 作者测试机 | 本机 |
|---|---|---|
| GPU | RTX 2060 SUPER（TU104） | RTX 2070 Super Max-Q（TU104） |
| VRAM | 8 GB | 8 GB |
| BAR1 | 256 MiB | 256 MiB（`nvrm-bar1-mb` = 256） |
| 系统 | Tahoe 26.7.1 | Tahoe 26 |

同代同芯片、同 BAR1 孔径、同大版本系统。它的 small-BAR 与着色器缓存数字可以直接拿来当预期值，不需要打折。
本机内屏由 UHD 630 驱动（1920x1080@144），NVIDIA 只负责 HDMI 外接——这一点与 fork 作者的单卡机器不同，WindowServer 相关改动的收益要按"只影响外接屏上的合成"来估。

## 候选清单（按建议批次）

### 第一批：低风险、可验证、互不依赖

| 项 | commit | 收益 | 落地位置 | 移植状态 |
|---|---|---|---|---|
| **AIR 哈希复用** | `560a483` | Geekbench Metal **59616 → 73075（+22.6%）**。linker/lowerer 返回的 AIR 文本也标记为不可变，重复 dispatch 不再重新哈希整模块（数 MB 文本） | `plugin/NVMTLLibraryLoad.m` | 4 处替换点中 3 处可自动打，第 3557 行（`nvmtl_mesh_text`）需手工 |
| **WindowServer 图像池** | `87f5cd9` | 滚动/移窗期间复用 VRAM 块，少一次 RM 分配+释放；**顺带修掉池的并发竞态**（新块先挂链表再清零，别的 encoder 可能写进正在被清零的范围） | `plugin/nvmtl_vk.c` | `patch -F3` 可直接打 |
| **卷积诊断跳过** | `d78d09d` | MPS 回退路径每次 dispatch 都格式化 reason、建 NSString、加锁；release 构建下不再做 | `plugin/NVMTLObjects.m` | 可直接打 |
| **small-BAR 编码余量** | `2338e47` | 256 MiB 孔径下为 H.264 编码器预留 1/4（64 MiB，最低 16 MiB），避免桌面 surface 吃满导致录制报 RM 0x51 | `kexts/NVRMFB/fb/nvrm-fb.cpp` | 需手工：我们这版 `nvrmSurfaceBudget()` 只有固定 16 MiB margin，fork 是"先加 quarter 再收紧" |

**注意 `87f5cd9` 的自评**：作者原话 "desktop measurements are mixed, so no general CPU reduction is claimed"。它的价值一半在竞态修复，不要按"WindowServer 降 CPU"去验收。
**注意 `2338e47` 的前提**：只对 **≤256 MiB 已知孔径** 生效，≥4 GiB 走原来的半窗策略。我们正好命中。Turing 的 H.264 编码器在 1080p 要映射 33.5 MB 显存（含 15.9375 MB 重建缓冲），16 MiB margin 不够——这是 OBS 录制失败的根因。

### 第二批：改动面大，需要单独一轮

| 项 | commit | 收益 | 状态 |
|---|---|---|---|
| **Metal 元数据与绑定开销** | `d04dd86` | 复用不可变 AIR 分析、改善 surface/resource 缓存查找、稀疏 descriptor mask 遍历、只刷新变化的 texture-address 条目、去掉没用到的诊断格式化 | 586 行；`NVMTLDevice.m` 1/5 hunk、`NVMTLObjects.m` 1/8 hunk 打不上，其余可打。要逐个 hunk 过 |
| **OBS 采集修复** | `8de3fdb` | 桌面与光标卡顿 | `NVRM.cpp` 可打，`NVMTLDevice.m` 2/2 失败。只有用 OBS 才有意义 |

### 第三批：要额外组件，暂不纳入

- **Safari 硬件解码 + shader cache 复用**（11-21 提交，未发版）：需要 `NVWebKitFixup` 这个 helper kext 配合加载 codec addon，还要处理 WebKit GPU 沙箱拒绝文件同步的情况。Safari 卡顿确实是我们这台的老问题，但引入面太大，单独评估。
- **Mesa/NVK 26.3.0 backport**（11-20）：只影响走 Vulkan/Zink 的应用（Minecraft、Wine 游戏）。当前不跑这些，无收益。
- **`nvrmtgp=1` 笔记本 TGP**：用固件功耗预算放开 GPU 上限。Max-Q 机型上这是唯一可能改变"绝对性能"的项，但涉及散热与供电，风险等级和前面几个不同，不放进同一批。

### 明确不纳入

| 项 | 原因 |
|---|---|
| `0805a9d` Wine 混合呈现 | 不跑 Wine/CrossOver；补丁只动 `nvk/`，本地也没这个路径 |
| `620-dvi-rgb-scanout` | 数字 DVI 与旋转；我们用 HDMI |
| `NMIntelFB` / `NMGopFB` | 内屏已经由 UHD 630 正常驱动 |
| 活动监视器 GPU% 统计 | 默认关闭的诊断功能，不是性能项 |

## 必须先修的构建阻塞

这三条不解决，上面任何一项都构建不出来。当前 `build/` 下的脚本硬编码了不存在的 `$HOME` 路径。

1. `build/build_plugin.sh:4` → `N=$HOME/nvmtl-build; S=$N/LIVE/plugin`，而 `~/nvmtl-build` **不存在**。plugin 现在只能在 `01-NV工作区/nvidia-macos-driver/plugin` 里构建，脚本要改成指向工作区（或接受环境变量覆盖）。
2. `build/build_plugin.sh:11` → `-I"$HOME/Vulkan-Headers/include"`，**该目录不存在**。需要 `git clone https://github.com/KhronosGroup/Vulkan-Headers` 到位或改指 brew 路径。这是 plugin 编译的硬依赖。
3. `build/accel_build.sh:4` → `OGKM=${OGKM:-$HOME/ogkm610}`，需显式传 `OGKM=/Users/skyone/Desktop/NV驱动工作区-20261116/02-ogkm依赖树/rm620/ogkm-620.70.06`。该树的 `_out/Darwin_x86_64/compile_cmds.sh` 已在（11-15 生成），可以直接编。

## 落地顺序建议

1. 修构建脚本路径（纯脚本，无风险）
2. 打 `87f5cd9` + `d78d09d`（可直接打），`560a483` 手工补第 4 处，`2338e47` 手工改 margin —— 一次构建同时带上这四项 + 已评估的 `620-vblank-restore-unchanged-heads`
3. 装机后分两段验：
   - 性能：Geekbench Metal 跑三次取中位，对 59616 这个基线看是否靠近 73075；`ioreg` 看 `g_pool_blocks` 是否稳定不涨（池复用生效）
   - 显示：仍然跑 B 场景（无屏开机→插线），确认 vblank 补丁有没有让 `deferred until NVKMS latches a flip` 解除
4. 第二批（`d04dd86`）等这批结果稳定后再动，避免一次改太多分不清因果

## 与黑屏主线的边界

这批性能改动**不解决** HDMI 后插黑屏。fork 自己的 v3.0.2 release notes 仍列着 "Hotplugging displays does absolutely nothing when macOS is on unless restarted"。
唯一与显示相关的交集是 `560a483` 顺带保留的 WindowServer 分配优化，以及 `87f5cd9` 的池竞态——都不是黑屏的断点。
黑屏主线仍然是：phantom 占位 framebuffer 挑回 + 六个标志暴露 + vblank 补丁，三者一起进同一次构建。
