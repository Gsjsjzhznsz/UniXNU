#!/usr/bin/env bash
# Stage the raphael DCS brightness support (patch 0276 registers it).
#
# sweet_scanout.c owns scanout; this module owns one thing the scanout path
# cannot: sending a MIPI DCS 0x51 brightness packet over DSI0. It is inert
# unless the boot argument raphael_dcs=1 is given, so staging it changes
# nothing on a normal boot.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
test -f bsd/conf/files
test -d bsd/dev
install -m 0644 "$here/raphael_dsi.c" "$here/raphael_dsi.h" bsd/dev/
