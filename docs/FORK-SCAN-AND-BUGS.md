# 全机型 fork 扫描 + 本仓显示路径代码审查

扫描日期：2026-11-12。目标：为 raphael（Redmi K20 Pro / SM8150 / Adreno 640）
找其他机型 fork 里可移植的显示/GPU/scanout 优化，并审查本仓 sweetfb / MDP /
zero-copy 补丁的真实缺陷。

---

## 1. 结论速览

1. **同 SoC（SM8150）的其他 fork 全都没有 zero-copy**：cepheus、guacamole、
   flashlm、alphaplus、andromeda 都是「ABL 留屏 + CPU memcpy」，和 raphael
   起点一样，没有 MDP/DPU/QSEED 驱动。它们能提供的只有少量性能/时序修复。
2. **真正的 zero-copy 在 UniXNU 新版架构里**：`UniXNU/GopFramebuffer` +
   `UniXNU/Adreno740` + `UniXNU-sheng`（SM8550）。核心是 `dpu-scanout=2`：
   **显示 bank 装一个 apps-SMMU stage-1 窗口（TCR2 SEP bit 17:15，IOVA bit31
   走翻译），DPU VIG pipe 直接按 IOVA 取 compositor 的 surface**，配 GPU
   fence-before-flip。这恰好绕开 raphael 「MDP 流是 TRANSLATE 不是 BYPASS」的死结。
3. **本仓 zero-copy（0127）目前对 raphael 是死路，不是参数问题**：low pool
   的窗口地址和 reserved-memory 表全是 Poco F1（SDM845）硬编码，
   `sweet_lowpool_validate()` 拿 beryllium 的表去校验 raphael 的内存，
   等于没校验。`sweetfb_zcforce=1` 会把它放到 raphael 的固件 carve-out 上 →
   SoC 复位（与之前 zcforce 黑屏一致）。
4. **本仓已有 beryllium 全套性能补丁**（0216 合并、0243 一步黑屏、0210 统计、
   0235/0240/0241 scan-out copy/GPU blit），cephes 那几项「优化」我们早已包含，
   不需要回移。

---

## 2. 扫描范围

本地树：`ios16-reference/jacky-ios18`（本仓）、`ios16-reference/ios-raphael`、
`ios16-reference/ios-sweet`、`/tmp/hx`（hackios-xnu-kernel）、
`/tmp/ir`（ios-raphael）。

远端 13 个机型 fork：

| # | 仓库 | SoC | scanout 方式 | zero-copy | GPU 驱动 |
|---|------|-----|-------------|-----------|----------|
| 1 | UniXNU/GopFramebuffer | 通用 | gop-handoff + `dpu-scanout=2` | **是** | — |
| 2 | UniXNU/UniXNU-sheng | SM8550 | gop-handoff | **是** | Adreno740 + AdrenoMetal |
| 3 | UniXNU/Adreno740 | SM8550 | gop-handoff | **是** | 原生 A740 kext |
| 4 | UniXNU/UniXNU-sm8550-common | SM8550 | none | 否 | GMU/SMMU 底座 |
| 5 | YangQi0408/UniXNU-cepheus | SM8150 | cpu-copy | 否 | 只读探测 |
| 6 | wochgn/UniXNU-guacamole | SM8150 | gop-handoff | 否 | 无（外部 ios-sweet） |
| 7 | drx1x2/UniXNU-flashlm | SM8150 | gop-handoff | 否 | 无 |
| 8 | dragon120820/UniXNU-sdm845-common | SDM845 | none | 否 | 无 |

待补（后台扫描中）：cocopaim15/UniXNU-alphaplus（SM8150）、
giannoccarol/UniXNU-andromeda（SM8150）、FFT-infra/UniXNU-alioth（SM8250）、
reAAAq/UniXNU-umi（SM8250）、keyfan/UniXNU-enchilada（SDM845）、
johnpanos/UniXNU-fajita（SDM845）。按已扫的同族模式，预期不引入新的显示
机制，仅补充 SoC 级事实。

---

## 3. 可移植优化（按对 raphael 的价值排序）

