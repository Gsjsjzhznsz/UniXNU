# GPU 直扫交接文档（raphael / SM8150 / Adreno 640 / iOS 18.7.8）

> 目标：iOS 的合成结果由 DPU **直接扫描**，CPU 不再拷贝 10.9 MB/帧。
> 结论：**直扫已跑通（单 VIG0）**；DCS `0x51` 背光写入已实机确认。60 fps 稳态、动态黑块和唤醒仍未达标，
> 双 pipe 乒乓已写但默认关闭。未解决项见 §7。

---

## 0. 一句话现状（2026-11-14 更新）

**公开证据检查（2026-11-14）**

- UniXNU 的 [raphael 设备索引](https://unixnu.com/devices/models/raphael)列出多个移植仓库，但其中独立 K20 Pro 记录把 GPU 标为 `planned`，且明确写明公开源码/设备日志不足；[penYo22 记录](https://unixnu.com/devices/raphael)也说明 GitHub 当前无法读取，需维护者补充启动证据。没有找到可引用的公开 raphael GPU scanout 成功案例。
- UniXNU [显示适配指南](https://unixnu.com/zh/porting/display)指出 compositor 可能在 GPU 命令完成前就提交 swap；安全的 direct scanout 必须等对应 GPU 写 fence 后再 flip，并分别测 GPU wait、flip 和 panel latch。它支持 fence handoff 的通用要求，但不是本机型的黑块根因证明。
- UniXNU [GPU 适配指南](https://unixnu.com/zh/porting/gpu)建议把同一次触控对应的 command commit、GPU completion、显示 validation/fence wait/flip/latch 放在统一时间线上；[GPU 帧诊断指南](https://unixnu.com/zh/porting/gpu-diagnostics)列出了触控边界、render/encode/complete 线程采样、GPU pass、fence 和 display latch 的关联证据。
- 本仓实测已否定“只把 SwapEnd 延后一帧就足以修复”的强结论：`0281` fresh-fence wait 的 `scanout_us_max` 仅约 624 µs，说明当时 fence snapshot 立即通过；late-present `0283` 的 15 秒窗口 `swaps_s` 均值 38.23（6.98–54.17），未证明 60 fps，用户可见效果/延迟尚无结论。保留 `0283` 为 opt-in，不宣称修复。
- [本轮 late-present A/B 记录](../validation-20261113/448fd78/retest/RESULTS.md)中 20 帧截图证实画面在变化，但低分辨率静态像素检测不能替代对移动黑块的视觉判定。设备当前使用无 late-present 的对照镜像。
- Dynamic Island：此前通过 SSH 直接读到 MobileGestalt `CacheExtra[DeviceSupportsDynamicIsland]=True`；本次整理末尾复核时 `dynamic-island.sh show` 报 cache missing，因此该值只作为上轮历史读数，不宣称当前仍有效。即便为 `True` 也只证明能力 override，不证明 SpringBoard 已画出岛或 DPU 多图层。`usblink.sh` 默认 SSH key 回退已修复；cache 缺失时需等系统重新生成后再读，不要盲写。

CoreAnimation 交出的 IOSurface 通过显示流自己的 stage-1 页表被 **VIG0** 直接读取，QSEED3 做
`1125x2436 → 1080x2340`，每帧 CPU 只做 20 来次 MMIO 写。

- **已确认可用**：`mode=direct`、`copy_frames` 不随运行增长（累计 3–4，全在启动初期）、
  GPU `submitted == completed`、`hangs=0`。滑动窗口实测 `swaps_s` 到过 **41.5–52.4**，
  静态锁屏时是 0–10（用户态本来就不出帧）。
- **P0 未达标**：60 fps 稳态和动态黑块仍未解决；近期窗口没有证明持续 60 fps。DCS `0x51` 硬件写入及可见亮度变化已确认；滑块接线/唤醒恢复属于 P1 待办。
- **双 VIG 关闭**：`sweet_scanout.c` 中 `sc.pp` 强制为 `false`；保留原因与重新评估条件见 §7 双 SSPP 状态。

**上机实测（内核 `3ca6b55`，2026-11-13，滑动中）—— 0275 + 0277 生效**

```
mode=direct hz=59 swaps_s 峰值 60.49（活跃期常见 55–60）
latch_waits=1  latch_timeouts=0        <- 之前是 42–49 与多次 25 ms 超时
coalesced_s 0–5（之前 9.89）
copy_frames=5（全部在启动初期）
scanout_frames=3285 scanout_latched=3034 scanout_declines=148
scanout_us=719 µs mean, scanout_us_max=848
```

逐秒采样（同一进程，含息屏空档）：

```
sec  swaps  coal  frames  latch  decline  latchw
  2     24     0      25     22        4       0
  9     59     5      53     53        0       0
 13     76     0      76     76        0       0
 15    102     4      99     91        0       0
```

结论（仅限该历史窗口）：`latchw` 全 0 说明 work-loop 上的 25 ms 忙等已消失；部分活跃秒的 `swaps` 达到 76–102，`latch` 紧随其后。该样本证明单管有瞬时超过 60 的能力，不证明稳态 60 fps，也未解决观感卡顿/黑块。它不能单独证明 CA 一定是当前限制，也不能据此断言双 pipe 对目标无用。

**历史诊断结论（勿再当作黑块根因）**

- 某次 GPU 日志出现 CP ring timeout（`WPTR > RPTR`、`HW_FAULT=0`）；说明该区间 GPU queue 未排空，但后续 `0279` 只在少数 swap 超时、`0281` fresh-fence wait 立即通过，且 `sweetfb_scanout=0` 仍有黑块。该 timeout 不能解释持续黑块或卡顿，遇到时作为独立 GPU 健康问题处理。
- GPU OPP 参数：`sweetgpufreq=5` 实际落到 level 1/257 MHz，不是锁 5；`sweetgpufreq=g sweetgpuboost=5` 可达 level 5/585 MHz。该参数解析事实有效，但历史主观黑块变化没有严格同场景对照，不能据此归因频率。
- SMMU CB1 FSR `0x400` 与 49 位 FAR 是启动残留；submit 路径清 stale status，不能作为运行时 fault 根因。
- `0278` IB 地址检查为只报不改，历史日志没有证实越界 IB 是黑块原因。

### 关键否证：黑块与「卡」都和直扫无关（2026-11-13）

用户实测两点，直接改变了方向：

1. **`sweetfb_scanout=0`（纯 CPU 拷贝）下黑块依旧** → 伪影不在直扫路径。
2. **开不开 GPU 直扫，手感一样** → 用户感到的「卡」也不在显示路径。

每帧时间戳（`kern.sweet_fb_trace`，直扫模式，滑动中）：

```
id   swap_us  scan_us  latch_us  done_us  wcall_us  wret_us
591    18330    20881     30869    30869     29372    29373
592    34753    37184     47660    47660     45993    45994
```

- 帧间隔 16.4 ms → **确实 60 fps**
- swap→提交 2.5 ms、提交→latch 10.0 ms、swap→上屏 **12.5 ms**
- SwapWait 调用到返回 1 µs（0271 已生效）

即：显示侧交付是健康的。`0275/0277/0279/0280/0281` 对用户体感问题**不是关键**，
不要再沿这条线加补丁。剩余问题（黑块 + 卡）在上游：CoreAnimation 交出来的内容本身，
或 iOS 侧合成/渲染能力。

### 实测排除的候选解释（2026-11-13，内核 `4e0a054`）

用户描述最终确定为「**可移动的黑块**」（随内容一起移动，属帧内容的一部分）。

| 假设 | 补丁 | 实测结果 |
|---|---|---|
| 交付时 GPU 还没画完 | `0281` 提交前重新取 fence 并等待 | **否定**：`scanout_us_max = 624 µs`，fence 每次都立即通过；若真等过应看到 ~16000 µs |
| 面板读到被改映射的 IOVA 槽位 | `0280` 钉住最近两次 IOVA | **否定**：黑块依旧，`scanout_declines` 29→78（钉住使可用槽位变少，多余帧走拷贝） |
| GPU 超时后放行未完成帧 | `0279` 超时改为丢帧 | **比例不足**：420 swap 中仅 4 次，解释不了持续可见的黑块 |

**该二分实验后来已做**：`sweetfb_scanout=0`（CPU copy）下黑块仍在，用户也反馈直扫开关不改变卡顿手感。因此伪影不能归因于 raphael stage-1 IOVA/单 VIG0 直扫实现；接下来应检查 CA/Metal 输出内容与 panel/DSI 更新时序，不能再把“开关 scanout”列作待做实验。

### 用户实拍：不是随机黑块，是会往下走的水平接缝

2026-11-13 用户拍屏确认：在桌面滑动时看到的是**渐进式往下移动的水平接缝**，上/下两边内容
不属于同一帧。这是「面板被部分更新」，不是随机噪声。

`IF_TEAR_LINE (0x2b0)` 在 `sweet_scanout.c` 里只有两处、都是**只读观测**（`f0`/`f1` 是
初始化前后各读一次，用来确认我们没动它）。日志里的 `tear line 1104 -> 1432` 是面板自己在推进，
不是我们改的。

**带宽估算（关键，尚未验证）**：一帧 `1080x2340x4 = 10.11 MB`。

| 链路 | 未压缩上限 |
|---|---|
| 4 lane @1.0 Gbps | 39.6 Hz |
| 4 lane @1.2 Gbps | 47.5 Hz |
| 4 lane @1.5 Gbps | **59.4 Hz** |
| 4 lane @2.0 Gbps | 79.1 Hz |

60 Hz 需要 607 MB/s。若链路是 1.5 Gbps/lane，理论上限 59.4 Hz——**零余量**；若是 1.2 Gbps/lane
只有 47.5 Hz。也就是「每刷新一帧都送一整帧未压缩画面」在物理上就送不完，面板只能分几次更新，
表现正是那条会移动的接缝。这能解释「项目一开始就有」。

> **未验证，且上一次尝试无效**：2026-11-13 用 `sweetfb_hz=30` 想降提交率，结果 `hz` 仍是 59——
> `sweetfb_hz` 只节流**拷贝路径**，直扫路径不受它控制，所以那次实验什么都没证明。
> 要真正验证，必须让直扫路径本身隔帧提交（代码改动），或者读回 DSI 的实际 lane 速率/DSC 状态。

若确认是带宽，可行的方向（按代价排序）：启用 DSC 压缩（ABL 若已开则无需做）、降低像素格式
（如 RGB565 减半）、或接受 ≤面板实际上限的刷新率。**不能靠改提交率掩盖**。

### 历史无效截图与 IOVA 假设（已否证，2026-11-13）

最初 `kern.sweet_fb_shot` 按 `fShotId` 抓最新提交 surface，并据一张静态截图的 `dark=0` 推断 panel 所见帧干净；这不是可靠的 latch 对齐证据。之后 `0282` 改为捕获 `fScanoutPin[0]` 指向的 latched surface，修复借用引用释放问题；同时确认静态画面无法判断动态伪影。

据此产生的“DPU 仍读已重映射 IOVA”假设由 `0280` pin 最近两个 IOVA 实机测试，黑块仍在且 CPU-copy decline 增多，未获支持。它不再是优先根因。当前更可靠的事实是：CPU-copy 路径也有黑块、用户报告直扫开关不改变手感；普通逐帧静态截图也不能代替同步触控、GPU completion、flush/latch 的时间线。

**可疑点：IB 地址没有范围校验。** `sweet_gpu_submit()` 把客户端给的 IOVA 直接写成环形缓冲里的
两个 dword：

```c
ring_emit((uint32_t)ib_iova[i]);
ring_emit((uint32_t)(ib_iova[i] >> 32));
```

raphael 的 GPU VA 窗口是 `[GVM_BASE, GVM_END) = [0x80000000, 0x100000000)`，但这里什么值都收。
一个 64 位越界地址会让 CP 去取一块根本不存在的内存，表现正是「卡住 + 无 fault 位」。

`0278` 先加了**只报不改**的校验：越界或非页对齐的 IB 地址按不同值各打一次，带 seqno，
提交行为完全不变。下一次上机若打出这条，就等于把坏地址锁到了具体调用点。

历史上曾建议优先查 GPU ring/fence，但当前证据没有把它确认为黑块根因。若重新调查 GPU queue，应作为独立性能问题验证，不能与黑块因果混写。原建议为：
`sweet_a630.c` 的提交/完成推进、`sweet_gpu_fence_snapshot()` 的粒度（全局 vs 每帧）、
以及 `0229` 门控是否在等一个不相干的 GPU 工作。

对比基线（内核 `f54a0a3`，2026-11-12，滑动中）：

```
mode=direct hz=59 up_s=2333 win_ms=1010
swaps=4030 swaps_s=41.54 coalesced=90 coalesced_s=9.89
copy_frames=3 latch_waits=42 vsync_s=58.36 swapsync=0
gpu submitted=22945 completed=22945 hangs=0
```

历史构建（内核 `3ca6b55`，2026-11-13）：CI `43416181039` success（第一次跑被 macOS runner 抢占失败，`gh run rerun` 后通过），`errors.txt` 空；该内核已用于单 VIG0/0275/0277 实机验证，不是当前最新镜像：

| 镜像 | 用途 | sha256 |
|---|---|---|
| `validation-20261113/3ca6b55/boot-3ca6b55.img` | 单管 + `0275` 退休修复 + `0277` 直扫计数，已上机验证 | `43da833a…390093d2` |
| `validation-20261113/3ca6b55/boot-3ca6b55-dcs.img` | 同上，外加 `raphael_dcs=1`，用于 DCS 亮度验证 | `d2d0b834…7ebee466` |

DCS 版为塞进 298 字符 args 上限去掉了 `sweetfb_statlog=1`（只影响周期 IOLog，
`kern.sweet_fb` sysctl 不受影响）。

上机后第一步就跑 `tools/raphael/measure-fps.sh 20`：它每秒打印
`swaps / coalesced / scanout_frames / scanout_latched / scanout_declines` 的增量，
直接区分「CA 不产帧」「单管跟不上」「回落 CPU 拷贝」三种情况。

---

## 1. 硬件事实（全部实测，可直接引用）

| 项 | 值 |
|---|---|
| 面板内存 | PA `0x9c000000`，stride `4320`，`1080x2340`，`SRC_FORMAT=0x000237ff`（ARGB8888） |
| MDP base | `0x0ae00000`；VIG0 `+0x5000`；QSEED3 `VIG0+0xa00`，**version `0x1004` (v1.4)** |
| CTL0 | `+0x2000`，`CTL_FLUSH=0x18`，VIG0=bit0，CTL=bit17 |
| INTF1（DSI0） | `+0x6b800`，tear check `+0x284 = 1`，autorefresh `+0x2b4 = 0x80000001`（bit31 置位） |
| apps SMMU | PA `0x15000000`；SID `0x800` → SMR 3 = `0x84200800`，S2CR 3 = `0x2` → **translate，bank 2** |
| bank 2 原始状态 | `SCTLR=0x000000e0`（M 位为 0，即翻译关闭）、`CBAR=0x0001f000`、`TCR2=0x4001c077` |
| 页表落位 | splash carve-out 尾部 `0x9e380000`（`0x9c000000 + 0x2400000 - 0x80000`），20 页 |
| 输出地址位宽 | `IDR2=0x00005555`，OAS=32 bit（表必须在 4 GiB 以下） |
| GPU | `SweetAdreno630`，GMU `0xbabeface`，HFI v2，`submitted==completed`、`hangs=0` |

**关键结论**：raphael 的显示流是 **TRANSLATE**，不是 BYPASS —— 所以老的 `sweetfb_zc`
（要求 bypass）在 raphael 上永远不可能工作，必须走 stage-1 页表这条路线。

---

## 2. 代码位置

### 新增文件（由 stage 脚本装进源码树）

| 文件 | 作用 |
|---|---|
| `xnu/patches-11215/sweet_scanout.c` | 显示流 SMMU bank、页表池、IOVA 槽、QSEED3 编程、flush/latch、panic 回退 |
| `xnu/patches-11215/sweet_scanout.h` | 对外 API + `sweet_swap_done()`（swap_wait_gated 判定） |
| `xnu/patches-11215/0268-stage-sweet-scanout.sh` | 把它们 install 到 `bsd/dev/` 与 `iokit/Kernel/arm/` |

### 补丁

| 补丁 | 内容 |
|---|---|
| `0268-sweet-fb-gpu-scanout.patch` | `bsd/conf/files` 加 `sweet_scanout.c`；`SweetFramebuffer.cpp` 加 `fScanout*` 字段、
`scanoutIova()`、`scanoutPresent()`，并在 `zcPresent()` 里插入扫描分支、在 `start()` 里按
`sweetfb_scanout=1` 打开 `fEarly/fSwapSync` |
| `0269-sweet-fb-swapwait-nolatch.patch` | `swapWaitInner()`：`fLastFlush==0`（没有任何帧交给 MDP）直接算完成，避免冻屏 |
| `0270-sweet-fb-scanout-perf-and-off.patch` | ① GPU 帧不再做 10.9 MB 缓存清理；② 熄屏时先把管道切回面板内存，切成功才释放 surface hold |
| `0271-sweet-fb-swapwait-at-submit.patch` | ① scanout 模式下 SwapWait 提交即答复（省 9.9 ms/帧）；② `sc_flush_idle` 最多等 2 ms，pending flush 由新写入覆盖（省 16.2 ms/帧） |
| `0275-scanout-retire-before-submit.patch` | **关键性能修复（已在当前补丁系列保留）**：提交前退休已 latch 帧 + 容量预检（去掉 25 ms work-loop 忙等）；映射/清 cache 前 hold surface，失败撤销 |
| `0272`（在 `sweet_scanout.c` 内，**默认关闭**） | VIG0/VIG1 乒乓骨架，`sc.pp = false`；当前无证据证明它能解决用户伪影，见 §7 双 SSPP 状态 |
| `0137-beryllium-reboot-mode.patch`（改动） | 把 reboot-to-bootloader 开放到 raphael（PM8150 PON spare + IMEM + PSCI fallback），用于自愈循环 |

> **已删除**：`0273-raphael-pm8150l-wled-read.patch`。它给 raphael 开 SID 5 的 WLED 读白名单，
> 但亮度实际走 DSI DCS（§7 P1），写了也点不亮，且会把后来人引到错误方向。

### 工具 / 资料

| 路径 | 说明 |
|---|---|
| `tools/raphael/raphael-base.adt` | **已知可启动**的 12964 B ADT（从 `boot-ios18-mdp-zcscale.img` 提取）。打包必须用它 |
| `tools/raphael/capfast.py` | USB CDC-ECM 上 UDP 5140 的内核控制台捕获（自动配 192.168.7.1/24） |
| `tools/raphael/pack-mdp-tests.sh` | 打包测试镜像（含 `boot-ios18-scanout.img` 目标） |
| `docs/FORK-SCAN-AND-BUGS.md` | 13 个机型 fork 的显示/GPU 横向扫描 + 本仓显示路径代码审查 |
| `ios16-reference/ios18-image/boot-ios18-scanout.img` | 当前可用镜像 |

### CI

`.github/workflows/xnu.yml`：push 到 `main`（`xnu/patches-11215/**` 或 workflow 自身变动）自动构建，
artifact 名 `xnu-kernel`（`kernel.development.sweet`）。**已修**：暂存步骤原来只拷 `*.patch`/`*.sh`，
现在额外把 `sweet_scanout.c/.h` 按原名拷进 `dxb/patches/15.0/`。

---

## 3. 启动参数（唯一可用的组合）

```
debug=0xc serial=3 cpus=8 rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1
launchd_unsecure_cache=1 wdt=-1 sweetfb_hz=60 sweetusb=2 sweetarm64e=1 sweetlog=0
sweetgpu=8 sweetgpufreq=g ufsrw=1 rootrw=1 sweetfb_noconsole=1 sweetfb_statlog=1
sweetfb_mdp=1 sweetfb_scale=2 sweetfb_scanout=1
```

**不要**加 `sweetfb_zc` / `sweetfb_hw` / `sweetfb_flip`：
`zc` 会去用 beryllium 硬编码的 low pool；`hw` 会重写 pipe 寄存器让 `scanout_init` 认不出来。

保持 args ≤ 298 字符（已知可启动镜像 manifest 480 B；UEFI 按 blob 长度解析，超长会截断）。

---

## 4. 构建 → 启动 → 取数（可脚本化）

```sh
# 1) 构建（push 自动触发，或单次 dispatch）
gh run list -R Gsjsjzhznsz/UniXNU-raphael-iOS18-K20Pro --workflow xnu.yml -L 1
gh run download <RUN> -R ... -n xnu-kernel -D /tmp/qk

# 2) 重打包（必须用入库的 ADT）
export PATH=<mtools>/bin:$PATH
CMDLINE="...上面的参数..." sh tools/raphael/mkboot-raphael.sh \
  ios16-reference/ios18-image/boot-ios18-mdp-zcscale.img \
  /tmp/qk/kernel.development.sweet tools/raphael/raphael-base.adt \
  ios16-reference/ios18-image/boot-ios18-scanout.img

# 3) 抓控制台 + 临时引导
python3 -u tools/raphael/capfast.py /tmp/boot.log &   # 需要 /tmp/.sudo_pw
fastboot boot ios16-reference/ios18-image/boot-ios18-scanout.img

# 4) 取运行状态（USB 网 192.168.7.1/24，手机 192.168.7.2）
ssh -i keys/raphael_ios_ed25519 root@192.168.7.2 \
  'sysctl -n kern.sweet_fb; sysctl -n kern.sweet_gpu'

# 5) 自愈回 fastboot（不需人工按键）
ssh ... 'sysctl -w kern.sweet_reboot_mode=bootloader; /var/jb/usr/sbin/reboot'
```

判据：日志出现 `SWEET SCANOUT: bank 2: tables ... the display reads through its own tables`
→ `scanout on:` → `first GPU scanout: ... no copy`；`kern.sweet_fb` 的 `swaps_s` 上升而
`copy_frames` 不动。

---

## 5. 这套代码怎么工作（读代码前先看这段）

1. **第一次 Metal 帧**：`zcPresent()` → `scanoutPresent()` → `sweet_scanout_init()`
   - 认管道：`VIG0.SRC0_ADDR == 面板 PA` 且 stride 一致（其余寄存器 `show()` 会自己写）。
   - 认流：唯一匹配 SID `0x800` 的 SMR，S2CR translate → bank 2，且 `SCTLR.M==0`。
   - 建 4K stage-1 表（32-bit 输入空间，TCR `T0SZ=32`，TCR2 SEP `7<<15`），把 **整个 36 MiB
     splash carve-out 按其物理地址映射**（这样 copy 回退和 console 仍用物理地址）。
   - 试 flush：100 ms 内必须被吸收且不 fault，否则全部回滚（fail-closed，保持 CPU 拷贝）。
2. **每帧**：把 surface 的物理页映射进 8 个 16 MiB IOVA 槽之一（按 surface 缓存，LRU 回收
   仅限未被 hold 的），然后 `show()` 写 SRC0_ADDR/STRIDE/SRC_SIZE/XY/OUT_SIZE/XY + QSEED3
   相位步进，最后 `CTL_FLUSH = VIG0|CTL`。
3. **回收**：surface 在 `fZcHeld[]` 里 hold 到「后面的某次 flush 被取走」为止（`zcPoll`）。
4. **熄屏/panic**：把 16 个 SSPP + 14 个 QSEED 寄存器恢复成接管前的值，管道回到面板内存。

---

## 6. 已经踩过的坑（**不要重复**）

1. **ADT 必须是镜像里那份**（12964 B）。用 `make-raphael-adt.py` 现场生成的 12604 B 会在
   `SWEET EARLY 17: fbcon config read; mapping` 处 `pmap_map_bd @pmap.c:1962` panic。
2. **新增文件类补丁改行数必须同步 hunk 头**（`@@ -0,0 +1,N @@`）。不同步 → 整文件被
   `git apply` 跳过 → `unterminated conditional directive`。改完用 §8 的校验脚本跑一遍。
3. **stage 脚本在补丁之前执行**，只能新增文件，不能修改 `SweetFramebuffer.cpp`。
4. **dxb 只拷 `*.patch`/`*.sh`** 进 `patches/15.0`，支撑文件（`.c`/`.h`）要在 workflow 里单独拷。
5. **`ml_io_map` 是 device memory**：不能 `dc cvac`（会 unaligned data abort），也不要假设
   16 字节 store 可用；写表之后一句 `dsb sy` 就够（无 cache 可清）。
6. **SMMU 页表必须低于 4 GiB**（xnu 管的 RAM 全在 4 GiB 以上），所以放 carve-out 尾部。
7. **打开 `fEarly/fSwapSync` 就必须保证有人给 MDP 编程**，否则 `swapWaitInner` 永远 Busy
   （实例：`swap_wait_timeouts=2810`、33 分钟 35 帧、界面冻死）→ 0269 的 `fLastFlush==0` 保护。
8. **别每帧清 10.9 MB cache**（`sweet_dma_sync_va`）：GPU 写的东西绕过 CPU cache，清了等于
   把省下的 memcpy 又加回来（第一版就是这么卡死的）。
9. **`fastboot boot` 是临时引导**，不写 boot/dtbo/userdata；宿主重启会清 `/tmp`（脚本要入库）。
10. `sweetfb_zc`/`sweetfb_hw`/`sweetfb_flip` 与 scanout 模式互斥（见 §3）。

---

## 7. 未解决 / 下一步（按优先级）

### P1 背光与唤醒
现象：熄屏要按好几次电源；`SWEET FB: backlight fade: cannot read WLED string 0 brightness; pixel fallback`。

**结论已改（2026-11-13）：raphael 的 AMOLED 亮度不走 WLED，走 DSI DCS。** 依据（Xiaomi
`raphael-p-oss`，固定 SHA `663db50e`）：

| 证据 | 内容 |
|---|---|
| `xiaomi-sm8150-common.dtsi:329-337` | 面板 `bl_ctrl_dcs`，`min 2 / max 2047` |
| `dsi-panel-ss-fhd-ea8076-cmd.dtsi` | `qcom,mdss-dsi-bl-dcs-type-ss`（Samsung 高字节在前），cmd mode，1080x2340 |
| `dsi_panel.c:746-755` | `dsi_panel_set_backlight()` 按面板 `bl_config.type` 选 WLED **或** DCS；DCS 面板永不进 WLED 分支 |
| mainline `panel-samsung-ams639rq08.c` | DCS `0x51` 写亮度（2 字节，高字节先），`0x52` 回读，max 2047，init 1023 |

PM8150L 的 `wled@d800`（usid 5）**确实存在且被 raphael 板级 DTS 打开**（`raphael-sm8150.dtsi:743-747`，
`status="ok"`、`string-cfg=15`），但节点存在/使能与「这块 AMOLED 的亮度传输」不是一回事——
不能用它当亮度通路。

**已回退的错误改动**：`0273`（给 raphael 开 SID 5 WLED 读）已删除，`0126` 的
`sp_write_allowed` / `SWEET_WLED_SID` 也回到 beryllium-only。原因是加 SID 5 只能让
`blInit()` 读到寄存器，写路径仍被 beryllium 条件挡住，`fBlOK` 还是 false，对亮度零作用。

**已完成硬件通路；后续工作**：不要再实现 WLED 或另写 DCS raw MMIO。DCS `0x51` 已由下方补丁安全发送并在实机得到 `last_error=0`。剩余验证是把 `kern.raphael_dcs_level` 接到系统亮度 UI、肉眼确认亮度有变化，并单独验证熄屏/唤醒恢复。若需要扩大协议（例如 `0x52` 回读或 HS/LPM 状态机），先查明现有写入对面板的可见效果再动。

**已实现（`0f90a60` + `691a02f`）：`bsd/dev/raphael_dsi.c`，默认关闭**

启动参数 `raphael_dcs=1` 才武装；没有它不映射、不读、不写任何寄存器，镜像行为与不加该模块完全一致。

- 发送：一条 DCS 长写 `0x39`，word count 3，payload `51 hi lo`，补 `0xFF` 到 8 字节。
  两条 32 位字：`0xC0390003`、`0xFF000051 | (hi<<8) | (lo<<16)`。
- 前置：轮询 `DSI_STATUS(0x008)` bit2（command-mode MDP busy）清零，上限 200 ms /
  2 ms 步进，对应下游 `dsi_ctrl_hw_cmn_wait_for_cmd_mode_mdp_idle`。
- 发包：`TEST_PATTERN_GEN_CTRL(0x15C) = BIT(1)|BIT(2)|(3<<16)` → 两个 dword 推进
  `TEST_PATTERN_GEN_CMD_DMA_INIT_VAL(0x17C)` → `COMMAND_MODE_DMA_CTRL(0x03C)` 置
  `BIT(28) EMBEDDED_MODE`（先清 BIT31/30/26）→ `DMA_CMD_LENGTH(0x04C) = 8` → `dsb sy` →
  `CMD_MODE_DMA_SW_TRIGGER(0x090) = 1`。
- 完成：轮询 `INT_CTRL(0x110)` bit0 `CMD_MODE_DMA_DONE`，上限 50 ms；bit24 出错则放弃。
- 收尾：把 `COMMAND_MODE_DMA_CTRL` 还原成读到的原值（帧通路的 DMA 配置不留在被改状态），
  并 W1C 清掉自己消费的完成位。不 reset、不改 lane/PHY/clock/timing。
- 接口：`sysctl kern.raphael_dsi`（只读状态，含 hw_version/status/fifo_status/dma_ctrl）、
  `kern.raphael_dcs_level`（写 0..2047 触发一次）。
- 包头打包器已 host 测试：对 0/1/2/0xff/0x100/0x123/0x3ff/0x7fe/0x7ff 与下游算法逐字节一致，
  2048 与空指针被拒。

**已上机验证（2026-11-13，内核 `3ca6b55` + `raphael_dcs=1`）**

读回来的寄存器（`kern.raphael_dsi`）：

```
hw_version=0x20030000   -> major 2, minor 3：寄存器独立印证了 DT 的 v2.3
status=0x00000005       -> bit2 置位 = command-mode MDP 正在推帧
fifo_status=0x00001010
dma_ctrl=0x14000000     -> BIT(28) EMBEDDED + BIT(26) LPM，硬件本来就是这个模式
```

写入结果：

```
sysctl -w kern.raphael_dcs_level=2047
  -> writes=1 last_level=2047 last_error=0
     status 0x05 -> 0x00（不再忙），fifo_status 0x00001010 -> 0x11111010
sysctl -w kern.raphael_dcs_level=120
  -> writes=2 last_level=120 last_error=0
```

写入后显示继续正常扫描（`swaps` 834→872、`scanout_latched` 692→718 持续增长），
没有 panic、没有 SMC/DSI 错误、没有丢帧。**即 DCS 0x51 到达了控制器且被接受。**

**用户已确认亮度可见变化**，`last_error=0` 且写入后显示持续扫描。硬件发送路径达标；仍未完成的是把 sysctl 接入系统亮度滑块，以及熄屏/唤醒恢复验证。`0x52` 回读仅在需要独立面板寄存器证据时再实现，不是当前亮度写入的阻塞项。

> 注意：`COMMAND_MODE_DMA_CTRL` 在发送窗口内被切到 EMBEDDED 模式，若此时正好有一帧
> command-mode DMA 在排队，可能出现一帧闪烁后自恢复。这是 opt-in 的理由，也是先测中间值
> 而不是先测两端的理由。

早前试过并撤回的方案：`kern.raphael_dsi_request=1` 触发一次性 `ml_io_map` 的纯只读探针。
它能编译，但等于让 root 直接对未声明 ownership 的 DSI 发 MMIO load，
无硬件证据证明安全返回，故弃用；现在的实现只在显式 opt-in 下动，且带 idle 前置与超时。

### P0 显示帧率、黑块与 scanout 退化

当前确认：单 VIG0 direct scanout 能正常显示；0271/0275 降低了早期 latch/SwapWait 串行开销，且锁存超时低。近期 late-present 采样均值 38.23 swaps/s（峰值 54.17），当前无延迟镜像最近单秒样本 33.73 swaps/s；这些窗口未严格控制同一手势，不能当作稳态基准，未证明持续 60 fps。近期 `scanout_declines` 增长，下一项低风险诊断是 opt-in `sweetfb_holdflush=1`：当 VIG0 flush 超过现有 2 ms 窗口仍 pending 时返回 `EBUSY`，让 0275 回退此帧到 CPU copy，而不是覆盖 pending flush。它可能增加 copy/延迟，只用于判断“覆盖未 latch flush”对伪影/回退的影响，不预设为性能修复。

用户已确认 `sweetfb_scanout=0` 下黑块仍在、开关 direct scanout 对手感影响不明显。`0280` IOVA pin、`0279` timeout-drop 与 `0281` fresh-fence wait 均未解决黑块；`0283` late-present 尚无用户视觉改善证据，保持 opt-in。根因尚未确定。公开 UniXNU GPU/display 指南支持将同一触控区间的 GPU commit/completion、scanout submit、flush/latch 对齐；当前全局 fence snapshot 在 `0281` 中立即通过（`scanout_us_max≈624 µs`），可能没有覆盖 CA 后续才提交的绘制。

**历史串行基线**：早期实测每帧平均 **latch 等待 16.2 ms + SwapWait 9.9 ms**（`pace` 行 `latch_us=1623225`、`swap_wait_us=992400` 是 ×100 的定点数），当时约 30 fps、端到端延迟 40–60 ms。0271/0275 已修正该特定等待路径；这些旧数字不代表当前稳态性能。

1. `sweet_scanout_show()` 里的 `sc_flush_idle()`：同一个 SSPP 不能有两个未 latch 的 flush，
   所以每个 swap 都要等上一帧被面板吸收（一个 60 Hz 帧 = 16.2 ms）。
2. `swapWaitInner()`：每 swap 再等 1.5 帧（9.9 ms）。

**已在 0271 落地（`8a61ab3`）**：
- `fSwapSync` 在 scanout 模式默认关（`sweetfb_swapsync=1` 仍可开）：surface 已 hold 到后面的帧
  latch，CA 改不了它，提交即答复 → 省 9.9 ms。
- `sc_flush_idle` 改成最多 2 ms，pending flush 被下一次写入覆盖，计入 `sc.replaced` → 省 16.2 ms。

**已在 0275 落地（`b7908b3`）——这条是真正卡顿的可疑来源**：
旧顺序是「先 `sweet_scanout_show()` 写新 flush，再 `zcPoll()`」。问题是新 flush 一写下去，
pending 位重新置起，`latchedNow()` 立刻变 false，于是上一帧其实早已 latch 的事实被自己挡住，
退休永远晚一拍；hold 攒到 4 个就进 `waitLatch()` 忙等最多 25 ms，而这发生在 work loop 上，
把 vsync、early present、zcPoll 一起卡住。0275 改成：

1. **提交前**先 `zcPoll()` 退休已 latch 的帧，再查 `fZcCount >= 4`，满了直接 decline（走 CPU 拷贝回退），
   不再同步等一整帧；
2. **映射/清 cache 之前**就 `sweet_hold(s, true)`，保证 CA 在映射和 `sweet_dma_sync_va` 期间
   不能复用这个 surface；IOVA 映射失败或 `show()` 在编程/触发之前拒绝时，`sweet_hold(s, false)` 撤销。

`show()` 的非 0 返回全部发生在写 `SRC0`/`CTL_FLUSH` 之前，所以「拒绝时没有 MMIO 拥有这个引用」成立——
这条不变量是 0275 提前释放的依据，改 `show()` 时必须保持。

**历史诊断能力（`0277`，`3ca6b55`）**：`zcPresent()` 的 scanout 分支提前 `return true`，
绕过了 `fSt.zc`/`zcAbs`/`fZcFrames`；0277 加入专用字段（`kern.sweet_fb` 行尾），这些计数仍用于诊断，但不能单独解释黑块根因：

| 字段 | 含义 |
|---|---|
| `scanout_frames` | `scanoutPresent()` 接受了这一帧（**提交**，不等于上屏） |
| `scanout_declines` | 被拒（容量/IOVA/几何/管道），这一帧走了 CPU 拷贝 |
| `scanout_latched` | `zcPoll()` 看到 flush 位清 0（**真的到了面板**） |
| `scanout_us` / `scanout_us_max` | 进入 `scanoutPresent()` 的耗时均值 / 窗口内峰值 |

**正确使用边界**：`scanout_frames - scanout_latched` 只表示提交与观察到 latch 的累计差，不等价于当前硬件队列深度；flush 位可能被新提交复用。`scanout_declines` 表示回 CPU copy 的次数，需结合 decline reason、同窗 `copy_frames` 与帧提交/latch 时间线解释。`scanout_us` 只测 CPU submit path，不是 GPU 渲染时长。不要仅凭 `scanout_latched < scanout_frames` 推断单 SSPP 替换是唯一瓶颈或直接启用双 pipe。

比较帧率时必须用同一动态操作、同一时间窗的 per-second 计数；静态桌面/锁屏的 0–10 swaps/s 不是性能指标。`zc_s` / `zc_us` 只代表 low-pool 零拷贝，不能衡量 direct scanout。

**实验分支：双 SSPP 乒乓，保留但关闭**

`0272` 已有 VIG0/VIG1 骨架，当前 `sweet_scanout.c` 中 `sc.pp=false`。不要直接打开；若未来证据表明单管 submit/latch 是瓶颈，必须先解决以下硬件与所有权条件：

1. **VIG1 的取数配置从未初始化**。`show()` 只写 `SRC0_ADDR/YSTRIDE/SRC_SIZE/XY/OUT_*` 和
   QSEED，从不写 `SRC_FORMAT (+0x30)`、`UNPACK_PATTERN (+0x34)`、`SRC_OP_MODE (+0x38)`。
   VIG0 继承 ABL 的工作配置，VIG1 没有这个保证；`SC_SAVED_SSPP` 里也没有这三个。
   探测到 `QSEED3 HW_VERSION=0x1004` 只证明「这是个 VIG」，不证明它能取数。
2. **LM `CONST_ALPHA` 的打包方向写反了**。Linux `dpu_hw_lm.c`
   （`setup_blend_config_combined_alpha`）是 `const_alpha = (bg & 0xff) | ((fg & 0xff) << 16)`，
   即 fg 在高 16 位。原代码 `VIG1_ON = 0x000000ff` 实际是 fg=0/bg=255。
   `b7908b3` 已修正常量与注释，但在 `pp=false` 下是死代码，重开时必须实测。
3. **surface hold 模型按单 pipe 设计**。`zcPoll()` 在一帧 latch 后释放除最新之外的全部 hold，
   而双 pipe 会同时持有两个 surface。释放后 CA 可重绘，甚至映射槽会被回收 → 残留旧帧或 SMMU fault。
   要重开必须改成「每 pipe 各自持有 current/queued，直到该 pipe 被替换且 flush 确认退休」。

视觉症状正好对应：屏幕极暗、`display on` 后立刻 `display off`、触控看起来没反应。

**测量提醒**：`latch_us` / `swap_wait_us` 只在有帧提交时才有数。实测那几轮锁屏是静态的
（`swaps` 几十次/分钟），抓 170 s 可能一个 swap 都没有。要测延迟，必须先让界面动起来
（滑动、开 app、或切到动态壁纸），否则日志里全是 0。

### P2 多图层与动态岛 UI

此前一次 MobileGestalt `CacheExtra[DeviceSupportsDynamicIsland]` 读取返回 `True`；最近一次 `dynamic-island.sh show` 因 cache missing 失败，因此当前 capability 值未核验。即使为 `True` 也只是 UI capability，不是 SpringBoard 已显示动态岛的视觉确认，也不代表 DPU 多图层完成。

直扫当前只接受「单图层、整 canvas」帧：多个 layer 会退回 CPU copy。已有滑动日志记录的是 layer 0 单层整 canvas，未证明桌面编辑操作确实提交多层；旧 copy path 逐层覆盖写而非 alpha blend。后续多图层工作应先取得目标场景的真实 layer ID/rect/format/alpha 记录，再决定 CPU 合成修复还是硬件 blend，不假设双 pipe 是必然前置。

**注意这和「动态岛」不是同一件事**：
- 动态岛 UI 由 SpringBoard 根据 MobileGestalt `DeviceSupportsDynamicIsland` 决定，与 DPU 图层数无关；cache 当前需恢复后再只读验证，勿在 cache 缺失时盲目写入。
- 硬件多层是独立目标：需要多 SSPP + LM blend 或 multirect。只有确认真实 layer 负载后，再基于该配置设计 pipe fetch、alpha 与 surface lifetime；不为 Dynamic Island capability 本身启用多层 scanout。

参考：andromeda `0207` 也是单层限制；本仓 `0211/0213` 有 HUD 图层的 blend stage 做法
（`SWEET_MDP_LM_BLEND0 0x20`、`BLEND_STRIDE 0x18`、FG_ALPHA/BG_ALPHA/OP `0x602`）。

### 次要维护项
- `fScanoutMaps[]` 使用 LRU 淘汰；熄屏时映射可能保留到当前启动结束（最多 4 个约 10.96 MB 的映射）。目前不是已证实用户故障；若要回收，需确保 DPU 已不再读取对应 IOVA 后再 unmap。
- `fScanoutRefused` 目前未单独导出；`scanout_declines` 已由 0277 覆盖常规失败计数，除非需要逐类拒绝统计，否则不单独加接口。
- `0127` low pool 的 beryllium 硬编码与 raphael direct scanout 无关，保留为跨机型历史提示。

---

## 8. 补丁自检（每次改补丁必跑）

**两件事都要做，缺一不可**：hunk 计数正确 ≠ 补丁能应用。`b7908b3` 之前那次 CI 失败
（`f9360bc`，run `43243143613`）就是 hunk 计数全绿、但 `bsd/dev/sweet_spmi.c:96` 的上下文
文本经 0262 之后已经变了，`git apply` 直接拒绝，fatal hook 让构建 4 分钟就退出。

### 8.1 真应用（首选，能抓上下文冲突）

```sh
python3 tools/raphael/check-patch-series.py              # 默认查 sweet_spmi.c + SweetFramebuffer.cpp
python3 tools/raphael/check-patch-series.py --only bsd/conf/files
python3 tools/raphael/check-patch-series.py --only iokit/Kernel/arm/SweetFramebuffer.cpp --through 275
```

它在临时目录 `git init`，从最早的 `new-file` diff 重建目标文件，再按文件名顺序逐个
`git apply`。任何一次真实拒绝就非零退出。`--check-hunks-only` 只验计数，**不要**用它替代真应用。

### 8.2 hunk 计数

```python
import re, glob
bad = 0
for p in sorted(glob.glob('xnu/patches-11215/*.patch')):
    L = open(p, errors='replace').read().split('\n'); i = 0
    while i < len(L):
        m = re.match(r'^@@ -(\d+),(\d+) \+(\d+),(\d+) @@', L[i])
        if not m: i += 1; continue
        oc, nc = int(m.group(2)), int(m.group(4)); i += 1; o = n = 0
        while i < len(L):
            s = L[i]
            if s.startswith('@@') or s.startswith('diff --git') or s == '': break
            if s.startswith('+'): n += 1
            elif s.startswith('-'): o += 1
            elif s.startswith(' '): o += 1; n += 1
            else: break
            i += 1
        if (o, n) != (oc, nc):
            print('MISMATCH', p, oc, nc, o, n); bad += 1
print('mismatched hunks:', bad)
```

整系列验证（在新 clone 上）：除 3 个 hfs 补丁外应全部应用（`skip` 只会是 `0031/0041/0155`，
需要 stage 脚本装 hfs 源码，dxb 里会由 `0026-stage-hfs.sh` 提供）。

---

## 9. 参考实现索引（找「完整 GPU 方案」从这里开始）

| 来源 | 看什么 |
|---|---|
| **MiCode/Xiaomi_Kernel_OpenSource** `raphael-p-oss` @ `663db50e195a8a54c8565ac4ab0c6d9ba7d80db0` | 主要设备级依据：`xiaomi-sm8150-common.dtsi:329-337`（`bl_ctrl_dcs` 2..2047）、`dsi-panel-ss-fhd-ea8076-cmd.dtsi`（`bl-dcs-type-ss`）、`dsi_panel.c:746-755`（WLED/DCS 选择）、`dsi_ctrl_hw_cmn.c:705`（v2.3 FIFO 发包）、`dsi_ctrl.c:942-983,1161-1178`（包头/padding/last） |
| **Linux** `drivers/gpu/drm/panel/panel-samsung-ams639rq08.c` | DCS `0x51`/`0x52` 用法、max 2047 / init 1023、亮度前后临时退 LPM |
| **Linux** `drivers/gpu/drm/msm/disp/dpu1/dpu_hw_lm.c` | `setup_blend_config_combined_alpha`：`const_alpha = (bg&0xff) \| ((fg&0xff)<<16)` —— 双 pipe 重开时必须按这个校验 |
| **Linux** `dpu_hw_sspp.c` / `catalog/dpu_5_0_sm8150.h` | VIG 取数配置（`SRC_FORMAT`/`UNPACK`/`OP_MODE`）、VIG0 `0x4000`→本板 `+0x1000`、VIG1 `0x6000`→`0x7000`、LM0 `0x44000`→`0x45000` |
| **giannoccarol/UniXNU-andromeda** | `xnu/andromeda_scanout.c`（**同 SoC 实测算通**，29.6–30.4 fps）、`andromeda_ctl.c`（只读 DPU 探针 `andctl dpu`）、patch `0201/0202/0205/0207/0208` |
| **UniXNU/GopFramebuffer** | `NativeLegacyDisplay.cpp`：`dpu-scanout=2`、GPU fence 等待（200 µs 重试 / 250 ms 慢等待）、`fb-gpu=1` hand-off |
| **UniXNU/Adreno740** | scanout 内存类型 `0x5343`、`fenceProgress()` 前缀、device memory 跳过 cache 维护、持久化 import |
| **UniXNU-sheng** | `porting.toml` 里 `dpu-scanout` 的 apps-SMMU stage-1 做法与实测结论 |
| 本仓 `docs/FORK-SCAN-AND-BUGS.md` | 13 个机型 fork 的横向对比 + 本仓显示路径的 bug 清单（B1–B7） |
| `ios16-reference/display-research-20261112/RESEARCH.md` | 上述 DCS 结论的完整取证：18 份参考文件 + SHA256 清单 + 未验证项 |

---

## 10. 文件索引

- 补丁：`xnu/patches-11215/0268/0269/0270/0271/0275-*.patch`、`0137-beryllium-reboot-mode.patch`
- 源码：`xnu/patches-11215/sweet_scanout.c`、`sweet_scanout.h`、`0268-stage-sweet-scanout.sh`
- 工具：`tools/raphael/check-patch-series.py`（改补丁后必跑）、`capfast.py`、`raphael-base.adt`、`pack-mdp-tests.sh`、`mkboot-raphael.sh`
- 最近 A/B 镜像：`ios16-reference/validation-20261113/448fd78/boot-late.img`（0283 opt-in）与 `boot-base.img`（同 kernel、无 late-present）；细节见 `ios16-reference/validation-20261113/448fd78/retest/RESULTS.md`。当前手机运行 `boot-base.img` 对照参数。
- 历史基线：`ios16-reference/validation-20261112/boot-0271-recovery.img`（0271 单管，已上机验证可显示）；不是当前构建。
- 文档：`docs/FORK-SCAN-AND-BUGS.md`、`docs/HANDOVER-GPU-SCANOUT.md`（本文）
- CI：`.github/workflows/xnu.yml`
