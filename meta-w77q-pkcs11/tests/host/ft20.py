#!/usr/bin/env python3
"""ft20.py — Full flash state compare: LUT + raw content before/after pkcs11-demo.

Runs w77q-dump list-all, dumps raw bytes of every LUT entry, runs pkcs11-demo,
then dumps again and shows NEW/MOVED/SAME/GONE entries with per-entry hex diffs.
"""
import argparse, serial, time, re, sys

PORT = "/dev/ttyUSB0"
BAUD = 921600
TAG  = "ZZDONE_FT20"

ap = argparse.ArgumentParser(
    description='Full W77Q flash compare before/after pkcs11-demo (RSA/EC/AES/RNG)')
ap.add_argument('--port', default=PORT, help=f'Serial port (default: {PORT})')
args = ap.parse_args()
PORT = args.port

s = serial.Serial(PORT, BAUD, timeout=0.1, exclusive=False)


def read_until(m, timeout=30):
    buf = b""
    t0 = time.time()
    while time.time() - t0 < timeout:
        buf += s.read(4096)
        if m.encode() in buf:
            return buf.decode(errors="replace")
    return buf.decode(errors="replace")


def run(cmd, timeout=60):
    s.write(f"{cmd}\r".encode()); time.sleep(0.05)
    s.write(f"echo {TAG}\r".encode())
    raw = read_until(TAG, timeout)
    read_until("\n", 2)
    return raw.split(TAG)[0].strip("\r\n ")


def log(m=""): print(m, flush=True)


def parse_lut(text):
    """Return dict keyed by obj_id with uuid/off/size."""
    out = {}
    for line in text.splitlines():
        m = re.match(
            r'\[(\d+)\]\s+ta_uuid=(\S+)\s+flash_off=(\S+)'
            r'\s+data_size=\s*(\d+)\s+obj_id=(.*)', line.strip())
        if m:
            obj_id = m.group(5).strip()
            out[obj_id] = {
                'uuid': m.group(2),
                'off':  m.group(3),
                'size': int(m.group(4)),
            }
    return out


def read_raw_bytes(off, size):
    """Read raw bytes from W77Q flash; return flat list of hex byte strings."""
    sz = min(max(size, 16), 256)
    out = run(f"w77q-dump read-raw {off} {sz} 2>&1", 15)
    hexbytes = []
    for line in out.splitlines():
        m = re.match(r'\s*[0-9a-fA-F]+:\s+((?:[0-9a-fA-F]{2}\s+)+)', line)
        if m:
            hexbytes.extend(m.group(1).split())
    return hexbytes


def hex_diff(label_before, label_after, b_bytes, a_bytes):
    """Print hex diff of two byte lists (16 bytes per row)."""
    changed = False
    for i in range(0, max(len(b_bytes), len(a_bytes)), 16):
        bl = b_bytes[i:i+16]
        al = a_bytes[i:i+16]
        if bl != al:
            if not changed:
                changed = True
            log(f"      [{i:04x}] BEFORE: {' '.join(bl)}")
            log(f"      [{i:04x}] AFTER:  {' '.join(al)}")
    if not changed:
        log("      (no byte-level changes)")


# ── main ─────────────────────────────────────────────────────────────

s.write(b"\r"); time.sleep(0.3)
read_until("# ", 8); s.reset_input_buffer()
s.write(b"stty -echo\r"); time.sleep(0.2)
log("Shell ready.")

# ── STEP 1: baseline LUT + raw dump ──────────────────────────────────
log("\n════════════════════════════════════════════════════")
log("  FT20  STEP 1 — w77q-dump list-all  (BEFORE)")
log("════════════════════════════════════════════════════")
before_lut_raw = run("w77q-dump list-all 2>&1", 20)
log(before_lut_raw)
before = parse_lut(before_lut_raw)
log(f"\n  {len(before)} entries — reading raw content…")

before_hex = {}
for obj_id, e in before.items():
    before_hex[obj_id] = read_raw_bytes(e['off'], e['size'])
    log(f"    {e['off']}  {obj_id[:40]}  ({len(before_hex[obj_id])} bytes)")

