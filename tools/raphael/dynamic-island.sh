#!/bin/sh
# Dynamic Island on raphael: flip MobileGestalt's DeviceSupportsDynamicIsland and restart
# SpringBoard. Reversible (`revert`), no boot image or root image change.
#
#   dynamic-island.sh show     print the cached key (and the island geometry keys we know)
#   dynamic-island.sh on       patch the cache to true, restart cfprefsd/SpringBoard
#   dynamic-island.sh off      patch it back to false and restart
#   dynamic-island.sh revert   delete the cache (MobileGestalt regenerates stock answers)
#
# Needs the USB link (tools/raphael/usblink.sh ssh) and python3 on the host.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
U=$HERE/usblink.sh
CACHE=/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist
TMP=${TMPDIR:-/tmp}/mg-cache.$$
KEY=DeviceSupportsDynamicIsland

fetch() {
	sh "$U" ssh "cat '$CACHE'" > "$TMP.in"
	python3 "$HERE/mg-patch.py" --show "$TMP.in" "$KEY"
}

push_and_restart() {
	sh "$U" ssh "cat > '$CACHE.tmp' && chmod 644 '$CACHE.tmp' && mv '$CACHE.tmp' '$CACHE' && ls -l '$CACHE'" < "$TMP.out"
	# cfprefsd caches the file; SpringBoard re-reads MobileGestalt when it restarts
	sh "$U" ssh 'killall -9 cfprefsd 2>/dev/null; kill $(ps -A -o pid= -o comm= | grep /System/Library/CoreServices/SpringBoard.app/SpringBoard | grep -v grep | cut -d" " -f1) 2>/dev/null; echo restarted'
}

case ${1:-} in
show) fetch ;;
on|off)
	val=$([ "$1" = on ] && echo 1 || echo 0)
	fetch
	python3 "$HERE/mg-patch.py" "$TMP.in" "$TMP.out" "$KEY=$val"
	push_and_restart
	;;
revert)
	sh "$U" ssh "ls -l '$CACHE' 2>/dev/null; rm -f '$CACHE'; echo removed; killall -9 cfprefsd 2>/dev/null; echo reverts on next SpringBoard start"
	;;
*) sed -n '2,12p' "$0"; exit 2 ;;
esac
rm -f "$TMP.in" "$TMP.out"
