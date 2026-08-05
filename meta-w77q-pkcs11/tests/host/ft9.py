#!/usr/bin/env python3
"""ft9: PKCS#11 write → find new flash sector → dump 4096 bytes"""

import argparse, serial, time, sys, re

PORT  = '/dev/ttyUSB0'
BAUD  = 921600
TOUT  = 30.0

def log(msg): print(msg, flush=True)

class Board:
    def __init__(self, port, baud):
        self.s = serial.Serial(port, baud, timeout=0.1,
                               exclusive=False)  # hold port, MM can't grab it
        time.sleep(0.2)
        self.s.reset_input_buffer()

    def send(self, cmd):
        self.s.write((cmd + '\r').encode())

    def read_until(self, marker, timeout=TOUT):
        buf = b''
        t0 = time.time()
        while time.time() - t0 < timeout:
            chunk = self.s.read(256)
            if chunk:
                buf += chunk
            if marker.encode() in buf:
                return buf.decode(errors='replace')
        raise TimeoutError(f'timeout waiting for {marker!r}, got: {buf[-200:].decode(errors="replace")}')

    def cmd(self, c, timeout=TOUT):
        tag = f'SYNC{int(time.time()*1000)%99999}'
        self.send(c + f'; echo {tag}')
        out = self.read_until(tag, timeout)
        time.sleep(0.1)   # let prompt bytes arrive then discard
        self.s.reset_input_buffer()
        return out

    def close(self):
        self.s.close()

ap = argparse.ArgumentParser(description='PKCS#11 write → find new flash sector → dump 4096 bytes')
ap.add_argument('--port', default=PORT, help=f'Serial port (default: {PORT})')
ap.parse_args()  # just for --help; script uses fixed port

b = Board(PORT, BAUD)
log("Port opened — MM cannot grab it now")

# wake up
b.send('\r')
time.sleep(0.3)
b.send('\r')
try:
    b.read_until('# ', 5)
except TimeoutError:
    pass
log("Shell ready")

# disable echo so output parsing works cleanly
b.send('stty -echo\r')
time.sleep(0.2)
b.s.reset_input_buffer()

# tee-supplicant
log("\n--- Restarting tee-supplicant ---")
b.cmd('systemctl restart tee-supplicant; sleep 2', 20)

PKCS_MOD = '/usr/lib/libckteec.so.0'
USER_PIN  = '87654321'
SO_PIN    = '12345678'

# ensure token is initialised with known PIN
log("\n--- Checking PKCS#11 token (slot 1 = yuri) ---")
tok = b.cmd(f'pkcs11-tool --module {PKCS_MOD} --slot 1 -T 2>&1')
if 'uninitialized' in tok.lower() or 'no token' in tok.lower():
    log("  initialising slot 1...")
    b.cmd(f"pkcs11-tool --module {PKCS_MOD} --slot 1 "
          f"--init-token --label yuri --so-pin {SO_PIN} 2>&1", 30)
    b.cmd(f"pkcs11-tool --module {PKCS_MOD} --slot 1 "
          f"--init-pin --login --login-type so --so-pin {SO_PIN} --pin {USER_PIN} 2>&1", 30)
else:
    log("  slot 1 token OK")

# baseline LUT
log("\n=== BASELINE w77q-dump list-all ===")
out0 = b.cmd('w77q-dump list-all 2>&1')
log(out0.split('SYNC')[0].strip())
base_entries = set(re.findall(r'flash_off=(\S+)', out0))

# delete stale object from slot 1
b.cmd(f'pkcs11-tool --module {PKCS_MOD} --slot 1 -p {USER_PIN} '
      '--delete-object --type data --label flash-record 2>/dev/null; true', 30)

# pkcs11 write to slot 1
log("\n=== PKCS#11 write data object (slot 1 / yuri) ===")
out = b.cmd(f"printf 'W77Q-PKCS11-XVAL-SH001' | "
            f"pkcs11-tool --module {PKCS_MOD} --slot 1 -p {USER_PIN} "
            f"--write-object /dev/stdin --type data --label flash-record --id 01 2>&1", 30)
for line in out.split('\n'):
    line = line.strip()
    if line and 'SYNC' not in line:
        log(f"  {line}")

# after LUT
log("\n=== AFTER w77q-dump list-all ===")
out1 = b.cmd('w77q-dump list-all 2>&1')
log(out1.split('SYNC')[0].strip())

# delta
after_entries = set(re.findall(r'flash_off=(\S+)', out1))
new_entries = after_entries - base_entries
log(f"\n=== New LUT entries after PKCS#11 write: {sorted(new_entries)} ===")

# raw hex dump of each new flash entry
for off in sorted(new_entries):
    log(f"\n=== w77q-dump read-raw {off} 128 ===")
    out = b.cmd(f'w77q-dump read-raw {off} 128 2>&1', 20)
    for line in out.split('\n'):
        if line.strip() and 'SYNC' not in line:
            log(f"  {line}")

if not new_entries:
    log("No new LUT entries — PKCS#11 may have updated an existing record")
    log(f"  before offsets: {sorted(base_entries)}")
    log(f"  after  offsets: {sorted(after_entries)}")
    moved = after_entries.symmetric_difference(base_entries)
    if moved:
        log(f"  changed offsets: {sorted(moved)}")

b.send('stty echo\r')
time.sleep(0.1)
b.close()
log("\nDone.")
