#!/bin/bash
# 0267: let raphael's zero-copy run with an IDENTITY-MAPPED MDP stream.
#
# raphael's apps_smmu has the MDP stream in TRANSLATE (smr3: id 0x800 mask 0x420 -> CB2), so
# SweetFramebuffer's zero-copy (patch 0127) declines: "MDP SMMU streams not verified as bypass",
# and every frame keeps a 10.9 MB CPU copy. sweetfb_zcforce=1 alone makes the MDP scan a physical
# address the SMMU translates, which kills display init (no SWEET FB stats after boot).
#
# Fix: first build the identity-mapping context bank for the MDP sid (sweet_smmu_map_sid(), 0094:
# PA == IOVA holds afterwards), then treat "identity-mapped" like "bypass" in the zc decision.
# Boot args: sweetfb_zc=1 sweetfb_zcforce=1 [sweetfb_zc_sid=0x800].
set -eu
REL=SweetFramebuffer.cpp
F=$(find . -maxdepth 8 -name "$REL" -path "*iokit*" 2>/dev/null | head -1)
[ -n "$F" ] || F=$(find . -maxdepth 8 -name "$REL" 2>/dev/null | head -1)
[ -n "$F" ] || { echo "0267: $REL not found (cwd $(pwd))"; ls -d */ 2>/dev/null | head -8; exit 1; }
echo "0267: patching $F"
python3 - "$F" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
needle = 'if (force && !fZcOK) {'
if s.count(needle) != 1:
    sys.exit('0267: expected exactly one "%s" in %s, found %d' % (needle, p, s.count(needle)))
ins = (needle + '\n'
       '\t\t\t\t\textern int sweet_smmu_map_sid(unsigned sid);\n'
       '\t\t\t\t\tunsigned zsid = 0x800;\n'
       '\t\t\t\t\tPE_parse_boot_argn("sweetfb_zc_sid", &zsid, sizeof(zsid));\n'
       '\t\t\t\t\tint zmap = sweet_smmu_map_sid(zsid);\n'
       '\t\t\t\t\tIOLog("SWEET FB: zc: MDP sid 0x%x identity map -> %d\\n", zsid, zmap);\n'
       '\t\t\t\t\tif (zmap != 0) { IOLog("SWEET FB: zc: identity map failed, zero-copy stays off\\n"); goto zcno; }\n'
       '{')
s = s.replace(needle, ins)
open(p, 'w').write(s)
print('0267: identity-mapped MDP path inserted')
PY
