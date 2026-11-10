# raphael display: why every boot ends in `mode=copy`, and what patch 0263 changes

## What the measurements said

`kern.sweet_fb` on raphael always reports `mode=copy hw=0 flip=0 zc_frames=0`, and
`copy_frames` tracks `swaps`, i.e. every presented frame is a CPU copy of the composited
surface. `sweetfb_direct=1` (image `boot-gui2`) and `sweetfb_hw=1 sweetfb_zc=1`
(`boot-gui3`) both booted and stayed in `mode=copy`.

## Root cause

Reconstructing the kernel sources locally (pristine `apple-oss-distributions/xnu` at tag
`xnu-11215.81.4` + the dxb patches + all 222 patches of `xnu/patches-11215`) shows that the
whole MDP block in `AppleCLCD::start()` is behind one check:

```c
if (sweet_board_beryllium()) {
        mdpProbe();                     /* read-only: which pipe scans the boot fb */
        ... sweetfb_hw  -> hwSetup()    /* QSEED3 scaler */
        ... sweetfb_flip-> flipSetup()  /* page flipping */
        ... sweetfb_smmu-> sweet_smmu_probe_sid(0x880 / 0xc80)
        ... sweetfb_zc  -> low pool + zero-copy scanout
        ... sweetfb_direct -> early swap path + swap latch
}
```

`sweet_board_beryllium()` is false on raphael, so on SM8150 none of it ever runs and the
boot arguments `sweetfb_direct` / `sweetfb_hw` / `sweetfb_zc` are parsed but have no effect.
That is the whole explanation for the `mode=copy` rows above, and it is a kernel issue, not a
boot-argument one: no boot image can turn those paths on.

## What the two reference repositories do and do not have

* `hackios-xnu-kernel` (`/tmp/hx`): `xnu/redwood.patch` touches 76 files, all `bsd/kern`,
  `bsd/vfs`, `bsd/miscfs` hardening plus `raphael_touch.c`, `raphael_buttons.c`,
  `raphael_adreno.c`. `grep -l SweetFramebuffer` there finds nothing: **no display/MDP work**
  to port.
* `ios-raphael` (`/tmp/ir`): `0097-display-sweetscale` (nearest-neighbour upscale inside the
  copy path), `0098/0100-display-brightness*`, `0061-display-size`, `0083-raphael-input`.
  Also copy-path only, no MDP.

So raphael's direct/zero-copy scanout has no reference implementation to lift; the register
data has to be read from the phone.

One useful fact from `0098`: raphael's DSI0 is at `0x0ae94000`, i.e. MDSS + `0x94000`, the same
layout as beryllium, whose MDP window this driver uses at `0x0ae00000`. The window is therefore
expected to exist on raphael; what is unverified is the DPU revision's pipe offsets and the
`apps_smmu` stream ids the MDP fetches with.

## Patch 0263 (`xnu/patches-11215/0263-raphael-fb-mdp-and-smmu-scan.patch`)

1. `gMdp = sweet_board_beryllium() || (sweet_board_raphael() && sweetfb_mdp=1)` gates the block.
   With `sweetfb_mdp=1` alone it is **read-only**: `mdpProbe()` and the SMMU probe. Nothing is
   written until `sweetfb_hw` / `sweetfb_direct` / `sweetfb_zc` are given as well.
2. Board data as boot arguments, so iterating does not need a kernel rebuild:
   * `sweetfb_mdp_pa=0x...` — MDP window base (default `0x0ae00000`)
   * `sweetfb_sid_a`, `sweetfb_sid_b` — MDP `apps_smmu` stream ids (default `0x880`, `0xc80`)
3. `sweetfb_smmu_scan=1` calls the new read-only `sweet_smmu_dump_smr()` (`bsd/dev/sweet_smmu.c`)
   which prints every valid SMR/S2CR (id, mask, type, cb), so the MDP's real stream ids can be
   read off the phone instead of guessed.
4. Diagnostics: a stats window longer than 3 s logs `SWEET FB: stats window N ms: vsync ticks
   stalled`. (Note: the `win_ms=8901` seen on one boot coincides with `SWEET FB: display off` /
   `display on` and only 15 swaps in that window, so it is probably the panel being off, not a
   freeze; this line settles it on the next boot.)

## Test images and how to judge them

Packed from the 0263 kernel with `tools/raphael/mkboot-raphael.sh` (only the FAT of the boot
image changes; `boot`/`dtbo` are never written, iOS root is untouched):