# ── STEP 2: pkcs11-demo ───────────────────────────────────────────────
log("\n════════════════════════════════════════════════════")
log("  FT20  STEP 2 — pkcs11-demo  (RSA/EC/AES/RNG)")
log("════════════════════════════════════════════════════")
t0 = time.time()
demo_out = run('pkcs11-demo 2>run('pkcs11-demo 2>&1; echo "RC=$?"', 90)1; echo "RC=$?"', 150)
log(demo_out)
demo_rc_m = re.search(r'RC=(\d+)', demo_out)
demo_rc = int(demo_rc_m.group(1)) if demo_rc_m else -1
log(f"\n  pkcs11-demo completed in {time.time()-t0:.0f}s  RC={demo_rc}")

# ── STEP 3: after LUT + raw dump ──────────────────────────────────────
log("\n════════════════════════════════════════════════════")
log("  FT20  STEP 3 — w77q-dump list-all  (AFTER)")
log("════════════════════════════════════════════════════")
after_lut_raw = run("w77q-dump list-all 2>&1", 20)
log(after_lut_raw)
after = parse_lut(after_lut_raw)
log(f"\n  {len(after)} entries — reading raw content…")

after_hex = {}
for obj_id, e in after.items():
    after_hex[obj_id] = read_raw_bytes(e['off'], e['size'])
    log(f"    {e['off']}  {obj_id[:40]}  ({len(after_hex[obj_id])} bytes)")

# ── STEP 4: LUT delta ──────────────────────────────────────────────────
log("\n════════════════════════════════════════════════════")
log("  FT20  STEP 4 — LUT DELTA")
log("════════════════════════════════════════════════════")

new_keys   = [k for k in after  if k not in before]
gone_keys  = [k for k in before if k not in after]
moved_keys = [k for k in after  if k in before and after[k]['off'] != before[k]['off']]
same_keys  = [k for k in after  if k in before and after[k]['off'] == before[k]['off']]
cnt_diff   = [k for k in same_keys if after_hex.get(k) != before_hex.get(k)]

for k in same_keys:
    note = "  ← content changed" if k in cnt_diff else ""
    log(f"  ·  SAME   {k:40s}  {after[k]['off']}  sz={after[k]['size']}{note}")
for k in moved_keys:
    b_off = int(before[k]['off'], 16)
    a_off = int(after[k]['off'],  16)
    log(f"  ▶  MOVED  {k:40s}  {before[k]['off']} → {after[k]['off']}"
        f"  (+0x{a_off-b_off:x})")
for k in new_keys:
    log(f"  ★  NEW    {k:40s}  {after[k]['off']}  sz={after[k]['size']}")
for k in gone_keys:
    log(f"  ✗  GONE   {k:40s}  was {before[k]['off']}")

log(f"\n  Before={len(before)}  After={len(after)} | "
    f"Same={len(same_keys)} Moved={len(moved_keys)} "
    f"New={len(new_keys)} Gone={len(gone_keys)}")

# ── STEP 5: hex diff for moved/new token.db entries ───────────────────
interesting = [(k, after[k]) for k in (moved_keys + new_keys) if 'token.db' in k]
if interesting:
    log("\n════════════════════════════════════════════════════")
    log("  FT20  STEP 5 — Raw hex diff for token.db entries")
    log("════════════════════════════════════════════════════")
    for k, e in interesting[:4]:
        log(f"\n  ── {k}  {e['off']}  sz={e['size']} ──")
        b_h = before_hex.get(k, [])
        a_h = after_hex.get(k, [])
        if b_h and a_h:
            hex_diff("BEFORE", "AFTER", b_h, a_h)
        else:
            log(f"    {' '.join(a_h[:32])}{'…' if len(a_h)>32 else ''}")

# ── result ─────────────────────────────────────────────────────────────
log("\n════════════════════════════════════════════════════")
if demo_rc == 0:
    log("  FT20: PASS")
    rc = 0
else:
    log(f"  FT20: FAIL  (pkcs11-demo RC={demo_rc})")
    rc = 1
log("════════════════════════════════════════════════════")

s.write(b"stty echo\r"); time.sleep(0.1)
s.close()
sys.exit(rc)
