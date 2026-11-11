#!/usr/bin/env bash
# Stage the GPU scanout of patch 0268 (sweetfb_scanout, off by default): the
# display reads iOS's frames itself instead of the CPU copy into the panel
# memory (full GPU acceleration: GPU renders into the surface, the DPU scans
# it directly). sweet_scanout.c owns the display's SMMU bank and pipe;
# sweet_scanout.h is shared with iokit/Kernel/arm/SweetFramebuffer.cpp, which
# 0268 patches.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
test -f bsd/conf/files
mkdir -p iokit/Kernel/arm
install -m 0644 "$here/sweet_scanout.c" "$here/sweet_scanout.h" bsd/dev/
install -m 0644 "$here/sweet_scanout.h" iokit/Kernel/arm/
