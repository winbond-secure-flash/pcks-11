#!/usr/bin/env python3
"""ft17.py — w77q-dump before/after 25× tee-demo demo 12345678.

Runs tee-demo demo <password> N times, capturing counter/boot_count
per iteration, then shows the full LUT delta and raw flash comparison
of the first moved object (BEFORE vs AFTER address).
"""
import argparse, serial, time, re

PORT     = "/dev/ttyUSB0"
BAUD     = 921600
PASSWORD = "12345678"
RUNS     = 8
TAG      = "ZZDONE77"

ap = argparse.ArgumentParser(description='W77q-dump before/after 25x tee-demo wear test')
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

def run(cmd, timeout=30):
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

# STEP 2 — run tee-demo N times
log(f"\n════════════════════════════════════════")
log(f"  STEP 2: tee-demo demo {PASSWORD} × {RUNS}")
log(f"════════════════════════════════════════")
t0 = time.time()
for i in range(1, RUNS + 1):
    out = run(f"tee-demo demo {PASSWORD} 2>tee-demo demo {PASSWORD} 2>&1", 601", 70)
    ctrs  = re.findall(r'counter=(\d+)',    out)
    boots = re.findall(r'boot_count=(\d+)', out)
    ctr  = ctrs[-1]  if ctrs  else '?'
    boot = boots[-1] if boots else '?'
    log(f"  [{i:02d}/{RUNS}] counter={ctr}  boot_count={boot}"
        f"  ({time.time()-t0:.0f}s elapsed)")
log(f"\n  Done — {RUNS} runs in {time.time()-t0:.0f}s")

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
    log(f"  ·  SAME    {k:40s}  {after[k]['off']}")
for k in moved_keys:
    b_off = int(before[k]['off'], 16)
    a_off = int(after[k]['off'],  16)
    log(f"  ▶  MOVED   {k:40s}  {before[k]['off']} → {after[k]['off']}"
        f"  (+0x{a_off-b_off:x} = {a_off-b_off} bytes)")
for k in new_keys:
    log(f"  ★  NEW     {k:40s}  {after[k]['off']}  size={after[k]['size']}")
for k in removed_keys:
    log(f"  ✗  GONE    {k:40s}  was {before[k]['off']}")

log(f"\n  Before: {len(before)} entries | After: {len(after)} entries")
log(f"  Same:{len(same_keys)}  Moved:{len(moved_keys)}"
    f"  New:{len(new_keys)}  Removed:{len(removed_keys)}")

# STEP 5 — raw flash compare of first moved demo object
if moved_keys:
    k = next((x for x in moved_keys if 'demo_00' in x), moved_keys[0])
    b_off = before[k]['off']
    a_off = after[k]['off']
    sz    = min(after[k]['size'] + 32, 96)
    log(f"\n════════════════════════════════════════")
    log(f"  STEP 5: raw flash compare — '{k}'")
    log(f"════════════════════════════════════════")
    log(f"  BEFORE @ {b_off}:")
    log(run(f"w77q-dump read-raw {b_off} {sz} 2>&1", 20))
    log(f"  AFTER  @ {a_off}:")
    log(run(f"w77q-dump read-raw {a_off} {sz} 2>&1", 20))

s.write(b"stty echo\r"); time.sleep(0.1)
s.close()
log("\nDone.")
