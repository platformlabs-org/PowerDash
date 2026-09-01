#!/usr/bin/env python3
"""Verify that the driver embedded in PowerMode.exe... in PowerDash.exe
matches the .sys on disk (e.g. after manually signing it).

Usage:  python verify_embed.py [PowerDash.exe] [PowerDash.sys]
"""
import hashlib
import sys

exe_path = sys.argv[1] if len(sys.argv) > 1 else "x64/Release/PowerDash.exe"
sys_path = sys.argv[2] if len(sys.argv) > 2 else "x64/Release/PowerDashSYS/PowerDash.sys"

exe = open(exe_path, "rb").read()
sysf = open(sys_path, "rb").read()

h_sys = hashlib.sha256(sysf).hexdigest()
idx = exe.find(sysf)

print(f"exe        : {exe_path} ({len(exe)} bytes)")
print(f"sys        : {sys_path} ({len(sysf)} bytes, sha256 {h_sys})")
if idx < 0:
    print("RESULT     : sys NOT embedded verbatim (stale build? run build-exe.cmd)")
    sys.exit(1)
h_emb = hashlib.sha256(exe[idx:idx + len(sysf)]).hexdigest()
print(f"embedded at: file offset {idx:#x}, sha256 {h_emb}")
print(f"RESULT     : {'MATCH - embedded driver is exactly the on-disk file' if h_emb == h_sys else 'MISMATCH'}")
sys.exit(0 if h_emb == h_sys else 1)
