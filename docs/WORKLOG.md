# Worklog — UniXNU-raphael-touchfix

## Task 36 (t-merge) — 合并 t 系列触摸驱动能力（配置下发 + 冷启动重试）（2026-11-08）

Agent: Claude (Gsjsjzhznsz session)

Task: UniXNU 内核（jackyhowh 0112）在本机触摸不可用；t 系列同协议驱动实测可用。
差异定位 = 0112 缺少 784 字节面板配置下发（无配置 IC 固件不上报触摸帧）与
t23/0102 冷启动重试阶梯。用户指示"合并 t 系列部分驱动"。

Work Log:
- 重放两条补丁链取得终态基线（apple-oss xnu-8796.141.3 + t 系列 0001-0102 /
  ios-sweet + 0101-0112）；两树 sweet_touch.c 分别 1218/975 行。
- 合并：新增 GT9886 命令通道 defines（0x6f68/0x6f78/0x80/0x82/0x83/0xff）、
  gt9886_write_reg（多字节写）、gt9886_cmd、gt9886_doze_disable（稳健版，
  替换错误恢复路径调用）、gt9886_send_config（五步 784B 下发）、
  gt9886_cfg.h（sensor0/1 双配置，原样自 t 系列）；touch_raphael_start 重写为
  3 次冷启动重试阶梯（200/50/10/500、300/100/10/500 + att2 INT strap）、
  PID"9886"+72B sum==0 双判据、sensor→配置选择（flip=3/0，sweettouchflip
  可覆盖）、T 格式判决行；轮询/板表/SweetHID 未动。
- 验证：新 0112 对 pre-0112 基线干净应用/反向应用/应用结果与交付件逐字节
  一致；gcc -fsyntax-only（stub 头）零警告。
- 构建路径：本 fork `touch-cfg-merge` 分支 → build-raphael CI（full）。

Stage Summary:
- 补丁 = patches/0112-raphael-touch-gt9886-normandy.patch（989 行，19 hunks，
  3 文件）。等待 CI 产物实机验证触摸。

## Task 37 — macos-14 退役迁移 + 触控修复版首次启动（2026-11-08 下午）

Agent: Claude (Gsjsjzhznsz session)

Task: CI kernel job 因 GitHub 退役 macos-14 镜像被秒取消；改用 macos-15 重建并实机验证合并补丁。

Work Log:
- 对照实验（ubuntu 成功 / macos-14 秒取消，含最小 echo 探针）定位 = runner 镜像退役，非账号/工作流问题。
- kernel job 切 macos-15（镜像仅 Xcode 16/26，无 15；作业本就 sort -V 取最低版 = 16.0.0）。
- CI run 42448019354 全链成功：kernel（含"raphael touch patch landed"验证）+ firmware/payload/bootimg。
- 产物：boot-raphael.img sha256 ff215b6c7901d07075ff3815db4d231d2e4768920e37c319d23c544b61f2a0fa；
  kernel.macho sha256 1010c6bda3379c1215ab5d481c90a4a4136446607acd5f9893dda4a6be48c2dc。
- 13:34 fastboot boot：内核 USB 未如上轮在 ~90s 枚举，8 分钟后 USB 完全消失（疑似早期 panic 或触摸线程 panic）。
- 已准备诊断镜像 touchfix-diag.img（就地等长字节改 unixnu.toml：sweetfb_noconsole=0 屏幕控制台常开、
  sweetlog=1 持久日志写 userdata 尾部、sweettouchdelay=20→60），回读校验通过；待手机回到可操作状态启动。

Stage Summary:
- 修改版内核可在 macos-15 CI 完整构建；实机首启动结果待诊断镜像确认。

## Task 38 — 静默早期死亡定位 + SweetHID PE_current_console 修复（2026-11-08 傍晚）

Agent: Claude (Gsjsjzhznsz session)

Task: 诊断 13:34/15:41 两次新内核启动 USB 零枚举且无 fastboot 回落的原因。

