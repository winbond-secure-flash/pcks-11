#!/usr/bin/env python3
"""ft10: PKCS#11 write + read round-trip, then tee-storage cross-check"""
import argparse, serial, time, sys, re

PORT     = '/dev/ttyUSB0'
BAUD     = 921600
MOD      = '/usr/lib/libckteec.so.0'
USER_PIN = '87654321'
SO_PIN   = '12345678'
SLOT     = '0'
TAG      = 'XDONE99'
PAYLOAD  = 'W77Q-PKCS11-XVAL-SH001'

def log(msg): print(msg, flush=True)

ap = argparse.ArgumentParser(description='PKCS#11 write + read round-trip, then tee-storage cross-check')
ap.add_argument('--port', default=PORT, help=f'Serial port (default: {PORT})')
ap.parse_args()  # just for --help; script uses fixed port

log("Opening port...")
s = None
for i in range(60):
    try:
        s = serial.Serial(PORT, BAUD, timeout=0.1, exclusive=False)
        log(f"Port grabbed (attempt {i+1})")
        break
    except serial.SerialException:
        time.sleep(0.5)
if s is None:
    log("ERROR: port busy after 30s"); sys.exit(1)

def drain(t=0.3):
    time.sleep(t)
    s.reset_input_buffer()

def read_until(marker, timeout=30.0):
    buf = b''
    t0 = time.time()
    while time.time() - t0 < timeout:
        chunk = s.read(256)
        if chunk: buf += chunk
        if marker.encode() in buf:
            return buf.decode(errors='replace')
    return buf.decode(errors='replace')

def run(cmd, timeout=30):
    """Run cmd, return clean stdout (echo disabled on board)."""
    s.write(f'{cmd}\r'.encode()); time.sleep(0.05)
    s.write(f'echo {TAG}\r'.encode())
    raw = read_until(TAG, timeout)
    read_until('\n', 2)
    out = raw.split(TAG)[0].strip('\r\n ')
    return out

# wake board
s.write(b'\r'); time.sleep(0.4); s.write(b'\r')
read_until('# ', 10)
drain()

s.write(b'stty -echo\r'); time.sleep(0.3)
log("Shell ready (echo off)")

# restart tee-supplicant
log("\n--- tee-supplicant restart ---")
run('systemctl restart tee-supplicant; sleep 3', 20)

# ── Fix user PIN via SO-PIN ───────────────────────────────────────────────────
log("\n=== Reset user PIN with SO-PIN ===")
out = run(f'pkcs11-tool --module {MOD} --slot {SLOT} --so-pin {SO_PIN} '
          f'--init-pin --pin {USER_PIN} 2>&1', 30)
log(out)

# ── Baseline: list objects ────────────────────────────────────────────────────
log("\n=== BASELINE: PKCS#11 objects ===")
out = run(f'pkcs11-tool --module {MOD} --slot {SLOT} -p {USER_PIN} -O 2>&1', 20)
log(out)

log("\n=== BASELINE: tee-storage info ===")
info0 = run('tee-storage info 2>&1', 15)
log(info0)
m = re.search(r'(\d+)\s+object', info0)
count0 = int(m.group(1)) if m else -1
log(f"  tee-storage objects: {count0}")

# ── Delete stale test object if present ──────────────────────────────────────
run(f'pkcs11-tool --module {MOD} --slot {SLOT} -p {USER_PIN} '
    '--delete-object --type data --label flash-record 2>/dev/null; true', 20)

# ── PKCS#11 write ─────────────────────────────────────────────────────────────
log("\n=== PKCS#11 write data object ===")
out = run(f"echo -n '{PAYLOAD}' > /tmp/pkcs_payload.bin; "
          f"pkcs11-tool --module {MOD} --slot {SLOT} -p {USER_PIN} "
          f"--write-object /tmp/pkcs_payload.bin --type data "
          f"--label flash-record --id 01 2>&1", 30)
log(out)
write_ok = 'created' in out.lower() or 'Data object' in out

# ── PKCS#11 read-back ─────────────────────────────────────────────────────────
log("\n=== PKCS#11 read-back ===")
out = run(f'pkcs11-tool --module {MOD} --slot {SLOT} -p {USER_PIN} '
          f'--read-object --type data --label flash-record '
          f'--output-file /tmp/pkcs_readback.bin 2>&1; '
          f'echo "readback_hex:$(xxd -p /tmp/pkcs_readback.bin 2>/dev/null)"', 30)
log(out)
m = re.search(r'readback_hex:([0-9a-f]+)', out)
if m:
    hex_bytes = bytes.fromhex(m.group(1))
    log(f"  Read back: {hex_bytes!r}")
    log(f"  Match: {hex_bytes.decode(errors='replace') == PAYLOAD}")

# ── AFTER: tee-storage info ───────────────────────────────────────────────────
log("\n=== AFTER: tee-storage info ===")
info1 = run('tee-storage info 2>&1', 15)
log(info1)
m = re.search(r'(\d+)\s+object', info1)
count1 = int(m.group(1)) if m else -1
log(f"  tee-storage objects: {count1}")

# ── Probe known PKCS#11 sector address ───────────────────────────────────────
log("\n=== Probing PKCS#11 meta sector 0xaa00 ===")
out = run('tee-storage read-sector 0xaa00 2>&1', 20)
log(out)

# ── Summary ────────────────────────────────────────────────────────────────────
log("\n" + "="*60)
log("SUMMARY")
log(f"  PKCS#11 write:   {'PASS' if write_ok else 'FAIL'}")
if m:
    matched = hex_bytes.decode(errors='replace') == PAYLOAD
    log(f"  PKCS#11 readback:{'PASS' if matched else 'FAIL'} ({hex_bytes.decode(errors='replace')!r})")
log(f"  tee-storage before: {count0} objects")
log(f"  tee-storage after:  {count1} objects  (PKCS#11 in separate TA namespace — count unchanged is expected)")
log("="*60)
