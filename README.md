# UniXNU-raphael

## Status of this fork (jackyhowh), 2026-11-05

Tested on a Mi 9T Pro (`raphael`, 6 GB RAM). **iOS 16.7.16 reaches the SpringBoard home screen, and the touchscreen works.**

| Area | State |
| --- | --- |
| Boot chain | `fastboot boot` → Project Aloha UEFI → UniXNU menu → DarwinBoot → XNU (ios-sweet `SWEET` kernel, 8 CPUs) |
| Root | iOS 16.7.16 HFS+ root flashed to `userdata`, mounted read-write ([FLASH-ROOT.md](FLASH-ROOT.md)) |
| Display | Home screen, status bar, dock and app icons; CPU rendering through the UEFI framebuffer (1080×2340 panel presented as 1125×2436) |
| Touch | Goodix GT9886 (Normandy protocol) on QUP19 I2C at 0x5d: [patches/0112](patches/0112-raphael-touch-gt9886-normandy.patch), boot-arg `sweettouchboard=1`; taps and swipes reach iOS |
| Buttons, USB console | Hardware buttons work; the kernel's USB CDC console enumerates (1209:0001). Read it from WSL via usbipd; the Windows `usbser` driver stalls after the first 1 KiB |
| Not working | GPU acceleration (CPU rendering only); wallpaper and lock screen (black: PosterBoard finds no poster extensions); widgets stay "Waiting…"; Wi-Fi, cellular, audio, camera, sensors |

### Fixes this fork needed (branch [`diag-firmware`](https://github.com/jackyhowh/UniXNU-raphael/tree/diag-firmware))

1. **Firmware without XNU support.** `build-raphael.yml` never adds UniXNU and DarwinBoot to the raphael firmware (the guacamole patch only edits `oneplus-guacamole`), so its image stops at the UEFI recovery screen. Use `firmware-only.yml` with `variant=full`; it reuses an already built kernel.
2. **Kernel window too large for 6 GB.** The 3 GiB window overran the top of RAM by 16 MiB. `dram_size` = `owned_memory_size` = `0xbe000000` in `board.toml`.
3. **Window allocation rejected by Mu.** Mu's `ConvertPages` refuses one `AllocatePages` that spans several memory-map entries. `patches/darwinboot-window-split.patch` reserves the window one entry at a time; `darwinboot-window-below4g.patch` prefers a window below 4 GiB.
4. **Boot hangs right after AppleCLCD starts.** The kernel's CPU driver matches `qcom,kryo470`, but `scripts/devices.py` gave raphael `qcom,kryo485`. The CPUs never started, and XNU waited forever in `PE_lockdown_iokit` for `max_cpus`. Fixed in `devices.py` (the guacamole entry has the same mismatch).
5. **GUI covered by diagnostics.** Boot-args `sweetfb_noconsole=1` and `sweetpanel=0` stop the text console and the 20-second panel digest from painting over the GUI. Keep `unentitled_ios_sim_launch=1`: without it launchd is killed (CS_KILLED) and the kernel panics.

Build the firmware with the prebuilt kernel (run `41798632101`):

```
gh workflow run firmware-only.yml -R jackyhowh/UniXNU-raphael --ref diag-firmware \
  -f variant=full -f device=xiaomi-raphael -f target=RELEASE -f kernel_run_id=41798632101
```

Upstream text follows.

---

**Boot Apple's XNU (iOS) on the Redmi K20 Pro / Mi 9T Pro (`raphael`, Snapdragon 855 / SM8150) and reach the iOS SpringBoard desktop — from a stock, unlocked phone, with no macOS, no private source, and no manual kernel work.**

