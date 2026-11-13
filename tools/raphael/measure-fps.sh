#!/usr/bin/env bash
# Sample the raphael framebuffer counters once a second and print the per-second
# deltas that decide where a frame goes.
#
#   tools/raphael/measure-fps.sh [SECONDS] [HOST]
#
# Defaults: 20 samples, root@192.168.7.2. The phone must be booted (not in
# fastboot) and reachable over the USB gadget link.
#
# Why deltas: kern.sweet_fb publishes window rates, but swaps_s is rate of
# SwapEnd calls and includes frames that were never shown. The counters below
# separate "CoreAnimation produced it", "the pipe took it" and "it reached the
# panel":
#
#   swaps            SwapEnd calls            (producer)
#   coalesced        swaps that replaced an unshown frame
#   scanout_frames   scanoutPresent() accepted it   (pipe took it)
#   scanout_latched  flush bits observed to clear   (reached the panel)
#   scanout_declines refused, frame went to CPU copy
#
# Production limit   -> swaps/s flat, scanout_latched/s far below scanout_frames/s
# Pipe/latch limit   -> scanout_frames/s flat while swapwait/latch stalls appear
# Copy fallback      -> scanout_declines rising
set -euo pipefail

N=${1:-20}
HOST=${2:-root@192.168.7.2}
KEY=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/keys/raphael_ios_ed25519
SYSCTL=/private/var/jb/usr/sbin/sysctl

ssh_fb() {
	ssh -o BatchMode=yes -o ConnectTimeout=5 -o ServerAliveInterval=5 -i "$KEY" "$HOST" \
		"$SYSCTL -n kern.sweet_fb" 2>/dev/null
}

field() { # field <line> <key>
	printf '%s\n' "$1" | tr ' ' '\n' | sed -n "s/^$2=//p" | head -1
}

# The USB gadget drops out while the display sleeps; retry instead of aborting.
probe=
for _ in $(seq 1 10); do
	probe=$(ssh_fb) && [ -n "$probe" ] && break
	probe=
	sleep 2
done
[ -n "$probe" ] || { echo "cannot reach $HOST (phone booted? screen awake?)" >&2; exit 1; }
line=$probe

prev_swaps=$(field "$line" swaps)
prev_frames=$(field "$line" scanout_frames)
prev_latch=$(field "$line" scanout_latched)
prev_dec=$(field "$line" scanout_declines)
prev_coal=$(field "$line" coalesced)

printf '%s\n' "$line" | tr ' ' '\n' | grep -E '^(mode|hz|swapsync|copy_frames|scanout_us_max)=' | sed 's/^/  /'

printf '\n%4s %8s %8s %8s %8s %10s\n' sec swaps coal/s frames/s latch/s declines
for i in $(seq 1 "$N"); do
	sleep 1
	line=$(ssh_fb) || continue
	swaps=$(field "$line" swaps); frames=$(field "$line" scanout_frames)
	latch=$(field "$line" scanout_latched); dec=$(field "$line" scanout_declines)
	coal=$(field "$line" coalesced)
	printf '%4d %8s %8s %8s %8s %10s\n' "$i" \
		"$(( swaps - prev_swaps ))" "$(( coal - prev_coal ))" \
		"$(( frames - prev_frames ))" "$(( latch - prev_latch ))" \
		"$(( dec - prev_dec ))"
	prev_swaps=$swaps; prev_frames=$frames; prev_latch=$latch
	prev_dec=$dec; prev_coal=$coal
done

echo
echo "read: producer=swaps  pipe=scanout_frames  panel=scanout_latched  fallback=scanout_declines"
echo "want: scanout_latched close to scanout_frames and both near hz under a continuous swipe"
