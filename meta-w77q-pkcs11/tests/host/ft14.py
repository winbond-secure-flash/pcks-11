#!/usr/bin/env python3
"""ft14: read ALL flash data — tee-storage objects + raw SPI/MTD if accessible"""
import argparse, serial, time, sys, re

PORT = '/dev/ttyUSB0'
BAUD = 921600
TAG  = 'XDONE14'

def log(msg): print(msg, flush=True)

ap = argparse.ArgumentParser(description='Read ALL flash data — tee-storage objects + raw SPI/MTD if accessible')
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

# ensure tee-supplicant
out = run('systemctl is-active tee-supplicant', 10)
if 'active' not in out:
    run('systemctl start tee-supplicant; sleep 2', 15)

# ── Check raw flash access ─────────────────────────────────────────────────────
log("\n=== Raw flash devices ===")
out = run('ls /dev/mtd* /dev/spi* /dev/w77q* 2>&1', 10)
log(out)
out = run('cat /proc/mtd 2>/dev/null || echo NO_MTD', 10)
log(out)

# ── tee-storage: list all named objects ───────────────────────────────────────
log("\n=== tee-storage: list named objects ===")
out = run('tee-storage list 2>&1', 20)
log(out)

log("\n=== tee-storage: info ===")
out = run('tee-storage info 2>&1', 15)
log(out)
obj_ids = re.findall(r'\[obj\]\s+(\S+)', out) if '[obj]' in out else []

# ── tee-storage: read all named objects ───────────────────────────────────────
if obj_ids:
    log(f"\n=== Reading {len(obj_ids)} named objects ===")
    for oid in obj_ids:
        log(f"\n[obj] {oid}:")
        out = run(f'tee-storage read {oid} 2>&1', 15)
        log(out)

# ── tee-storage: scan sector address space ────────────────────────────────────
# Scan common addresses used by tee_storage_ta sector commands
log("\n=== Scan sector address space (0x0000 - 0xffff, step 0x1000) ===")
occupied = []
for addr in range(0x0000, 0x10000, 0x1000):
    out = run(f'tee-storage read-sector 0x{addr:04x} 2>&1', 10)
    if '[OK]' in out:
        m = re.search(r'(\d+) bytes stored', out)
        n = int(m.group(1)) if m else 0
        if n > 0:
            log(f"\n  0x{addr:04x}: {n} bytes stored")
            lines = [l for l in out.splitlines() if l.startswith('0')]
            for l in lines[:6]:
                log(f"    {l}")
            occupied.append(addr)
        else:
            log(f"  0x{addr:04x}: empty (erased)")

if not occupied:
    log("  No occupied sectors in 0x0000-0xffff")

# ── PKCS#11: list all slots and objects ───────────────────────────────────────
MOD = '/usr/lib/libckteec.so.0'
PIN = '87654321'
log("\n=== PKCS#11: all slots and objects ===")
out = run(f'pkcs11-tool --module {MOD} -T 2>&1', 15)
log(out)

slots = re.findall(r'Slot (\d+)', out)
for slot in slots:
    log(f"\n  Slot {slot} objects:")
    obj_out = run(f'pkcs11-tool --module {MOD} --slot {slot} -p {PIN} -O 2>&1', 30)
    log(obj_out)

log("\n" + "="*60)
log("DONE")
log("="*60)
