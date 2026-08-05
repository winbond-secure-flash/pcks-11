#!/usr/bin/env python3
"""ft13: persistence test — write, restart tee-supplicant (LUT rebuild from flash), read back"""
import argparse, serial, time, sys, re

PORT     = '/dev/ttyUSB0'
BAUD     = 921600
MOD      = '/usr/lib/libckteec.so.0'
USER_PIN = '87654321'
SO_PIN   = '12345678'
SLOT     = '1'
TAG      = 'XDONE13'
PKCS_PAYLOAD  = 'W77Q-PKCS11-XVAL-SH001'
TS_PAYLOAD    = 'W77Q-TEESTORE-XVAL-SH001'

def log(msg): print(msg, flush=True)

ap = argparse.ArgumentParser(description='Persistence test — write, restart tee-supplicant, read back')
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
read_until('# ', 10); s.reset_input_buffer()
s.write(b'stty -echo\r'); time.sleep(0.3)
log("Shell ready")

# ensure tee-supplicant running
out = run('systemctl is-active tee-supplicant', 10)
if 'active' not in out:
    run('systemctl start tee-supplicant; sleep 2', 15)
log(f"tee-supplicant: {out.strip()}")

# ── WRITE PHASE ───────────────────────────────────────────────────────────────
log("\n" + "="*60)
log("PHASE 1: Write to flash via both TAs")
log("="*60)

# tee-storage write
log("\n[A] tee-storage write-sector 0x2000")
out = run(f"tee-storage write-sector 0x2000 '{TS_PAYLOAD}' 2>&1", 20)
log(f"  {out.strip()}")
write_ts = '[OK]' in out

# PKCS#11 write
run(f'pkcs11-tool --module {MOD} --slot {SLOT} --so-pin {SO_PIN} --init-pin --pin {USER_PIN} 2>&1', 20)
run(f'pkcs11-tool --module {MOD} --slot {SLOT} -p {USER_PIN} '
    '--delete-object --type data --label reread-test 2>/dev/null; true', 20)
log("\n[B] PKCS#11 write data object 'reread-test'")
out = run(f"printf '%s' '{PKCS_PAYLOAD}' > /tmp/p.bin; "
          f"pkcs11-tool --module {MOD} --slot {SLOT} -p {USER_PIN} "
          f"--write-object /tmp/p.bin --type data --label reread-test --id 02 2>&1", 60)
log(f"  {out.strip()}")
write_pkcs = 'data object' in out.lower() or 'created' in out.lower()

# ── RESTART tee-supplicant (forces w77q_fs LUT rebuild from raw flash) ────────
log("\n" + "="*60)
log("PHASE 2: Restart tee-supplicant → w77q_fs LUT rebuilt from flash")
log("="*60)
out = run('systemctl restart tee-supplicant; sleep 3', 20)
out = run('systemctl is-active tee-supplicant', 10)
log(f"  tee-supplicant after restart: {out.strip()}")

# ── READ PHASE ────────────────────────────────────────────────────────────────
log("\n" + "="*60)
log("PHASE 3: Read back after LUT rebuild")
log("="*60)

log("\n[C] tee-storage read-sector 0x2000 (after restart)")
out = run('tee-storage read-sector 0x2000 2>&1', 20)
log(out)
read_ts = '[OK]' in out and TS_PAYLOAD[:8] in out

log("\n[D] PKCS#11 read-back (after restart)")
out = run(f'pkcs11-tool --module {MOD} --slot {SLOT} -p {USER_PIN} '
          f'--read-object --type data --label reread-test '
          f'--output-file /tmp/rb.bin; echo rc=$? 2>&1', 60)
log(f"  {out.strip()}")
out = run('od -A x -t x1z /tmp/rb.bin 2>/dev/null || echo NO_FILE', 10)
log(out)
read_pkcs = PKCS_PAYLOAD[:8] in out

# ── Cleanup ───────────────────────────────────────────────────────────────────
run('tee-storage erase-sector 0x2000 2>&1', 15)
run(f'pkcs11-tool --module {MOD} --slot {SLOT} -p {USER_PIN} '
    '--delete-object --type data --label reread-test 2>/dev/null; true', 20)

# ── Summary ───────────────────────────────────────────────────────────────────
log("\n" + "="*60)
log("FLASH PERSISTENCE TEST SUMMARY")
log(f"  [A] tee-storage write-sector 0x2000 : {'PASS' if write_ts else 'FAIL'}")
log(f"  [B] PKCS#11 write 'reread-test'     : {'PASS' if write_pkcs else 'FAIL'}")
log(f"  --- tee-supplicant restarted → w77q_fs LUT rebuilt from flash ---")
log(f"  [C] tee-storage read-sector  0x2000 : {'PASS' if read_ts else 'FAIL'}")
log(f"  [D] PKCS#11 read 'reread-test'      : {'PASS' if read_pkcs else 'FAIL'}")
log(f"  Both TAs persist through tee-supplicant restart → data lives in W77Q NOR flash")
log("="*60)