### ★★★ A. `dpu-scanout=2`：apps-SMMU stage-1 窗口 + DPU 直接取 surface
来源：`UniXNU/GopFramebuffer`（README；`NativeLegacyDisplay.cpp:56`）、
`UniXNU-sheng`（`porting.toml:142,182`）。
做法：给显示 bank 的 context 装 stage-1 页表（原生 4K 表），TCR2 的 SEP
位段 `17:15` 置位，使 IOVA bit31 的地址走翻译；把 MDP/SSPP 的 SRC0 指向
该 IOVA，而不是要求 SMMU BYPASS。
**对 raphael 的意义**：这是解决「MDP 流是 TRANSLATE cb2」的正解。比
identity-map（PA==IOVA）更通用，且不依赖 bypass 假设。

### ★★★ B. GPU fence-before-flip（硬件已验证）
来源：`UniXNU-sheng/porting.toml:184`——加上 fence 等待后，面板上
「已显示 surface 内容变化」从 808 次降到 2 次。零拷贝一旦让 GPU 与
scanout 共享 surface，这一步是防撕裂/闪烁的必要条件。
本仓 0220 的 `latchedNow()` / `waitReplace()` 已经是同类机制，但只针对
MDP latch，没有对 `sweet_gpu_fence_passed` 的显式等待——0241 用
「同 ring 排队」绕过了它。若改走 stage-1 IOVA 直取，应补显式 fence 等待。

### ★★ C. Adreno740 的 scanout 内存与 fence 移交
- `scanout` 内存类型（0x5343）：直接把固件 framebuffer 的
  IOMemoryDescriptor 交给 compositor，uncached 映射，原地读写。
  `NativeAdrenoPublish.cpp:614-620`。
- `fenceProgress()`：跨两个优先级 ring 求「所有 ≤F 的 fence 都完成」的
  前缀，让显示端只等一个前缀。`NativeAdrenoSubmit.cpp:366-379`。
- **设备内存跳过 cache 维护**：`pmap_valid_address()` 门控所有 DC 操作，
  对非 DRAM 的 scanout 页绝不 `dc cvac`/`phystokv`（会 panic）。
  `NativeAdrenoCpu.cpp:28-40`——raphael 的 low pool 若在 xnu 管理范围外，
  必须同样处理。
- **去掉每帧 TLBIALL**：只在 fence 停滞时失效 TLB，去掉后单次运行少
  了 10944 次页表遍历。`NativeAdrenoSubmit.cpp:608-610`。
- **持久化 import**（kIOMemoryPersistent）：compositor 的 scanout surface
  在 GPU 仍使用时允许 owner 先 unmap。`NativeAdrenoMemory.cpp:345-360`。

### ★★ D. SM8150 一致性法则（flashlm 实测）
来源：`UniXNU-flashlm/HANDOVER.md:162-181`。
**TTBR1/内核 VA 写到 framebuffer 后，即使 `dc cvac`/`civac` + `dsb sy`，
对「caches-off 直读 DRAM」的引擎仍不可见；只有 identity（VA=PA）写才可见。**
这条直接约束 raphael 的零拷贝 cache 维护：如果 MDP 以 bypass/identity
方式读，内核 VA 写的帧必须先经 VA=PA 别名写，或改用可缓存 low pool +
显式 clean（0127 的 `sweet_dcache_clean_va` 走的就是后者，但依赖 SMMU 语义）。

### ★ E. SMMU 原生 4K 表与失败即毒化
来源：`UniXNU-sm8550-common/ShengGpuSmmu.c:48-68,95-103`。
用原生 4K 表绑定 SID 到自有 context bank；**禁用 context 前先把所有
stream 打到 fault**，避免禁用银行后出现 stage-1 bypass 窗口。失败即
毒化、绝不自动重试。这对 raphael 的 apps_smmu 操作是可移植的纪律。

### ★ F. qsmmu-v500 识别（SDM845-common）
`dts/sdm845.dtsi:205-211`：apps SMMU 是 `qcom,qsmmu-v500` 而非 MMU-500，
XNU 的 ArmSmmu4K 没有 V500 路径。SM8150 同族，确认 raphael 的
apps_smmu 也应按 V500 处理（当前 0094 直接按寄存器操作，需核对）。

