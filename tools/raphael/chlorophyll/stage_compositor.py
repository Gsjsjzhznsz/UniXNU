#!/usr/bin/env python3
"""Stage Chlorophyll as the iOS system compositor into a mounted root image.

backboardd links bgmtl-env.dylib (the Metal gate) FIRST and then
bgmtl-inject.dylib, MercuryPosterExtension links bgmtl-env.dylib (it already
links bgmtl-inject after Renegade's bgmtl()), both are re-signed ad hoc
keeping identifier and entitlements, bgmtl-env.dylib is installed, and the
backboardd inject is armed (bgmtl-bb.arm) with the Mercury deferred-schedule
marker (bgmtl.defer). Stock binaries are backed up to /var/root/bgmtl-backup.

usage: stage_compositor.py ROOT ENV_DYLIB
"""
import os, shutil, subprocess, sys, tempfile, struct

root = sys.argv[1]
envlib = sys.argv[2]

here = os.path.dirname(os.path.abspath(__file__))
ren = os.path.abspath(os.path.join(here, '..', '..', 'renegade'))
sys.path.insert(0, ren)
import root_tweaks18 as rt          # noqa: E402
import pacstrip                     # noqa: E402

ENV = '/usr/local/lib/bgmtl/bgmtl-env.dylib'
ENVL = ENV.lstrip('/')
BACKUP = 'var/root/bgmtl-backup'


def stage(binp_rel, libs):
    src = os.path.join(root, binp_rel)
    orig = os.path.join(root, BACKUP, os.path.basename(src))
    os.makedirs(os.path.dirname(orig), exist_ok=True)
    if not os.path.exists(orig):
        shutil.copyfile(src, orig)
        os.chmod(orig, 0o755)
    ents, ident = rt.entitlements_of(src), pacstrip.cs_identity(src)
    tmp = tempfile.mktemp(prefix='compositor-')
    shutil.copyfile(src, tmp)
    os.chmod(tmp, 0o755)
    for lib in libs:
        if not rt.macho_has_dylib(tmp, lib):
            subprocess.run([sys.executable, '-I', rt.ADDLC, tmp, lib], check=True)
    pacstrip.sign(tmp)
    n_ents, n_ident = rt.entitlements_of(tmp), pacstrip.cs_identity(tmp)
    assert n_ents == ents and n_ident[:2] == ident[:2], 'identity/entitlements changed for %s' % binp_rel
    shutil.move(tmp, src)
    os.chmod(src, 0o755)
    print('staged %s -> %s' % (binp_rel, [l.rsplit('/', 1)[-1] for l in libs]))


# backboardd: env BEFORE inject (the inject's constructor needs the gate already up)
stage('usr/libexec/backboardd', [ENV, rt.BGMTL_INJECT])

b = open(os.path.join(root, 'usr/libexec/backboardd'), 'rb').read()
ncmds = struct.unpack_from('<I', b, 16)[0]
off, order = 32, []
for _ in range(ncmds):
    c, sz = struct.unpack_from('<II', b, off)
    if c in (0xc, 0x80000018):
        o = struct.unpack_from('<I', b, off + 8)[0]
        order.append(b[off + o:off + sz].split(b'\0')[0].decode())
    off += sz
assert order.index(ENV) < order.index(rt.BGMTL_INJECT), 'bgmtl-env must load before bgmtl-inject'
print('backboardd dylib order ok:', order[-4:])

# Mercury: env only (bgmtl() already linked bgmtl-inject)
stage(rt.MERCURY_BIN, [ENV])

# install the env dylib and arm the compositor
shutil.copyfile(envlib, os.path.join(root, ENVL))
os.chmod(os.path.join(root, ENVL), 0o755)

bb_arm = os.path.join(root, 'private/var/tmp/bgmtl-bb.arm')
with open(bb_arm, 'w') as f:
    f.write('armed by stage_compositor\n')
os.chmod(bb_arm, 0o644)
for n in ('bgmtl-bb.off', 'bgmtl-bb.state'):
    p = os.path.join(root, 'private/var/tmp', n)
    if os.path.exists(p):
        os.remove(p)
with open(os.path.join(root, 'private/var/tmp/bgmtl.defer'), 'w') as f:
    f.write('BGMTL_DEFER_SCHEDULED=1 for posters (bgmtl-env.dylib)\n')

print('compositor staged: backboardd + Mercury env, bgmtl-bb.arm written, bgmtl.defer written')