| image | extra boot-args | what to look for |
| --- | --- | --- |
| `boot-ios18-mdp-base.img` | – | control: `mode=copy`, `swaps_s` ~20-30 |
| `boot-ios18-mdp-probe.img` | `sweetfb_mdp=1 sweetfb_smmu=1 sweetfb_smmu_scan=1 sweetfb_statlog=1` | `SWEET MDP:` lines (pipe, QSEED3 version, CTL/LM), `SWEET SMMU: scan:` entries; no writes |
| `boot-ios18-mdp-hw.img` | `+ sweetfb_hw=1` | `SWEET FB: hw scaler: ...` (pipe must be VIG, QSEED3 0x1002/0x1003) |
| `boot-ios18-mdp-zc.img` | `+ sweetfb_hw=1 sweetfb_zc=1` | `mode=zc`/`direct`, `zc_frames` rising, `copy_frames` flat |

Pass criteria for the goal: `mode` is no longer `copy`, `zc_frames` grows, `copy_frames` stops,
`swaps_s` >= 40 or the perceived stalls disappear. Every image can be tried with
`fastboot boot` and rolled back by booting the old image; nothing is flashed.

## Upstream sync

`UniXNU/UniXNU` is 786 commits ahead of the fork point (different history, merged by subtree:
`uefi/`, `scripts/`, `patches/`, `board.toml` are the useful parts; our `xnu/patches-11215`
series stays the reference for iOS 18). `jackyhowh/UniXNU-raphael` has 0 new commits.

## Built images (2026-11-11 03:11, kernel run 42880069538)

Kernel `kernel.development.sweet`, sha256 `366dff29298f9ff60b75594b55eb93d70518b903aed4d067d5876b99c00bf4ff`
(19,005,040 bytes). Both patches are in it: the binary carries `MDP/DPU probe on raphael`,
`SWEET SMMU: scan:` and `SWEET FB: pace win=`.

Packed by `tools/raphael/pack-mdp-tests.sh` into `ios16-reference/ios18-image/`:

| image | sha256 | args that matter |
| --- | --- | --- |
| `boot-ios18-mdp-base.img` | `f274671beb576b2e77c1a0cbf8774745a925dfac63e3a6090940ebb1c838b309` | control |
| `boot-ios18-mdp-probe.img` | `ba27920ece0a9e7639b7878c14538d6eff23a3327719114a80c2c0b2e984a248` | `sweetfb_mdp=1 sweetfb_smmu=1 sweetfb_smmu_scan=1 sweetfb_statlog=1` |
| `boot-ios18-mdp-hw.img` | `e871ae96e160b73734a00913c4869d7be133b8906df042e8825f0f9078146bde` | `+ sweetfb_hw=1` |
| `boot-ios18-mdp-zc.img` | `ab8d847f06c4452e7b8396447e0d7596c24058aa41c1f06d39243f181faa227b` | `+ sweetfb_hw=1 sweetfb_zc=1` |

Each carries the same kernel (verified byte-identical to the artifact) in the FAT of a copy of
`boot-raphael-ios18-plog.img`; `boot`/`dtbo`/userdata are untouched, so `fastboot boot` is the
whole procedure and booting any older image rolls back.

### Reading the probe boot

`SWEET MDP:` lines give, per pipe (VIG0..3, DMA0..3), the source address/stride/size/format and,
for VIG pipes, the QSEED3 hardware version; the pipe whose source equals the boot framebuffer is
the one ABL left scanning (and the one `fPipe` becomes). `SWEET MDP: INTF1 frame count a -> b`
says whether the interface is actually scanning.

`SWEET SMMU: scan:` lists every valid SMR with its S2CR: `id`/`mask` identify the stream, `type`
BYPASS means the MDP's fetches are not translated (that is the condition `fZcOK` wants), TRANSLATE
means the driver would have to program a context bank. If no SMR covers the MDP, give the ids as
`sweetfb_sid_a/sid_b` and re-run with `sweetfb_smmu=1`.

`SWEET FB: pace ...` (patch 0264) then shows per second whether frames are held by the GPU gate
(`gpu_holds`, `hold_to`) and what a copy costs (`copy_us`), which is what decides whether the
direct/zc path is worth chasing on this board.

## 0265: the delayed probe (why the first probe boot showed nothing)

The probe prints in 0263 are made from `SweetFramebuffer::start()`, which runs about two seconds after
the kernel enters BSD - before the CDC-ECM gadget exists. The USB log thread flushes the last 64 KiB of
the console ring, and by the time the link is up that ring has already wrapped, so a probe boot shows
only `smmu=translate-or-unknown` in the stats line and none of the `SWEET MDP:` / `SWEET SMMU: scan:`
output. That is a log-transport effect, not a probe failure.