### 不需要回移的项
cepheus 的「swap 合并 / 去每帧 malloc / 一步黑屏 / 绝对截止时间」四项：
本仓 0216（合并 + swapdepth）、0243（步进黑屏）、0210（统计）已覆盖；
绝对截止时间（`wakeAtTime`）本仓用 0220 的「+500us 过边界」方式替代，
二者等价性需上机验证，但 cepheus 版本并不更强。

---

## 4. 本仓代码审查（显示路径）

审查文件：`0126-beryllium-fb-hw-scaler.patch`、
`0127-beryllium-fb-zero-copy.patch`、`0220-beryllium-fb-direct-latch-safety.patch`、
`0241-beryllium-fb-scanout-blit-async.patch`、`0263/0264/0265-raphael-*`。

### 严重（阻断 raphael zero-copy）

**B1. low pool 全表是 beryllium 硬编码** — `0127` 第 373-407 行
```
SWEET_LOWPOOL_DEF_BASE 0xb0400000
SWEET_LOWPOOL_MIN      0x9f800000   /* end of cont_splash_region */
SWEET_LOWPOOL_MAX      0xfd000000   /* rmtfs */
sweet_lowpool_known[] = { {0x85700000,"hyp"}, ..., {0x9d400000,"cont_splash"},
                          {0xaf000000,"ramdump_fb"}, {0xb0000000,"ramoops"} };
```
注释自己写明是「Poco F1 live DT」。`sweet_lowpool_validate()`
（`0127:443-472`）只拿**这张 beryllium 表** + splash + `gPhysBase/gPhysSize`
做重叠检查，**完全不读 raphael 的 live DT reserved-memory**。
后果：raphael 上默认窗口要么被 `MIN/MAX` 或 xnu RAM 拒掉（安全但 zc 永不
启用），要么落进 beryllium 表里没有、但 raphael 实际存在的固件 carve-out
（hyp/tz/modem/adsp/cdsp 在 SM8150 地址不同）→ 越界映射 → 复位。
这就是 `sweetfb_zcforce=1` 黑屏的根因之一。

**B2. zc 门槛要求 SMMU BYPASS，而 raphael 是 TRANSLATE cb2** — `0126:424-430`
```
int a = sweet_smmu_probe_sid(0x880, ...);
int b = sweet_smmu_probe_sid(0xc80, ...);
fZcOK = a == 1 && b == 1;   /* 1 = bypass */
```
raphael 实测 `smr3 id 0x800 mask 0x420 type TRANSLATE cb2`，`0x880/0xc80`
根本不存在 → `fZcOK=false` → `zcPresent()` 第一道就 decline
（`0127:1224-1226`）。`sweetfb_zcforce=1` 强行置 true，但 low pool 仍是
B1 的 beryllium 地址 → 危险。**必须换 B1/B2 的组合方案**（见 §5）。

### 中等

**B3. `waitReplace()` 可重入无上界** — `0220:124-145`
```
while (*flush & fLastFlush) { if (超时) return false; ... }
if (!latchedNow()) { return waitReplace(); }   /* 递归 */
```
若 flush 位反复回置，递归每层再等 20 ms，理论上可深递归。实际 latch 后
位不再回置，属潜在风险。

**B4. `sweet_mdp_restore` 的 addr/stride 两条直写未做边界检查** — `0126:847-850`
```
sweet_mdp_restore.r[sweet_mdp_restore.n].off = po + SSPP_SRC0_ADDR;  /* 无 n<MAX 检查 */
sweet_mdp_restore.r[sweet_mdp_restore.n++].val = fBootSrc;
```
`SWEET_RESTORE_ADD` 宏有 `n < SWEET_MDP_RESTORE_MAX(56)` 检查，这两条没有。
今天 28(pipe_regs) + 2(border) + 2 = 32 < 56，不触发；但数组若增长会越界。

**B5. 0265 延迟 `mdpProbe()` 可能从 statsTick 上下文 `ml_io_map`** — `0265:91-99`
`if (fMdp == 0) { fMdp = ml_io_map(...); }`。正常路径 `start()` 已映射，
tick 3/6/9 只重读；但若 `start()` 映射失败，会在 vsync 统计上下文里做
`ml_io_map`（该上下文不宜做重操作）。

