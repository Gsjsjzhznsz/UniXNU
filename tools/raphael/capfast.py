#!/usr/bin/env python3
"""Fast capture of the iOS kernel UDP log (port 5140).

The phone's log thread starts with the last 64 KiB of the console ring and
drops any chunk it cannot send (no ARP), so the host address must be on the
gadget netdev as soon as it appears. Polls every 100 ms and configures the
link with one sudo call.

Kept in the repo (tools/raphael/) because /tmp is wiped on host reboot.
Needs the sudo password in /tmp/.sudo_pw.

Usage: sudo-pw file present, then
    python3 -u tools/raphael/capfast.py /tmp/boot.log
"""
import os, socket, subprocess, sys, time

OUT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/gpu-capture.log"
PW = open("/tmp/.sudo_pw").read()
CIDR = "192.168.7.1/24"

t0 = time.time()
out = open(OUT, "a", buffering=1)
def log(msg):
    out.write("=== host %s (+%.2fs): %s\n" % (time.strftime("%H:%M:%S"), time.time() - t0, msg))

def sudo(*args):
    return subprocess.run(["sudo", "-S", "-p", ""] + list(args), input=PW, capture_output=True, text=True)

def gadget_ifaces():
    found = []
    for n in os.listdir("/sys/class/net"):
        try:
            if "cdc_ether" in os.path.realpath("/sys/class/net/%s/device/driver" % n):
                found.append(n)
        except OSError:
            pass
    return found

def has_addr(n):
    r = subprocess.run(["ip", "-br", "addr", "show", n], capture_output=True, text=True)
    return CIDR.split("/")[0] + "/" in r.stdout

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("0.0.0.0", 5140))
s.settimeout(0.1)
log("fast watch: udp/5140 -> %s" % OUT)

pending = {}
while True:
    now = time.time()
    for n in gadget_ifaces():
        if has_addr(n):
            continue
        last = pending.get(n, 0)
        if now - last < 1.0:
            continue
        pending[n] = now
        r = sudo("sh", "-c", "ip link set %s up; ip addr replace %s dev %s; ip -br addr show %s"
                 % (n, CIDR, n, n))
        log("configured %s: %s" % (n, (r.stdout or r.stderr).strip().replace("\n", " | ")))
    try:
        while True:
            d, peer = s.recvfrom(65535)
            out.write("%s %s\n" % (time.strftime("%H:%M:%S"), d.decode(errors="replace")))
    except socket.timeout:
        pass
