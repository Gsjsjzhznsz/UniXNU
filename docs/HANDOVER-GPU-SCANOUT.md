# GPU 直扫交接文档（raphael / SM8150 / Adreno 640 / iOS 18.7.8）

> 目标：iOS 的合成结果由 DPU **直接扫描**，CPU 不再拷贝 10.9 MB/帧。
> 结论：**已跑通**，实测 ~23–30 fps、`copy_frames=2`（仅启动初期两帧）。剩余瓶颈见 §7。

---

## 0. 一句话现状

CoreAnimation 交出的 IOSurface 通过显示流自己的 stage-1 页表被 VIG0 直接读取，QSEED3 做
`1125x2436 → 1080x2340`，每帧 CPU 只做 20 来次 MMIO 写。**`swaps_s` 从 0.07 提升到 22.7–31**，
`copy_frames` 停在 2。还差的：背光/唤醒手感、latch 延迟、多图层（动态岛）。

最后一轮实测（内核 `66b36a2`，2026-11-12）：

```
mode=direct hz=59 hw=0 flip=0 smmu=untested
up_s=1047  swaps=894  swaps_s=22.71  copy_frames=2
pace win=1008ms swaps=31 copy=0 zc=0 gpu_holds=15606 hold_to=12 latch_us=1623225
scanout maps surface 199 @ iova 0x80000000
scanout maps surface 205 @ iova 0x81000000
scanout maps surface 208 @ iova 0x82000000
scanout maps surface 220 @ iova 0x83000000
```

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
| `0137-beryllium-reboot-mode.patch`（改动） | 把 reboot-to-bootloader 开放到 raphael（PM8150 PON spare + IMEM + PSCI fallback），用于自愈循环 |

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

### P0 背光与唤醒（用户最直接的手感问题）
现象：熄屏要按好几次电源；`SWEET FB: backlight fade: cannot read WLED string 0 brightness; pixel fallback`。
原因：`0126` 的 WLED 代码是 beryllium（PMI8998）的 `SID 3 / 0xd900`；raphael 的背光在
**PM8150L**，SID/基址不同。已用 `kern.sweet_spmi_addr` + `kern.sweet_spmi_dump` 在真机探测
`0xd957`（亮度）/`0xd846`（MOD_EN）的 SID 0–3，全部读失败（`--`），但 SID 2 @`0x4000` 能读
（说明探测路径本身可用）。**下一步**：从 raphael 的 downstream DTS / 内核源码确认 PM8150L 的
usid 与 `wled@d800` 基址，然后给 `sweet_scanout` 或 `0126` 加 raphael 分支。
背光一旦可用，熄屏可以平滑降亮，不必走 `memset` 兜底，唤醒手感也会正常。

### P1 串行等待（"拷贝已去掉但还是卡"的真因）
实测每帧平均：**latch 等待 16.2 ms + SwapWait 9.9 ms**（`pace` 行 `latch_us=1623225`、
`swap_wait_us=992400` 是 ×100 的定点数，即 16.23 ms / 9.92 ms），一帧 33 ms → ~30 fps，
但端到端延迟 40–60 ms，所以滑动手感仍然差。`copy=0` 说明不是带宽/拷贝问题，是**串行**：

1. `sweet_scanout_show()` 里的 `sc_flush_idle()`：同一个 SSPP 不能有两个未 latch 的 flush，
   所以每个 swap 都要等上一帧被面板吸收（一个 60 Hz 帧 = 16.2 ms）。
2. `swapWaitInner()`：每 swap 再等 1.5 帧（9.9 ms）。

**已在 0271 落地（`8a61ab3`，待有动画的界面下复测数字）**：
- `fSwapSync` 在 scanout 模式默认关（`sweetfb_swapsync=1` 仍可开）：surface 已 hold 到后面的帧
  latch，CA 改不了它，提交即答复 → 省 9.9 ms。
