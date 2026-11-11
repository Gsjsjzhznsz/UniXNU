#!/bin/sh
# Pack the raphael boot images that test the MDP fast paths (patches 0263/0264).
#
#   tools/raphael/pack-mdp-tests.sh KERNEL ADT BASE.img OUTDIR
#
# KERNEL is the kernel.development.sweet from the xnu workflow artifact, ADT the 12 KiB
# raphael.adt, BASE.img a working UniXNU-raphael boot image (its UEFI firmware and the
# Android boot header are reused; only the FAT is rewritten). Nothing here touches userdata,
# boot or dtbo: the images are booted with `fastboot boot` and rolled back by booting BASE.
#
# Produces in OUTDIR:
#   boot-ios18-mdp-base.img   control, same args as the current working image
#   boot-ios18-mdp-probe.img  sweetfb_mdp=1 sweetfb_smmu=1 sweetfb_smmu_scan=1 (read only)
#   boot-ios18-mdp-hw.img     + sweetfb_hw=1                      (QSEED3 scaler, page flip)
#   boot-ios18-mdp-zc.img     + sweetfb_hw=1 sweetfb_zc=1         (zero-copy scanout)
# All of them keep sweetfb_statlog=1 so the console gets one stats line per second.
set -eu
[ $# -eq 4 ] || { echo "usage: $0 KERNEL ADT BASE.img OUTDIR" >&2; exit 2; }
KERNEL=$1 ADT=$2 BASE=$3 OUTDIR=$4
HERE=$(cd "$(dirname "$0")" && pwd)
command -v mcopy >/dev/null || { echo "$0: needs mtools" >&2; exit 1; }
[ -f "$KERNEL" ] && [ -f "$ADT" ] && [ -f "$BASE" ] || { echo "$0: missing input" >&2; exit 2; }
mkdir -p "$OUTDIR"

# What every image needs: the working set plus the raphael root (userdata) in read-write mode.
# sweetlog stays 0 (the persistent log writes the end of userdata), the console is muted at the
# first swap and the diagnostic panel thread is off, as in board.toml's GUI set.
COMMON="debug=0xc serial=3 cpus=8 rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 \
launchd_unsecure_cache=1 wdt=-1 sweetfb_hz=60 sweetusb=2 sweetarm64e=1 sweetlog=0 sweetgpu=8 \
sweetgpufreq=g ufsrw=1 rootrw=1 sweetfb_noconsole=1 sweetpanel=0 sweetfb_statlog=1"

pack() { # NAME EXTRA-ARGS
	name=$1; extra=$2
	CMDLINE="$COMMON $extra" sh "$HERE/mkboot-raphael.sh" "$BASE" "$KERNEL" "$ADT" "$OUTDIR/$name"
}

pack boot-ios18-mdp-base.img  ""
pack boot-ios18-mdp-probe.img "sweetfb_mdp=1 sweetfb_smmu=1 sweetfb_smmu_scan=1"
pack boot-ios18-mdp-probe2.img "sweetfb_mdp=1 sweetfb_smmu=1 sweetfb_smmu_scan=1"   # 0265: late re-probe at 3/6/9 s
pack boot-ios18-mdp-hw.img    "sweetfb_mdp=1 sweetfb_hw=1 sweetfb_smmu=1"
pack boot-ios18-mdp-zc.img    "sweetfb_mdp=1 sweetfb_hw=1 sweetfb_zc=1"
pack boot-ios18-scanout.img   "sweetfb_mdp=1 sweetfb_scale=2 sweetfb_scanout=1"   # 0268: full GPU scanout, no CPU copy

echo "---"
for f in "$OUTDIR"/boot-ios18-mdp-*.img; do
	sha256sum "$f"
done
echo "boot with: fastboot boot $OUTDIR/boot-ios18-scanout.img   (0268: the display reads Metal frames itself)"
echo "judge by : 'SWEET MDP:' lines, 'SWEET SMMU: scan:' entries, mode= (copy/zc/direct),"
echo "           zc_frames rising and copy_frames flat, kern.sweet_fb 'pace' line (patch 0264)"
