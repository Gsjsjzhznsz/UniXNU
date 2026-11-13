# GPU 直扫交接文档（raphael / SM8150 / Adreno 640 / iOS 18.7.8）

> 目标：iOS 的合成结果由 DPU **直接扫描**，CPU 不再拷贝 10.9 MB/帧。
> 结论：**直扫已跑通（单 VIG0）**，`copy_frames` 停在启动初期。**60 fps 与亮度都还没达标**，
> 双 pipe 乒乓已写但默认关闭。未解决项见 §7。

---

## 0. 一句话现状（2026-11-13 更新）

CoreAnimation 交出的 IOSurface 通过显示流自己的 stage-1 页表被 **VIG0** 直接读取，QSEED3 做
`1125x2436 → 1080x2340`，每帧 CPU 只做 20 来次 MMIO 写。

- **已确认可用**：`mode=direct`、`copy_frames` 不随运行增长（累计 3–4，全在启动初期）、
  GPU `submitted == completed`、`hangs=0`。滑动窗口实测 `swaps_s` 到过 **41.5–52.4**，
  静态锁屏时是 0–10（用户态本来就不出帧）。
- **未达标**：目标 60 fps；`coalesced` 在同一窗口还有 ~10/s，说明到达 submit 的帧被合并；
  亮度/唤醒未解决（见 §7 P0，已确认走 DCS 而不是 WLED）。
- **不可用**：`0272` 双 VIG 乒乓——`sweet_scanout.c` 里 `sc.pp` 已强制 `false`，原因见 §7 P1。

最近一次上机实测（内核 `f54a0a3`，2026-11-12，滑动中）：

```
mode=direct hz=59 up_s=2333 win_ms=1010
swaps=4030 swaps_s=41.54 coalesced=90 coalesced_s=9.89
copy_frames=3 latch_waits=42 vsync_s=58.36 swapsync=0
gpu submitted=22945 completed=22945 hangs=0
```

最近一次构建（内核 `b7908b3`，2026-11-13，**未上机**）：CI `43332422963` success，
`errors.txt` 空，镜像已打好：
`ios16-reference/validation-20261112/b7908b3/boot-b7908b3.img`
（sha256 `158893890687b293df52cb5889f7b0d06cd1586bebca356098e7baaaa726865d`）。

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
| `0275-scanout-retire-before-submit.patch` | **本文件当前唯一在用的性能修复**：提交前退休已 latch 帧 + 容量预检（去掉 25 ms work-loop 忙等）；映射/清 cache 前 hold surface，失败撤销 |
| `0272`（在 `sweet_scanout.c` 内，**默认关闭**） | VIG0/VIG1 乒乓骨架，`sc.pp = false`，见 §7 P1 |
| `0137-beryllium-reboot-mode.patch`（改动） | 把 reboot-to-bootloader 开放到 raphael（PM8150 PON spare + IMEM + PSCI fallback），用于自愈循环 |

> **已删除**：`0273-raphael-pm8150l-wled-read.patch`。它给 raphael 开 SID 5 的 WLED 读白名单，
> 但亮度实际走 DSI DCS（§7 P0），写了也点不亮，且会把后来人引到错误方向。

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

> 并行会话已在 `1944d59` 落地 **0272：VIG0/VIG1 乒乓**（`sweet_scanout.c` +156 行）。若你接手时
> 已在跑，先复测 §0 的 fps/latch 数字再看下面两条。

### P0 背光与唤醒（用户最直接的手感问题）
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

**下一步**：按 DCS 实现亮度。SM8150 DSI0 = `0x0ae94000`/`0x400`，下游 `compatible` 是
`qcom,dsi-ctrl-hw-v2.3`，对应 `dsi_ctrl_hw_cmn_kickoff_fifo_command`（无需 cmd-DMA 分配）。
包头按下游 `dsi_ctrl.c`：长写 DT `0x39`、word count 3、`last` bit7、payload 补 `0xFF`；
`0x51` 的 payload 是 `51 hi lo`。**先只读**：确认 controller idle/clock/ownership，再串行发
一次小幅亮度写并用 `0x52` 回读。不要在缺少 power/ownership 的情况下裸读 MMIO——
无时钟时 load 可能 stall 或 abort。

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

**未上机验证**。测试顺序（手机在 fastboot 时）：
1. `kern.sweet_fb` 先确认 `mode=direct`、有画面（DSI 必须在被驱动状态）；
2. 读 `kern.raphael_dsi` 记录 `status`/`hw_version`（控制器不提供可信版本号，
   下游是从 DT compatible 选 v2.3，所以版本只作记录）；
3. 写 `kern.raphael_dcs_level=1023`（中途值），观察画面亮度与 `last_error`；
4. 成功再试 2 与 2047 两端，确认 `0x52` 回读需要另加（当前未实现回读）。

> 注意：`COMMAND_MODE_DMA_CTRL` 在发送窗口内被切到 EMBEDDED 模式，若此时正好有一帧
> command-mode DMA 在排队，可能出现一帧闪烁后自恢复。这是 opt-in 的理由，也是先测中间值
> 而不是先测两端的理由。