This repository is a *turnkey* device port built on the [UniXNU](https://github.com/UniXNU/UniXNU) framework. It reuses the proven **OnePlus 7 Pro (`guacamole`)** SM8150 bring-up (which already reaches the iOS 16.7.x userland) because `raphael` is the **exact same SM8150 silicon** — only the device tree and the panel resolution (1080×2340) differ. The SM8150 XNU kernel and the Project Aloha UEFI are shared, so there is **no per-device kernel defconfig to obtain**.

> Everything is produced by GitHub Actions. You only need a GitHub account and an unlocked phone.

---

## What you get

A single Android `boot-raphael.img` that you boot temporarily with `fastboot boot` (nothing is written to your device's partitions). The boot chain:

```
fastboot boot boot-raphael.img
   └─ Project Aloha UEFI (SM8150, xiaomi-raphael)  ── SM8150_EFI.fd
        └─ UniXNU boot menu
             └─ GUACESP FAT payload (64 MiB)
                  ├─ kernel.macho        (SM8150 XNU, from guacamole's build)
                  └─ raphael ADT         (iPhone X D22AP identity + raphael panel)
                       └─ DarwinBoot → XNU → UFS root → launchd → SpringBoard
```

---

## One-click build (no understanding required)

1. **Fork** this repository to your own GitHub account.
2. Go to **Actions → “build-raphael”** and click **Run workflow**.
   - The workflow does everything: on a free `macos-14` runner it builds the SM8150 XNU 8796 kernel the same public way `guacamole` does (`dipa-studio/ios-sweet` → `blacktop/darwin-xnu-build`, `MACHINE_CONFIG=SWEET`); then on `ubuntu-latest` it checks out the UniXNU driver and Project Aloha UEFI, fetches the raphael/SM8150 device tree, automatically extracts the iPhone X (`D22AP`) base Apple Device Tree from the public IPSW, builds the UEFI firmware volume, packs the FAT payload, and produces the boot image.
3. When it finishes, download the **`boot-raphael.img`** artifact.

That's it. Every input the build needs is either in this repo or fetched from a **public** source — there is no private kernel defconfig and no manual IPSW work.

---

## Boot it on the phone

Prerequisites (one-time, on your computer): install [Android platform-tools](https://developer.android.com/tools/release-notes-sdk-platform-tools) so you have `adb` and `fastboot`, and **unlock the bootloader** of the phone.

```bash
# phone in bootloader/fastboot mode (power + volume-down, or `adb reboot bootloader`)
fastboot boot boot-raphael.img
```

`fastboot boot` loads the image into RAM only — it does **not** flash or modify any partition, so it is safe and reversible (a normal reboot returns you to Android). Watch the screen for the UniXNU menu, then the iOS desktop.

> **The iOS root itself has to be flashed once.** The kernel mounts the root
> from the phone's `userdata` partition and that partition must hold an
> **HFS+ volume** (not the `SHNGROOT`/APFS image that `unixnu build userdata`
> produces — this kernel has no `SHNGROOT` support at all). See
> **[FLASH-ROOT.md](FLASH-ROOT.md)** for the download, verify and flash steps.
> The `ios-root-hfs` workflow builds the image with the upstream bring-up
> tweaks applied and publishes it as a release.

If your device has **6 GB** of RAM (instead of 8 GB), open `board.toml` and lower `payload.adt.owned_memory_size` (e.g. `0x80000000`) before building.

---

## How it works (for the curious)

| Piece | Source | Why |
|-------|--------|-----|
| SM8150 XNU kernel | `dipa-studio/ios-sweet` → `blacktop/darwin-xnu-build` on `macos-14` (`MACHINE_CONFIG=SWEET`) | The exact public path guacamole uses; identical silicon ⇒ identical kernel. **No private defconfig** — this is the standard XNU 8796 build. |
| Project Aloha UEFI | `Project-Aloha/mu_aloha_platforms` + 3 SM8150 patches | Boots XNU on SM8150. The tree **already ships a first-class `xiaomi-raphael` device directory**, so nothing is cloned — `TARGET_DEVICE=xiaomi-raphael` selects it and derives `FDT=raphael.dtb` |
| `raphael.dtb` | The **real raphael DTB** shipped by Aloha: `Device/xiaomi-raphael/DeviceTreeBlob/Android/android-raphael.dtb` (`get-raphael-dtb.sh`) | Not a reused/guess DTB — Aloha has one for this exact phone. (The sibling `Linux/linux-raphael.dtb` is a 6-byte placeholder and is skipped.) |
| `base-d22ap.adt` | Auto-extracted from the public iPhone X (D22AP) iOS 16.7.16 IPSW (`get-base-d22ap.sh`) | Provides the real `/product` identity MobileGestalt expects |
| `raphael` ADT | `scripts/mkadt.py --device raphael` | D22AP identity + raphael 1080×2340 panel + SM8150 MMIO |

### Why `MACHINE_CONFIG=SWEET` is right for an SM8150

`ios-sweet`'s public XNU tree defines the ARM64 board config `SWEET` (originally for the Redmi Note 10 Pro, SM7150). It is **not SoC-specific** — it is a generic Qualcomm ARM64 profile, and every value matches SM8150/Kryo 485:

- Qualcomm's architected-timer PPI numbering → virtual timer **INTID 19** (the ADT profile uses the same value)
- **GICv3**, `GICR_PE_SIZE 0x20000`, two security states (Group 0 owned by TrustZone)
- **16 KiB pages**, `-march=armv8.2-a+crypto`, **no pointer authentication** (Kryo 485 has none)

So the publicly buildable `SWEET` kernel is the correct kernel for raphael; no private per-device XNU config exists or is needed.

The `scripts/` directory contains the full, self-contained toolchain (`adt.py`, `devices.py`, `mkadt.py`, `build-firmware.py`, `build-payload.py`, `build-bootimg.py`, `get-raphael-dtb.sh`, `get-base-d22ap.sh`). You can reproduce the whole build locally; the only network input is the public IPSW (for the D22AP base tree) and the public git trees.

---

## Repository layout

```
board.toml                 device definition (SoC, firmware, payload, bootimg)
patches/                  the 3 SM8150 patches (unixnu / aloha / darwinboot)
scripts/
  adt.py                  Apple Device Tree parse/serialize
  devices.py              per-board hardware profiles (sweet/guacamole/raphael)
  mkadt.py                builds the minimal ADT XNU needs
  make-raphael-adt.py     ADT step for raphael (D22AP + raphael profile)
  build-firmware.py       builds the UEFI firmware volume (xiaomi-raphael)
  build-payload.py        packs kernel + ADT into the GUACESP FAT payload
  build-bootimg.py        packs the Android boot image
  get-raphael-dtb.sh      fetches the SM8150 device tree
  get-base-d22ap.sh       auto-extracts the iPhone X base ADT from the IPSW
.github/workflows/build-raphael.yml   the turnkey CI
```

---

## Known limitations (inherited from the SM8150 bring-up)

- **No SEP / secure enclave** ⇒ cannot sign in to an Apple ID or use cellular/baseband. The boot is set up as a dev-fused, activated board so the UI still reaches the desktop.
- **Bluetooth / cellular / fingerprint** are not expected to work.
- This is a **temporary boot** (`fastboot boot`); it does not persist across a reboot.
- Userland is **iOS 16.7.x** (iPhone X / D22AP restore build).

---

## Credits

- [UniXNU](https://github.com/UniXNU/UniXNU) — the UEFI/XNU boot framework.
- [Project Aloha](https://github.com/Project-Aloha) — SM8150 UEFI.
- `wochgn/UniXNU-guacamole` — the proven SM8150 (OnePlus 7 Pro) port this device reuses.
- `dipa-studio/ios-sweet` — the XNU 8796 build harness.
- Apple's iPhone X (D22AP) device tree is extracted from a public IPSW for identification only.
