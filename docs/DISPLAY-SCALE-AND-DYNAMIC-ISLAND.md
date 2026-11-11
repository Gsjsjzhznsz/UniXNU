# Display: status-bar cropping (fixed with a boot arg) and Dynamic Island (MobileGestalt)

raphael: Redmi K20 Pro, 1080 x 2340 panel, no notch, no island.
Userland: iOS 18.7.8, iPhone11,8 (D22) — reports 1125 x 2436 to UIKit.

## 1. Why the status bar was cut off

Patch 0061 (`0061-sweet-iphone-x-display-size`) presents the iPhone X logical panel
(1125 x 2436, 375 x 812 pt @3x) to userland so UIKit has status-bar/layout data at all
(without it UIKit logs "No status bar visual provider for screen"). The physical 1080 x 2340
panel is then shown as the **centred crop** of that logical frame:

```
fOffX = (1125 - 1080) / 2 = 22
fOffY = (2436 - 2340) / 2 = 48
presentCanvas(): dx -= fOffX; dy -= fOffY      /* logical row 48 -> panel row 0 */
```

So the top 48 logical pixels and the bottom 48 are dropped, plus 22 columns per side. The
status bar lives inside the top safe area (44 pt = 132 px), so its upper third is cropped —
the reported symptom.

## 2. The fix (boot image only, no root image change)

Patch 0126 already carries a CPU scaling path (`presentScaled`, nearest-neighbour column/row
maps) that is used *instead of* the crop when `sweetfb_scale != 0`:

```
fScale = 0  crop            (stock, cuts 48 px top/bottom)
fScale = 1  fit to panel    (1080 wide -> 1037 wide, ~21 px black bars left/right)
fScale = 2  stretch to panel (1080 x 2340, aspect 2436->2340 = 0.96, no bars)
```

Both keep the whole logical frame, so nothing of the status bar is lost.

| image | sha256 |
| --- | --- |
| `boot-ios18-scale1.img` (`sweetfb_scale=1`, fit) | `9101014c27f73cda6762e49950addc244d2e6e7b815632326cb7720d245e9015` |
| `boot-ios18-scale2.img` (`sweetfb_scale=2`, stretch) | `c5dbb0fd735b9f3e4ce236102b2f09a046dfff4856d5accdcd06cff5f11772f2` |

Both are the 0265 kernel (`mdp late` probe included) with `sweetfb_statlog=1` replaced by
`sweetfb_scale=N`, same file size as the other probe images, and nothing else changed.

Verify on the device:

```
fastboot boot boot-ios18-scale2.img
# serial console / UDP 5140:
#   SWEET FB: AppleCLCD 1080x2340 ... presented as 1125x2436
#   SWEET FB: stats ... mode=copy hw=0 ...          (fScale is CPU-side, mode stays copy)
# visually: status bar (clock, battery) fully visible, no clipped glyphs
```

Rollback: boot any other image (`boot-raphael-ios18-plog.img`), nothing is written.

Note: this is the CPU scaling path. Once the MDP hardware scaler (0126 `sweetfb_hw`) or
zero-copy (0127 `sweetfb_zc`) works on raphael, `sweetfb_scale=1` selects the QSEED3 scaler
instead and the same logical->panel mapping happens in hardware.

## 3. Dynamic Island

The iOS 18.7.8 userland **does** carry the capability: the 22H352 dyld cache contains
`DeviceSupportsDynamicIsland` plus the whole `DynamicIsland*` UIKit/SpringBoard class family
(`DynamicIslandContainer`, `DynamicIslandExpandedContent`, `DynamicIslandBackground`, ...).
SpringBoard only draws the island when MobileGestalt answers that key true; with our D22 /
iPhone11,8 identity it answers false, hence the XR (notch) status bar.

MobileGestalt composes its answers from the device identity in the ADT (`mkadt.py` writes
`target-type = D22`, i.e. iPhone XR) and caches them in

```
/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/
  Library/Caches/com.apple.MobileGestalt.plist        (dict: CacheData, CacheVersion, CacheExtra)
```

`CacheExtra` is the documented-by-practice override point: a key placed there wins over the
computed answer, and deleting the plist makes MobileGestalt regenerate the stock answers.

Two routes, cheapest first:

1. **Runtime override (no image change)** — `tools/raphael/dynamic-island.sh on`: dumps the
   cache over the USB link, sets `CacheExtra[DeviceSupportsDynamicIsland] = true` with
   `tools/raphael/mg-patch.py`, pushes it back, restarts `cfprefsd` and SpringBoard.
   `off` sets it back to false, `revert` deletes the cache file.
2. **Bake it in** — a first-boot job in the root image that applies the same key once the
   cache exists (the cache is created on the very first boot, so it cannot simply be shipped
   pre-seeded: it is keyed to the device tree).

Why not just change the ADT identity to an island device (iPhone 15 Pro)? That also changes
the logical display (393 x 852 pt = 1179 x 2556 px), the safe-area insets and the layout
expectations of every system app, while our framebuffer still presents 1125 x 2436. Mixing
those two is what produces odd layouts; flipping the single capability key keeps the iPhone X
geometry that the rest of the port is tuned for.

Verification (island is drawn, not just enabled):

```
tools/raphael/dynamic-island.sh show     # DeviceSupportsDynamicIsland  True
# screen: camera cutout pill centred in the status bar, clock left / indicators right
# SpringBoard logs: no "No status bar visual provider for screen"
# rollback: tools/raphael/dynamic-island.sh off   (or revert)
```

If the island appears but the layout is wrong (status bar height, pill position), the next
knob is the island geometry MobileGestalt reports (`DynamicIsland*` keys in the same
`CacheExtra`), which can be set the same way with `mg-patch.py KEY=VALUE`.
