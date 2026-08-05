#!/usr/bin/env python3
"""ft18.py — w77q-dump before/after pkcs11-demo (RSA/EC/AES/RNG tests).

Runs pkcs11-demo (full PKCS#11 lifecycle: token init, RSA-2048 keygen +
sign/verify, EC P-256 keygen + ECDSA sign/verify, AES-256-CBC enc/dec,
RNG), then shows the full LUT delta and raw flash dump of changed entries.
"""
import argparse, serial, time, re

PORT = "/dev/ttyUSB0"
BAUD = 921600
TAG  = "ZZDONE66"

ap = argparse.ArgumentParser(description='W77q-dump before/after pkcs11-demo (RSA/EC/AES/RNG tests)')
ap.add_argument('--port', default=PORT, help=f'Serial port (default: {PORT})')
ap.parse_args()  # just for --help; script uses fixed port

# ── serial helpers ──────────────────────────────────────────────────────────

s = serial.Serial(PORT, BAUD, timeout=0.1, exclusive=False)

def read_until(m, timeout=30):
    buf = b""
    t0 = time.time()
    while time.time()-t0 < timeout:
        buf += s.read(4096)
        if m.encode() in buf: return buf.decode(errors="replace")
    return buf.decode(errors="replace")

def run(cmd, timeout=60):
    s.write(f"{cmd}\r".encode()); time.sleep(0.05)
    s.write(f"echo {TAG}\r".encode())
    raw = read_until(TAG, timeout)
    read_until("\n", 2)
    return raw.split(TAG)[0].strip("\r\n ")

def log(m=""): print(m, flush=True)

def parse_lut(text):
    out = {}
    for line in text.splitlines():
        m = re.match(
            r'\[(\d+)\]\s+ta_uuid=(\S+)\s+flash_off=(\S+)'
            r'\s+data_size=\s*(\d+)\s+obj_id=(.*)', line.strip())
        if m:
            out[m.group(5).strip()] = {
                'uuid': m.group(2),
                'off':  m.group(3),
                'size': int(m.group(4)),
            }
    return out

# ── main ────────────────────────────────────────────────────────────────────

s.write(b"\r"); time.sleep(0.3)
read_until("# ", 8); s.reset_input_buffer()
s.write(b"stty -echo\r"); time.sleep(0.2)
log("Shell ready.")

# STEP 1 — baseline dump
log("\n════════════════════════════════════════")
log("  STEP 1: w77q-dump list-all  (BEFORE)")
log("════════════════════════════════════════")
before_raw = run("w77q-dump list-all 2>&1", 20)
log(before_raw)
before = parse_lut(before_raw)

# STEP 2 — pkcs11-demo
log("\n════════════════════════════════════════")
log("  STEP 2: pkcs11-demo  (RSA/EC/AES/RNG)")
log("════════════════════════════════════════")
t0 = time.time()
demo_out = run("pkcs11-demo 2>&1", 90)
log(demo_out)
log(f"\n  pkcs11-demo completed in {time.time()-t0:.0f}s")

# STEP 3 — after dump
log("\n════════════════════════════════════════")
log("  STEP 3: w77q-dump list-all  (AFTER)")
log("════════════════════════════════════════")
after_raw = run("w77q-dump list-all 2>&1", 20)
log(after_raw)
after = parse_lut(after_raw)

# STEP 4 — delta
log("\n════════════════════════════════════════")
log("  STEP 4: DELTA")
log("════════════════════════════════════════")
new_keys     = [k for k in after  if k not in before]
moved_keys   = [k for k in after  if k in before and after[k]['off'] != before[k]['off']]
same_keys    = [k for k in after  if k in before and after[k]['off'] == before[k]['off']]
removed_keys = [k for k in before if k not in after]

for k in same_keys:
    log(f"  ·  SAME    {k:40s}  {after[k]['off']}  size={after[k]['size']}")
for k in moved_keys:
    b_off = int(before[k]['off'], 16)
    a_off = int(after[k]['off'],  16)
    log(f"  ▶  MOVED   {k:40s}  {before[k]['off']} → {after[k]['off']}"
        f"  (+0x{a_off-b_off:x})")
for k in new_keys:
    log(f"  ★  NEW     {k:40s}  {after[k]['off']}  size={after[k]['size']}")
for k in removed_keys:
    log(f"  ✗  GONE    {k:40s}  was {before[k]['off']}")

log(f"\n  Before: {len(before)} | After: {len(after)} | "
    f"Same:{len(same_keys)} Moved:{len(moved_keys)} "
    f"New:{len(new_keys)} Removed:{len(removed_keys)}")

# STEP 5 — raw dump of moved/new token.db entries
interesting = [(k, after[k]) for k in moved_keys + new_keys if 'token.db' in k]
for k, e in interesting[:3]:
    sz = min(e['size'], 128)
    log(f"\n════════════════════════════════════════")
    log(f"  STEP 5: read-raw {e['off']} {sz}  ← {k}")
    log(f"════════════════════════════════════════")
    log(run(f"w77q-dump read-raw {e['off']} {sz} 2>&1", 20))

s.write(b"stty echo\r"); time.sleep(0.1)
s.close()
log("\nDone.")
