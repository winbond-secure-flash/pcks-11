#!/usr/bin/env python3
"""ft16.py — PKCS11 store a data object, then show w77q-dump list-all
             before and after, pointing to the newly stored entry.
"""
import argparse, serial, time, sys, re

PORT     = "/dev/ttyUSB0"
BAUD     = 921600
PKCS_MOD = "/usr/lib/libckteec.so.0"
USER_PIN = "87654321"
SO_PIN   = "12345678"
LABEL    = "dump-test-obj"
VALUE    = "SPARROW-HAWK-DUMP-DEMO-2026"

# ── serial helpers ──────────────────────────────────────────────────────────

def log(msg="", **kw):
    print(msg, flush=True, **kw)

ap = argparse.ArgumentParser(description='PKCS#11 store a data object, show w77q-dump list-all before and after')
ap.add_argument('--port', default=PORT, help=f'Serial port (default: {PORT})')
ap.parse_args()  # just for --help; script uses fixed port

log("Opening port…")
ser = None
for i in range(60):
    try:
        ser = serial.Serial(PORT, BAUD, timeout=0.1, exclusive=False)
        log(f"  grabbed (attempt {i+1})")
        break
    except serial.SerialException:
        time.sleep(0.5)
if ser is None:
    log("ERROR: port busy after 30s"); sys.exit(1)

TAG = "ZZZDONE999"

def read_until(marker, timeout=30.0):
    buf = b""
    t0 = time.time()
    while time.time() - t0 < timeout:
        buf += ser.read(256)
        if marker.encode() in buf:
            return buf.decode(errors="replace")
    return buf.decode(errors="replace")

def run(cmd, timeout=30):
    ser.write(f"{cmd}\r".encode())
    time.sleep(0.05)
    ser.write(f"echo {TAG}\r".encode())
    raw = read_until(TAG, timeout)
    read_until("\n", 2)
    return raw.split(TAG)[0].strip("\r\n ")

# wake + shell
ser.write(b"\r"); time.sleep(0.3); ser.write(b"\r")
read_until("# ", 8)
ser.reset_input_buffer()
ser.write(b"stty -echo\r"); time.sleep(0.2)
log("Shell ready.\n")

# tee-supplicant
log("── restarting tee-supplicant ──")
run("systemctl restart tee-supplicant; sleep 2", 20)
log("  done")

# ── parse w77q-dump list-all ────────────────────────────────────────────────

def parse_dump(text):
    """Return dict {obj_id: {uuid, off, size}} from w77q-dump list-all output."""
    entries = {}
    for line in text.splitlines():
        m = re.match(
            r'\[(\d+)\]\s+ta_uuid=(\S+)\s+flash_off=(\S+)'
            r'\s+data_size=\s*(\d+)\s+obj_id=(.*)', line.strip())
        if m:
            entries[m.group(5).strip()] = {
                'idx':  int(m.group(1)),
                'uuid': m.group(2),
                'off':  m.group(3),
                'size': int(m.group(4)),
            }
    return entries

# ── STEP 1: baseline dump ───────────────────────────────────────────────────
log("════════════════════════════════════════")
log("  STEP 1: w77q-dump list-all  (BEFORE)")
log("════════════════════════════════════════")
raw_before = run("w77q-dump list-all 2>&1", 20)
log(raw_before)
before = parse_dump(raw_before)
log(f"\n  → {len(before)} entries in flash LUT")

# ── STEP 2: PKCS11 token init if needed ────────────────────────────────────
log("\n── checking PKCS#11 token ──")
tok = run(f"pkcs11-tool --module {PKCS_MOD} -T 2>&1", 15)
if "uninitialized" in tok.lower() or "no token" in tok.lower():
    log("  token uninitialized — initialising…")
    log(run(f"pkcs11-tool --module {PKCS_MOD} --init-token --label 'OP-TEE' --so-pin {SO_PIN} 2>&1", 30))
    log(run(f"pkcs11-tool --module {PKCS_MOD} --init-pin --login --login-type so --so-pin {SO_PIN} --pin {USER_PIN} 2>&1", 30))
else:
    log("  token OK")

# delete stale object with same label (ignore errors)
run(f"pkcs11-tool --module {PKCS_MOD} -p {USER_PIN} "
    f"--delete-object --type data --label {LABEL} 2>/dev/null; true", 20)

# ── STEP 3: write PKCS11 data object ───────────────────────────────────────
log("\n════════════════════════════════════════")
log("  STEP 2: pkcs11-tool write data object")
log("════════════════════════════════════════")
write_cmd = (
    f"printf '%s' '{VALUE}' | "
    f"pkcs11-tool --module {PKCS_MOD} -p {USER_PIN} "
    f"--write-object /dev/stdin --type data --label {LABEL} --id 42 2>&1"
)
out = run(write_cmd, 30)
log(out)
if "created" in out.lower() or "object" in out.lower():
    log("  ✓ data object stored")
else:
    log("  ⚠ unexpected output — continuing anyway")

time.sleep(1)   # let flash settle

# ── STEP 4: dump after ──────────────────────────────────────────────────────
log("\n════════════════════════════════════════")
log("  STEP 3: w77q-dump list-all  (AFTER)")
log("════════════════════════════════════════")
raw_after = run("w77q-dump list-all 2>&1", 20)
log(raw_after)
after = parse_dump(raw_after)
log(f"\n  → {len(after)} entries in flash LUT")

# ── STEP 5: annotate diff ──────────────────────────────────────────────────
log("\n════════════════════════════════════════")
log("  STEP 4: DELTA")
log("════════════════════════════════════════")

new_ids   = [k for k in after if k not in before]
moved_ids = [k for k in after if k in before and after[k]['off'] != before[k]['off']]
gone_ids  = [k for k in before if k not in after]

for k in sorted(after):
    if k in new_ids:
        e = after[k]
        log(f"  ▶  NEW    {k:<36s}  {e['off']}  size={e['size']}")
    elif k in moved_ids:
        log(f"  ▶  MOVED  {k:<36s}  {before[k]['off']} → {after[k]['off']}"
            f"  (+{int(after[k]['off'],16)-int(before[k]['off'],16):#x})")
    else:
        log(f"  ·  SAME   {k:<36s}  {after[k]['off']}  size={after[k]['size']}")
for k in gone_ids:
    log(f"  ✗  GONE   {k:<36s}  was {before[k]['off']}")

log(f"\n  Before: {len(before)} | After: {len(after)} |"
    f" New:{len(new_ids)}  Moved:{len(moved_ids)}  Removed:{len(gone_ids)}")

# raw dump of new/moved entries
interesting = new_ids or moved_ids
if interesting:
    k = interesting[0]
    e = after[k]
    log(f"\n── reading 128 bytes raw @ {e['off']}  ← {k} ──")
    log(run(f"w77q-dump read-raw {e['off']} 128 2>&1", 20))

log("\n════════════════════════════════════════")
log("  SUMMARY")
log("════════════════════════════════════════")
log(f"  Before: {len(before)} LUT entries")
log(f"  After:  {len(after)} LUT entries")
log(f"  Delta:  {len(after)-len(before):+d} entries")
log(f"  Value stored: '{VALUE}'")

ser.write(b"stty echo\r"); time.sleep(0.1)
ser.close()
log("\nDone.")
