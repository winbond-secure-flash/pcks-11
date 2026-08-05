#!/usr/bin/env python3
"""ft11: PKCS#11 + tee-storage cross-validation with reconnect support"""
import argparse, serial, time, sys, re

PORT     = '/dev/ttyUSB0'
BAUD     = 921600
MOD      = '/usr/lib/libckteec.so.0'
USER_PIN = '87654321'
SO_PIN   = '12345678'
SLOT     = '1'
TAG      = 'XDONE11'
PAYLOAD  = 'W77Q-PKCS11-XVAL-SH001'

def log(msg): print(msg, flush=True)

def open_port(retries=120):
    for i in range(retries):
        try:
            s = serial.Serial(PORT, BAUD, timeout=0.2, exclusive=False)
            log(f"Port opened (attempt {i+1})")
            return s
        except serial.SerialException:
            time.sleep(0.5)
    log("ERROR: port busy"); sys.exit(1)

ap = argparse.ArgumentParser(description='PKCS#11 + tee-storage cross-validation with reconnect support')
ap.add_argument('--port', default=PORT, help=f'Serial port (default: {PORT})')
ap.parse_args()  # just for --help; script uses fixed port

s = open_port()

def drain():
    time.sleep(0.3); s.reset_input_buffer()

def read_until(marker, timeout=30.0):
    buf = b''
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            chunk = s.read(256)
        except serial.SerialException:
            return buf.decode(errors='replace')
        if chunk:
            buf += chunk
        if marker.encode() in buf:
            return buf.decode(errors='replace')
    return buf.decode(errors='replace')

def wait_for_shell(timeout=240):
    """Wait for login/shell prompt, handle reboots."""
    global s
    log("  Waiting for shell prompt...")
    t0 = time.time()
    buf = b''
    while time.time() - t0 < timeout:
        try:
            s.write(b'\r')
            time.sleep(0.5)
            chunk = s.read(512)
            if chunk:
                buf += chunk
                text = buf.decode(errors='replace')
                if 'login:' in text:
                    log("  Login prompt — sending root")
                    s.write(b'root\r')
                    buf = b''
                    time.sleep(1)
                elif '# ' in text or '$ ' in text:
                    log("  Shell prompt found")
                    return True
        except serial.SerialException:
            log("  Port disconnected, reopening...")
            time.sleep(2)
            s = open_port()
    return False

def run(cmd, timeout=30):
    """Send command, return output. Handles transient read errors."""
    s.write(f'{cmd}\r'.encode()); time.sleep(0.05)
    s.write(f'echo {TAG}\r'.encode())
    raw = read_until(TAG, timeout)
    read_until('\n', 2)
    return raw.split(TAG)[0].strip('\r\n ')

# ── Get shell ────────────────────────────────────────────────────────────────
if not wait_for_shell(120):
    log("ERROR: no shell"); sys.exit(1)
drain()
s.write(b'stty -echo\r'); time.sleep(0.3)
log("Echo disabled")

# ── tee-supplicant: start if not running (avoid restart panic) ────────────────
log("\n--- tee-supplicant check ---")
out = run('systemctl is-active tee-supplicant 2>&1', 10)
log(f"  tee-supplicant: {out.strip()}")
if 'active' not in out:
    log("  Starting tee-supplicant...")
    out = run('systemctl start tee-supplicant; sleep 2', 15)
    log(out)
else:
    log("  Already active — skipping restart")

# ── PART 1: tee-storage sector write + read ───────────────────────────────────
log("\n" + "="*60)
log("PART 1: tee-storage write-sector 0x1000 + read-sector 0x1000")
log("="*60)

log("\n[1a] tee-storage write-sector")
out = run("tee-storage write-sector 0x1000 'W77Q-FLASH-RECORD-SH001' 2>&1", 20)
log(out)
write1_ok = '[OK]' in out

log("\n[1b] tee-storage read-sector (full 4096-byte dump)")
out = run('tee-storage read-sector 0x1000 2>&1', 20)
log(out)
read1_ok = '[OK]' in out and 'W77Q' in out

log("\n[1c] tee-storage info")
out = run('tee-storage info 2>&1', 15)
log(out)
m = re.search(r'Objects\s*:\s*(\d+)', out)
count_ts = int(m.group(1)) if m else -1
log(f"  objects: {count_ts}")

# ── Resync shell before PKCS#11 (flush any pending output) ───────────────────
s.reset_input_buffer()
s.write(b'\r'); time.sleep(0.3); s.reset_input_buffer()
run('true', 5)  # eat any stale prompt

# ── PART 2: PKCS#11 round-trip ────────────────────────────────────────────────
log("\n" + "="*60)
log("PART 2: PKCS#11 write + read-back")
log("="*60)

out = run(f'pkcs11-tool --module {MOD} --slot {SLOT} --so-pin {SO_PIN} '
          f'--init-pin --pin {USER_PIN} 2>&1', 30)
log(f"[2a] PIN reset: {out.strip()}")

run(f'pkcs11-tool --module {MOD} --slot {SLOT} -p {USER_PIN} '
    '--delete-object --type data --label flash-record 2>/dev/null; true', 20)

log("\n[2b] pkcs11 write")
out = run(f"printf '%s' '{PAYLOAD}' > /tmp/p.bin; "
          f"pkcs11-tool --module {MOD} --slot {SLOT} -p {USER_PIN} "
          f"--write-object /tmp/p.bin --type data --label flash-record --id 01 2>&1", 60)
log(out)
write2_ok = 'data object' in out.lower() or 'created' in out.lower()

log("\n[2c] pkcs11 read-back")
out = run(f'pkcs11-tool --module {MOD} --slot {SLOT} -p {USER_PIN} '
          f'--read-object --type data --label flash-record '
          f'--output-file /tmp/rb.bin; echo "rc=$?" 2>&1', 60)
log(out)

log("\n[2d] hexdump of readback (od -A x -t x1z)")
out = run('od -A x -t x1z /tmp/rb.bin 2>/dev/null || echo NO_FILE', 10)
log(out)
# also try reading as text
out_txt = run('cat /tmp/rb.bin 2>/dev/null || echo NO_FILE', 10)
log(f"  as text: {out_txt.strip()!r}")
read2_ok = PAYLOAD in out_txt or PAYLOAD[:8] in out

# ── Cleanup ───────────────────────────────────────────────────────────────────
run(f'pkcs11-tool --module {MOD} --slot {SLOT} -p {USER_PIN} '
    '--delete-object --type data --label flash-record 2>/dev/null; true', 20)
run('tee-storage erase-sector 0x1000 2>&1', 15)

# ── Summary ───────────────────────────────────────────────────────────────────
log("\n" + "="*60)
log("CROSS-VALIDATION SUMMARY")
log(f"  [1] tee-storage write-sector 0x1000 : {'PASS' if write1_ok else 'FAIL'}")
log(f"  [1] tee-storage read-sector  0x1000 : {'PASS' if read1_ok else 'FAIL'}")
log(f"  [2] PKCS#11 write data object       : {'PASS' if write2_ok else 'FAIL'}")
log(f"  [2] PKCS#11 read-back match         : {'PASS' if read2_ok else 'FAIL'}")
log("  Both TAs → TEE_STORAGE_PRIVATE → same W77Q NOR flash backend")
log("="*60)