- `sc_flush_idle` 改成最多 2 ms，pending flush 被下一次写入覆盖，计入 `sc.replaced` → 省 16.2 ms。

**剩下的一条**：
- 双 SSPP 乒乓（VIG0/VIG1 轮流）：写 A 时 B 还在扫，彻底消掉那 2 ms 与单管串行。
  注意两条 pipe 的 CTL flush 位不同（VIG0=bit0、VIG1=bit1），QSEED3 要各自编程，LM0 blend
  两条都要使能。

**测量提醒**：`latch_us` / `swap_wait_us` 只在有帧提交时才有数。实测那几轮锁屏是静态的
（`swaps` 几十次/分钟），抓 170 s 可能一个 swap 都没有。要测延迟，必须先让界面动起来
（滑动、开 app、或切到动态壁纸），否则日志里全是 0。

### P2 多图层（动态岛 / 状态栏叠加）
现在只接受「单图层、整 canvas」的帧，多图层直接退回拷贝（日志 `several layers are composed`）。
**下一步**：第二个 SSPP（VIG1）或 multirect + LM blend；参考 andromeda 0207 的单层限制，以及
本仓 `0211/0213` 的 HUD 图层做法。

### P3 映射槽与内存回收
`fScanoutMaps[]` 只做 LRU 淘汰、不做超时回收；熄屏 `zcReleaseAll()` 后映射仍在（最多 4 个
10.96 MB 的映射）。长期跑要加回收。

### P4 其它
- `kern.sweet_fb` 的 `zc_frames` 不统计扫描路径（那是 low-pool 的计数器），建议加
  `scanout_frames` 计数，便于回归。
- `fScanoutRefused` 只有自增没有输出。
- 0127 的 low pool 仍是 beryllium 硬编码；与 raphael 无关，可留作 beryllium 用。

---

## 8. 补丁自检脚本（每次改补丁必跑）

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
| **giannoccarol/UniXNU-andromeda** | `xnu/andromeda_scanout.c`（**同 SoC 实测算通**，29.6–30.4 fps）、`andromeda_ctl.c`（只读 DPU 探针 `andctl dpu`）、patch `0201/0202/0205/0207/0208`（fence、异步 present、每 swap fence、扫描、surface cache） |
| **UniXNU/GopFramebuffer** | `NativeLegacyDisplay.cpp`：`dpu-scanout=2`、GPU fence 等待（200 µs 重试 / 250 ms 慢等待）、`fb-gpu=1` hand-off |
| **UniXNU/Adreno740** | scanout 内存类型 `0x5343`、`fenceProgress()` 前缀、device memory 跳过 cache 维护、持久化 import |
| **UniXNU-sheng** | `porting.toml` 里 `dpu-scanout` 的 apps-SMMU stage-1 做法与实测结论 |
| 本仓 `docs/FORK-SCAN-AND-BUGS.md` | 13 个机型 fork 的横向对比 + 本仓显示路径的 bug 清单（B1–B7） |

---

## 10. 文件索引

- 补丁：`xnu/patches-11215/0268-sweet-fb-gpu-scanout.patch`、`0269-sweet-fb-swapwait-nolatch.patch`、`0270-sweet-fb-scanout-perf-and-off.patch`、`0137-beryllium-reboot-mode.patch`
- 源码：`xnu/patches-11215/sweet_scanout.c`、`sweet_scanout.h`、`0268-stage-sweet-scanout.sh`
- 工具：`tools/raphael/capfast.py`、`raphael-base.adt`、`pack-mdp-tests.sh`、`mkboot-raphael.sh`
- 镜像：`ios16-reference/ios18-image/boot-ios18-scanout.img`（70,623,232 B，manifest 475 B）
- 文档：`docs/FORK-SCAN-AND-BUGS.md`、`docs/HANDOVER-GPU-SCANOUT.md`（本文）
- CI：`.github/workflows/xnu.yml`