### 轻微

**B6. `sweetfb_sid_a/b` 被解析两次** — `0263:91-93`（局部 `sidA/sidB`）与
`0265:45-46`（全局 `gSidA/gSidB`）。值一致，无功能 bug，仅冗余。

**B7. 0241 scan-out GPU blit 的缓冲复用** — `0241:88-91`：在飞的
`fScanBlitT` 缓冲被复用且「永不上屏」，CPU 回退路径要等它落地
（`0241:146-158`）。逻辑自洽但极难静态验证；若等待超时仍会叠加。

### 参考实现自身的 bug（不属本仓，仅记录）
- `GopFramebuffer/NativeLegacyDisplay.cpp` 经 `LegacyDisplayProbe` 用
  硬编码字节偏移（5472/5480/5820/…）读原显示对象，ABI 脆弱。
- 诊断计数器非原子，跨上下文读写有 benign race。
- `interpose()` 直接写 vtable 指针，arm64e（带 PAC）上静默失效。

---

## 5. raphael zero-copy 的正确路线

现有 0126/0127 的 beryllium 路线（BYPASS + Poco F1 low pool）在 raphael 上
不可行。两条可行路线：

### 路线 1（推荐）：照搬 GopFramebuffer 的 stage-1 窗口
1. 在 apps_smmu 的显示 context bank（cb2）装原生 4K stage-1 页表，
   TCR2 SEP `17:15` 置位，使 IOVA bit31 走翻译（参考 sheng `dpu-scanout=2`）。
2. 把 compositor 的 canvas surface（BGRA、64B 行对齐）映射进该 cb2 的
   IOVA；MDP/SSPP SRC0 写 IOVA。
3. 补 GPU fence-before-flip：swap 完成前等 `sweet_gpu_fence_passed`。
4. 不再需要 `fZcOK == bypass`；`zcPresent()` 的门槛改为「surface 已映射进
   cb2」。
5. 复用本仓 0127 的 hold/cooling/缓存 clean（`sweet_dcache_clean_va`），
   但 low pool 换成「按 cb2 已映射的 IOVA 分配」，或直接映射 xnu 已管理
   的高地址 RAM（走 SMMU 翻译，不再受 4 GiB 限制——这是 stage-1 路线的
   最大红利）。

### 路线 2（保守）：raphael 专用 low pool + identity-map
1. 用 `sweetfb_smmu_scan=1` 读出 raphael 真正的 SMR/S2CR，确认 MDP 流。
2. 写 raphael 专用 `sweet_lowpool_known[]`（从 raphael live DT 抄
   reserved-memory）+ 正确 `MIN/MAX`。
3. 用 0094 的 `sweet_smmu_map_sid(0x800)` 把该窗口 identity-map 进 cb2，
   使 PA==IOVA，从而 MDP 在 TRANSLATE 下也能取到。
4. 仍需通过本仓 0127 的 hold/cooling 保证不重用。

路线 1 更彻底（摆脱 4 GiB 和 bypass 两个约束），但工作量大；路线 2 改动
小、能先跑通 zc_frames>0。

---

## 6. 立即可以做的（不改 0126/0127 上下文）

1. 新增 `0268`（**新文件**，不碰已有补丁上下文）：`bsd/dev/sweet_fb_raphael.c`
   提供 `sweet_lowpool_raphael_window(...)` / 或 stage-1 窗口初始化，
   并在 `0265` 的延迟块里调用（0265 是最后一个补丁，自包含）。
2. 在 0265 的延迟块里加：读 raphael live DT 的 reserved-memory 并打印，
   与 `sweet_lowpool_known[]` 对照——**先只读**，确认地址后再改映射。
3. 修 B4（restore 数组边界）用一个**末尾新补丁**，或等 0268 一起做。
4. 修 B3（`waitReplace` 递归改循环）。

---

## 7. 附：仍待扫描的 6 个 fork

见 §2 表格末尾；后台任务完成后再补结论。预期：SM8150 的 alphaplus/
andromeda 与 cepheus 同类（CPU-copy）；SM8250 的 alioth/umi、SDM845 的
enchilada/fajita 可能带 Adreno 6xx DPU 但与本仓 beryllium 路线重叠。