Work Log:
- 15:41 启动诊断镜像（noconsole=0/log=1/delay=60）后现象与 13:34 相同；"ffu/devicerecovery" 画面实为
  内核挂死 + wdt=-1 无看门狗导致屏幕常亮至电池深度耗电后的厂商底层画面，非固件损坏。
- TWRP 拉取 userdata 尾部 4100KiB（sweet-tail-1545.bin，sha256 de7c82b9…）:全卷仅 1 个 SWEETLOG 头，
  内容为昨晚成功启动的完整日志（含干净 shutdown）→ 两次新内核启动均死在 SWEET LOG 初始化之前，
  早于屏幕控制台，故无任何可见输出。
- A/B 对照：artifacts/boot-fb060ad（昨晚成功内核）fastboot boot，~2 分钟 USB 枚举正常 → 硬件无恙。
- 变量二分：合并补丁 vs macos-15/Xcode16 工具链。推送 raphael-xcode16-bisect（=583bf5c 无合并补丁）
  触发 run 42461507109；同时审查合并补丁发现 SweetHIDTouch::start()（早于日志初始化执行）新增的
  PE_current_console() 调用为最高嫌疑（panic 即静默早死，与全部现象吻合）。
- 修复 7054c0e：删除该调用，硬编码 raphael 面板 1080x2340（与 SweetFramebuffer 探测输出一致），
  触发修复版 run 42461623732。两 run 结果将决定后续（若 bisect 也不启动则问题在工具链）。

Stage Summary:
- 死因假设收敛到 SweetHIDTouch::start 的 PE_current_console 早期调用；修复版与二分版并行构建中。

## Task 39 — UniXNU 官方构建链研究与路线收敛（2026-11-09 凌晨）

Agent: Claude (Gsjsjzhznsz session)

Task: 按用户指示走 UniXNU 文档路线（macos-26 / Xcode 26.6 官方 macOS build workflow），
研究 raphael 端可达到的实机镜像路径。

Work Log:
- 上游 `macos.yml`（main 分支）复制到默认分支 raphael-touchfix 后触发；首次失败 =
  `ModuleNotFoundError: unixnu`，原因是非默认分支缺少 scripts/unixnu 包 → 在 workflow 中
  固定 `ref: main` 检出 UniXNU（commit c5373a7）。
- 复跑 run 42499215112：Check inputs / Install tools / **Build kernel 均成功**（./unixnu build kernel，
  macos-26 + Xcode 26.6，device=Gsjsjzhznsz/UniXNU-raphael, defconfig=raphael_defconfig）。
  仅最后 Summary 步骤因上游脚本 `xcodebuild -version | head -1` 的 SIGPIPE 崩溃（exit 134），与构建无关。
