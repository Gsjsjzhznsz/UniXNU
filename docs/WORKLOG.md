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