`0265-raphael-fb-mdp-late-probe.patch` caches `sweetfb_mdp`, `sweetfb_mdp_pa`, `sweetfb_sid_a`,
`sweetfb_sid_b` and `sweetfb_smmu_scan` in globals and repeats the probe from `statsTick()` at the 3rd,
6th and 9th one-second window (roughly 3 s, 6 s and 9 s after the first vsync):

```
SWEET FB: mdp late 3/9 gMdp=1 pa=0xae00000 sid_a=0x880 sid_b=0xc80 scan=1 mode=copy
SWEET FB: mdp late sid 0x880 -> 2, 0xc80 -> 2 (1 bypass, 2 translate, 3 fault, 4 mixed, 0 none, -1 unreadable)
SWEET MDP: ...            mdpProbe() re-reads (3rd and 9th window only; window mapped once)
SWEET SMMU: scan: ...     sweet_smmu_dump_smr() when sweetfb_smmu_scan=1
```

Everything stays read-only; `sweetfb_hw` / `sweetfb_direct` / `sweetfb_zc` are still what enables the
fast paths. `tools/raphael/pack-mdp-tests.sh` now also writes `boot-ios18-mdp-probe2.img` with this set.

How to read a probe2 boot:

| line | meaning | next step |
| --- | --- | --- |
| `mdp late ... pa=0xae00000` but no `SWEET MDP:` lines | the window does not map at that base | try `sweetfb_mdp_pa` from the DT (`mdss` node) |
| `SWEET MDP: ctl/lm ...` with plausible values | DPU window is there, pipes readable | boot the `-hw` image |
| `scan:` entries with a translating SMR whose id matches `mdss` iommus | that id is the MDP stream | set `sweetfb_sid_a/sid_b` and boot `-zc` |
| all SMRs `type BYPASS` | apps_smmu is bypassed for the display | zero-copy should not need the SMMU probe; go `-zc` |

## Built images for the 0265 probe (2026-11-11)

Kernel: run `42925435728`, `kernel.development.sweet` sha256
`02ea0b86c073ac2cc10cce8dbd90b3fe8d658d834ae2056a8a7be65b11951273` (19,005,0xx B; contains
`SWEET FB: mdp late %d/9 gMdp=%d pa=0x%x ...`).

| image (`ios16-reference/ios18-image/`) | sha256 |
| --- | --- |
| `boot-ios18-mdp-base.img` | `daecc76e06effdc3a55e1adcd80c4b29dbc36df5b89b2fb21b7829714fb2cc9a` |
| `boot-ios18-mdp-probe2.img` | `edf8e04ebf2965a6a57414c9834b7670b0252f59d6a7bb2dda84a210a74bda4a` |
| `boot-ios18-mdp-hw.img` | `a3e4cb5fcf75bdcf773298297749a18482fda6eaaae8a56a7a544a52461df2ef` |
| `boot-ios18-mdp-zc.img` | `eb0e4ce882b4e49e1a4e6c69a0763861b5b3884416676f69f6c34575fe095962` |

`probe2` arguments: `... sweetfb_statlog=1 sweetfb_mdp=1 sweetfb_smmu=1 sweetfb_smmu_scan=1`.

What to read in its log (all read-only):

1. `SWEET FB: mdp late 3/9 gMdp=1 pa=0xae00000 sid_a=0x880 sid_b=0xc80 scan=1 mode=copy`
   - `gMdp=0` means `sweet_board_raphael()` did not answer at start(): stop here.
2. `SWEET MDP: <pipe> src0 ... stride ...` and `SWEET MDP: ctl/lm ...`
   - no such lines + `SWEET MDP: map failed` -> wrong window, find `mdss` in the DT and pass `sweetfb_mdp_pa`.
3. `SWEET SMMU: scan: smr N ... type TRANSLATE|BYPASS|FAULT`
   - the SMR whose id matches the MDP stream (0x880/0xc80 on sdm845) tells you which ids to pass as
     `sweetfb_sid_a/sid_b`; all BYPASS means zero-copy does not have to go through the SMMU probe.
4. `SWEET FB: mdp late sid 0x880 -> n` afterwards confirms the probe with the chosen ids.

Then `boot-ios18-mdp-hw.img` (hardware scaler + page flip) and `boot-ios18-mdp-zc.img` (zero-copy);
roll back by booting `boot-raphael-ios18-plog.img`. No partition is written by any of these.