- 官方 profile 只有三个：ipados-17.7.11-21H461 / ipados-26.5.2-23F84 / ipados-27.0.1-24A446
  （profiles/os/*/profile.toml），并且官方 devices.toml 中所有手机移植（Redmi K40、Mi 9、
  OnePlus 5 等）都使用 ipados-17.7.11-21H461 —— 即 iPadOS 只是内核/ADT/信任缓存输入来源，
  并非说法上的"iPad"。
- 设备仓库 UniXNU-raphael README：实机已进入 iPadOS 17.7.11 Hello UI 与锁屏（2026-11-07），
  触摸（GT9889）与图标主屏未验证；打包为 compile-only，release_recipe_complete=false，
  完整 firmware/payload/bootimg 配方与私有输入（平台种子、固件、镜像）不入库。
- xnu-8796 线：上游 ios-sweet 的 CI 注释同样确认 Xcode 16 的 iig 与 8796 IORPC ABI 不兼容，
  macos-14 退役后无法再产出可启动内核（本日二分已实机证伪 Xcode 16.0/16.4 构建）。
- 上游 UniXNU 提供完整 Linux 构建支持：scripts/linux/setup-toolchain.py（自建 clang、
  iig-port-darwin-target.patch、prepare-xnu.py 的源码改写、compiler-rt libcc_kext）——
  文档明确 kernel/package/firmware/payload/bootimg 均可在 Linux 主机执行。

Stage Summary:
- 官方 17.7.11 线的内核构建已在 macos-26 上验证通过；手机侧镜像需自行补齐 firmware/payload/bootimg
  配方（私有输入缺失），Linux 工具链是绕开 Xcode 的可行通道。下一步待用户确认根卷版本与镜像产出方式。

## Task 40 — 文档 Linux 构建链打通（2026-11-09 凌晨）

Agent: Claude (Gsjsjzhznsz session)

Task: 按 unixnu.com 的 Linux 要求文档（guide/requirements#linux）在内核步骤上取代 Xcode。

Work Log:
- 新增 `.github/workflows/linux-raphael-kernel.yml`（ubuntu-24.04）：检出 UniXNU main（含子模块）
  + 设备仓库，缓存 `/opt/unixnu-linux`，运行 `scripts/linux/setup-toolchain.py`，再
  `./unixnu defconfig/fetch/ipsw fetch/build kernel`。
- 三轮修复：① `kits/XnuPrelink` 等是 git submodule（recursive 检出）；② 缓存命中时 apt 依赖缺失
  （unifdef）+ `tee|tail` 吞退出码（改 pipefail）；③ 缓存不含 `/usr/bin` 的 xcrun/plutil/codesign
  shim（改为每次都跑幂等 setup，`--package-manager none`）。
- run 42510055544 全绿：62 个补丁应用、bootstrap 编译记录、正式编译、`emit_prelink.py` 预链接，
  产出 `out/raphael/ipados-17.7.11-21H461/raphael/kernel/kernel.macho`，
  sha256 d611048c598635af41a5d2da4c9caa9377d227d407d602e3150f115c689ddaa2，全程约 4 分钟。
- 已把 kernel.macho 加进 artifact（run 42510689931 起），供本地 payload/bootimg 打包使用。
- 设备仓库 README 记录：实体 raphael 已达 iPadOS 17.7.11 Hello UI 与锁屏；官方 device 仓库是
  compile-only（release_recipe_complete=false），完整 firmware/payload/bootimg 配方与平台种子为私有。

Stage Summary:
- 内核构建已完全脱离 macOS/Xcode（Linux 工具链 + 4 分钟周转）；剩下 payload/bootimg 配方与
  17.7.11 根卷两件事。待用户确认根卷来源（A 自行从 IPSW 提取 / B 用户提供）。

## Task 41 — 纯 Linux 产出可刷镜像（2026-11-09 早上）

Agent: Claude (Gsjsjzhznsz session)

Task: 用户指示"继续 Linux"：把内核、固件、payload、boot image 全链搬到 Linux。

Work Log:
- 新增 `.github/workflows/linux-full-image.yml`（workflow_dispatch）：kernel 作业 `./unixnu build kernel`
  产出 kernel.macho；image 作业复用本项目已验证的 Aloha UEFI 固件、FAT payload、boot image 打包脚本
  （scripts/build-firmware.py、build-payload.py、build-bootimg.py），无任何 macOS runner。
- run 42533531578 全绿：kernel + image 两作业均成功，产出 `boot-raphael.img`
  70,836,224 字节 sha256 537061cbfc751fd360a2fc27a5638750e867aefc1aef3b8bd6b53b024a74de11。
- 镜像内部校验：boot.json image/dtb/fd 摘要齐备；解出 64 MiB payload，其 kernel.macho 与本次
  kernel 作业产物 sha256 完全一致（80727c260de0c1bbc6452756f06b3de6b86ee33af247a8cf2234571cfa6bd31e），
  确认装入的是新内核（ipados-17.7.11 profile，即 iOS 17 代 Darwin 23.6）。
- 镜像已存 ios16-reference/linux-image/boot-raphael.img；内核存档
  ios16-reference/raphael-kernel/kernel-ipados-17.7.11.macho（前一轮）。

Stage Summary:
- Linux 全链产物就绪，可随时 fastboot boot 临时验证。已知不一致：手机 userdata 仍为 16.7.16 根卷，
  与 Darwin 23.6 内核的代际不匹配，实机首测重点看固件与内核早期启动，不期望用户态成功。
