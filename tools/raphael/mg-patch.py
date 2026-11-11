#!/usr/bin/env python3
"""Patch the on-device MobileGestalt cache so a non-island iPhone identity reports
Dynamic Island capability.

The iOS 18 userland carries the key `DeviceSupportsDynamicIsland` (found in SpringBoard /
UIKit strings of the 22H352 dyld cache) and SpringBoard picks the island status bar shape
from it. MobileGestalt answers it from the device identity (`D22` / iPhone11,8 here), so on
our port it is false and SpringBoard draws the notch (XR) status bar instead.

MobileGestalt caches its answers in
  /private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/
    Library/Caches/com.apple.MobileGestalt.plist
under `CacheExtra`. Rewriting one key there is the smallest, fully reversible override:
delete the plist (or `--revert`) and MobileGestalt regenerates the stock answers.

usage:
  mg-patch.py IN.plist OUT.plist KEY=VALUE [KEY=VALUE ...]
  mg-patch.py --show IN.plist

VALUE is 1/0/true/false. Existing keys are only changed when the value differs, everything
else in the plist (CacheData/CacheVersion and the other CacheExtra keys) is preserved.
"""
import plistlib
import sys

CACHE_EXTRA = 'CacheExtra'


def load(path):
    with open(path, 'rb') as f:
        return plistlib.load(f)


def save(path, plist):
    with open(path, 'wb') as f:
        plistlib.dump(plist, f, fmt=plistlib.FMT_BINARY, sort_keys=False)


def parse_value(text):
    if text in ('1', 'true', 'True', 'yes'):
        return True
    if text in ('0', 'false', 'False', 'no'):
        return False
    return text


def show(path):
    plist = load(path)
    extra = plist.get(CACHE_EXTRA, {})
    keys = sys.argv[3:] or ['DeviceSupportsDynamicIsland']
    print('cache keys: %s' % ', '.join(sorted(plist)))
    print('CacheExtra keys: %d' % len(extra))
    for k in keys:
        print('  %-40s %r' % (k, extra.get(k, '<unset>')))


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    if sys.argv[1] == '--show':
        show(sys.argv[2])
        return
    src, dst = sys.argv[1], sys.argv[2]
    plist = load(src)
    extra = plist.setdefault(CACHE_EXTRA, {})
    if not isinstance(extra, dict):
        sys.exit('%s is not a dictionary in %s' % (CACHE_EXTRA, src))
    changed = []
    for spec in sys.argv[3:]:
        if '=' not in spec:
            sys.exit('expected KEY=VALUE, got %r' % spec)
        key, _, text = spec.partition('=')
        value = parse_value(text)
        old = extra.get(key, '<unset>')
        if old != value:
            extra[key] = value
            changed.append('%s: %r -> %r' % (key, old, value))
    if not changed:
        print('nothing to change (already set)')
    for line in changed:
        print('changed ' + line)
    if dst != '-':
        save(dst, plist)
        print('wrote %s (%d CacheExtra keys)' % (dst, len(extra)))


if __name__ == '__main__':
    main()