早前试过并撤回的方案：`kern.raphael_dsi_request=1` 触发一次性 `ml_io_map` 的纯只读探针。
它能编译，但等于让 root 直接对未声明 ownership 的 DSI 发 MMIO load，
无硬件证据证明安全返回，故弃用；现在的实现只在显式 opt-in 下动，且带 idle 前置与超时。

### P1 串行等待（"拷贝已去掉但还是卡"的真因）
实测每帧平均：**latch 等待 16.2 ms + SwapWait 9.9 ms**（`pace` 行 `latch_us=1623225`、
`swap_wait_us=992400` 是 ×100 的定点数，即 16.23 ms / 9.92 ms），一帧 33 ms → ~30 fps，
但端到端延迟 40–60 ms，所以滑动手感仍然差。`copy=0` 说明不是带宽/拷贝问题，是**串行**：

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

**0271 的测量盲点（还没修）**：`zcPresent()` 的 scanout 分支提前 `return true`，绕过了
`fSt.zc`/`zcAbs`/`fZcFrames` 和 `scanAddr()` 的 `tr->scan`，所以 `zc_s`、`zc_us`、
`scan_replaced`、trace 的 `scan_us` 都**不反映直扫**；`fSwapScanID` 只代表「已提交 flush」，
真正的 latch 观察在 `zcPoll()` 的 `fSwapDone`。要判断 0275 的收益，看
`swaps_s`（同场景滑动）、`coalesced`、以及日志里 `MDP flush ... did not latch within 25 ms`
是否消失，不要看 `zc_s`。

**剩下的一条：双 SSPP 乒乓已实现但默认关闭（`b7908b3`）**
`1944d59`/`d6156fa`/`f54a0a3` 落地了 0272，但**上机不可用**，`sweet_scanout.c` 里
`sc.pp` 已强制为 `false`。三个独立缺陷，缺一不可开工：

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

### P2 多图层（动态岛 / 状态栏叠加）
现在只接受「单图层、整 canvas」的帧：`zcPresent()` 遇到多个置位图层直接
`zcDecline(1, "several layers are composed by the copy path")` 退回 CPU 拷贝。

**注意这和「动态岛」不是同一件事**：
- 动态岛的 UI 是 SpringBoard 用 MobileGestalt 的 `DeviceSupportsDynamicIsland` 决定画不画的，
  与 DPU 图层数无关；打开它只需要改 MobileGestalt 缓存（`tools/raphael/dynamic-island.sh on`），
  不需要改 scanout。
- 真正的 P2 是**硬件合成多图层**：要多一个 SSPP（VIG1）+ LM blend stage，或走 multirect。
  它和 §7 P1 的双 pipe 是同一批前置条件（VIG1 取数配置、`CONST_ALPHA`、每 pipe 独立 hold），
  所以**必须先把 P1 那三条修完**，否则就是在放不安全的双 pipe 上再叠一层。

参考：andromeda `0207` 也是单层限制；本仓 `0211/0213` 有 HUD 图层的 blend stage 做法
（`SWEET_MDP_LM_BLEND0 0x20`、`BLEND_STRIDE 0x18`、FG_ALPHA/BG_ALPHA/OP `0x602`）。

### P3 映射槽与内存回收
`fScanoutMaps[]` 只做 LRU 淘汰、不做超时回收；熄屏 `zcReleaseAll()` 后映射仍在（最多 4 个
10.96 MB 的映射）。长期跑要加回收。

### P4 其它
- `kern.sweet_fb` 的 `zc_frames` 不统计扫描路径（那是 low-pool 的计数器），建议加
  `scanout_frames` 计数，便于回归。
- `fScanoutRefused` 只有自增没有输出。
- 0127 的 low pool 仍是 beryllium 硬编码；与 raphael 无关，可留作 beryllium 用。

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
| **MiCode/Xiaomi_Kernel_OpenSource** `raphael-p-oss` @ `663db50e195a8a54c8565ac4ab0c6d9ba7d80db0` | 亮度唯一权威：`xiaomi-sm8150-common.dtsi:329-337`（`bl_ctrl_dcs` 2..2047）、`dsi-panel-ss-fhd-ea8076-cmd.dtsi`（`bl-dcs-type-ss`）、`dsi_panel.c:746-755`（WLED/DCS 选择）、`dsi_ctrl_hw_cmn.c:705`（v2.3 FIFO 发包）、`dsi_ctrl.c:942-983,1161-1178`（包头/padding/last） |
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
- 镜像：`ios16-reference/validation-20261112/b7908b3/boot-b7908b3.img`（当前最新，未上机）；`ios16-reference/ios18-image/boot-ios18-scanout.img`（旧）
- 基线：`ios16-reference/validation-20261112/boot-0271-recovery.img`（0271 单管，已上机验证可显示）
- 文档：`docs/FORK-SCAN-AND-BUGS.md`、`docs/HANDOVER-GPU-SCANOUT.md`（本文）
- CI：`.github/workflows/xnu.yml`
