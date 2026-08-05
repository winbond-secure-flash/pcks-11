#!/usr/bin/env python3
"""ft12: write via PKCS#11, read raw flash sector via tee-storage (cross-read)"""
import argparse, serial, time, sys, re

PORT     = '/dev/ttyUSB0'
BAUD     = 921600
MOD      = '/usr/lib/libckteec.so.0'
USER_PIN = '87654321'
SO_PIN   = '12345678'
SLOT     = '1'
TAG      = 'XDONE12'
PAYLOAD  = 'W77Q-PKCS11-XVAL-SH001'

def log(msg): print(msg, flush=True)

ap = argparse.ArgumentParser(description='Write via PKCS#11, read raw flash sector via tee-storage (cross-read)')
ap.add_argument('--port', default=PORT, help=f'Serial port (default: {PORT})')
ap.parse_args()  # just for --help; script uses fixed port

s = None
for i in range(60):
    try:
        s = serial.Serial(PORT, BAUD, timeout=0.2, exclusive=False)
        log(f"Port opened (attempt {i+1})")
        break
    except serial.SerialException:
        time.sleep(0.5)
if s is None:
    log("ERROR: port busy"); sys.exit(1)

def read_until(marker, timeout=30.0):
    buf = b''
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            chunk = s.read(256)
        except serial.SerialException:
            return buf.decode(errors='replace')
        if chunk: buf += chunk
        if marker.encode() in buf:
            return buf.decode(errors='replace')
    return buf.decode(errors='replace')

def run(cmd, timeout=30):
    s.write(f'{cmd}\r'.encode()); time.sleep(0.05)
    s.write(f'echo {TAG}\r'.encode())
    raw = read_until(TAG, timeout)
    read_until('\n', 2)
    return raw.split(TAG)[0].strip('\r\n ')

# get shell
s.write(b'\r'); time.sleep(0.4); s.write(b'\r')
read_until('# ', 10)
s.reset_input_buffer()
s.write(b'stty -echo\r'); time.sleep(0.3)
log("Shell ready")

# tee-supplicant check
out = run('systemctl is-active tee-supplicant 2>&1', 10)
log(f"tee-supplicant: {out.strip()}")
if 'active' not in out:
    run('systemctl start tee-supplicant; sleep 2', 15)

# ── Step 1: PKCS#11 write ──────────────────────────────────────────────────────
log("\n=== [1] PKCS#11 write ===")
run(f'pkcs11-tool --module {MOD} --slot {SLOT} --so-pin {SO_PIN} --init-pin --pin {USER_PIN} 2>&1', 20)
run(f'pkcs11-tool --module {MOD} --slot {SLOT} -p {USER_PIN} '
    '--delete-object --type data --label flash-record 2>/dev/null; true', 20)
out = run(f"printf '%s' '{PAYLOAD}' > /tmp/p.bin; "
          f"pkcs11-tool --module {MOD} --slot {SLOT} -p {USER_PIN} "
          f"--write-object /tmp/p.bin --type data --label flash-record --id 01 2>&1", 60)
log(out)
write_ok = 'data object' in out.lower() or 'created' in out.lower()

# ── Step 2: scan known PKCS#11 sector addresses ───────────────────────────────
log("\n=== [2] Scan PKCS#11 sectors via tee-storage read-sector ===")
# pkcs11_ta typically uses sectors starting at 0xaa00 (meta) + data sectors
# Try a range: 0xaa00 .. 0xaf00 (every 0x100 = 256 bytes, typical w77q_fs granularity)
found = []
for addr in [0xaa00, 0xaa40, 0xaa80, 0xaac0, 0xab00, 0xab40, 0xab80, 0xac00,
             0xad00, 0xae00, 0xaf00, 0xb000]:
    out = run(f'tee-storage read-sector 0x{addr:04x} 2>&1', 15)
    if '[OK]' in out:
        stored = re.search(r'(\d+) bytes stored', out)
        n = int(stored.group(1)) if stored else 0
        log(f"  0x{addr:04x}: {n} bytes stored  <-- FOUND")
        # show first 4 lines of hexdump
        lines = [l for l in out.splitlines() if l.startswith('0')]
        for l in lines[:4]:
            log(f"    {l}")
        found.append((addr, n))
    else:
        log(f"  0x{addr:04x}: not found")

# ── Step 3: PKCS#11 read-back via pkcs11-tool ─────────────────────────────────
log("\n=== [3] PKCS#11 read-back via pkcs11-tool ===")
out = run(f'pkcs11-tool --module {MOD} --slot {SLOT} -p {USER_PIN} '
          f'--read-object --type data --label flash-record '
          f'--output-file /tmp/rb.bin; echo "rc=$?" 2>&1', 60)
log(out)
out = run('od -A x -t x1z /tmp/rb.bin 2>/dev/null || echo NO_FILE', 10)
log(out)
read_ok = PAYLOAD[:8] in out

# ── Cleanup ───────────────────────────────────────────────────────────────────
run(f'pkcs11-tool --module {MOD} --slot {SLOT} -p {USER_PIN} '
    '--delete-object --type data --label flash-record 2>/dev/null; true', 20)

# ── Summary ───────────────────────────────────────────────────────────────────
log("\n" + "="*60)
log("SUMMARY")
log(f"  PKCS#11 write:              {'PASS' if write_ok else 'FAIL'}")
log(f"  Sectors visible in w77q_fs: {[f'0x{a:04x}({n}B)' for a,n in found]}")
log(f"  PKCS#11 read-back:          {'PASS' if read_ok else 'FAIL'}")
log("="*60)
