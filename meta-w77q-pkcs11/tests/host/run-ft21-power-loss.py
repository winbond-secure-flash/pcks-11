#!/usr/bin/env python3
"""
run-ft21-power-loss.py - Host runner for ft21 PKCS#11 power-loss test.
Phase 1: deploy scripts, run ft21-phase1.sh (creates key, reboots board)
Wait:    poll serial for board to come back online (up to 120s)
Phase 2: run ft21-phase2.sh (verify key survived)
"""
import argparse, serial, time, sys, subprocess, os

PORT = '/dev/ttyUSB0'
BAUD = 921600
BASE = os.path.dirname(os.path.abspath(__file__))

SCRIPTS = [
    (f'{BASE}/target/common.sh',      '/usr/bin/tee-tests-common.sh'),
    (f'{BASE}/target/ft21-phase1.sh', '/usr/bin/ft21-phase1.sh'),
    (f'{BASE}/target/ft21-phase2.sh', '/usr/bin/ft21-phase2.sh'),
]

def kill_port_holders():
    r = subprocess.run(['lsof', PORT], capture_output=True, text=True)
    for line in r.stdout.splitlines()[1:]:
        pid = line.split()[1]
        try: os.kill(int(pid), 9)
        except Exception: pass
    time.sleep(0.5)

def open_port(retries=10):
    for i in range(retries):
        try:
            return serial.Serial(PORT, BAUD, timeout=2.0)
        except Exception as e:
            print(f'[port] attempt {i+1}: {e}'); time.sleep(2)
    print('[port] FAILED'); sys.exit(1)

def deploy(s, src, dst):
    lines = open(src).readlines()
    marker = 'XEOF_DEPLOY_MARKER'
    s.write(f"cat > {dst} << '{marker}'\n".encode())
    time.sleep(0.4); s.read(65536)
    for line in lines:
        safe = line.rstrip('\n').encode('ascii', errors='replace').decode('ascii')
        s.write((safe + '\n').encode('ascii'))
        time.sleep(0.03)
    time.sleep(0.5)
    s.write(f'{marker}\r'.encode()); time.sleep(1); s.read(65536)
    s.write(f'chmod +x {dst}\r'.encode()); time.sleep(0.4); s.read(65536)

def stream(s, cmd, timeout=180, stop_on_reboot=False):
    s.reset_input_buffer()
    s.write((cmd + '\r').encode())
    buf = b''; t0 = time.time(); last = time.time()
    while True:
        try:
            chunk = s.read(4096)
        except Exception:
            print('[runner] serial disconnected (board rebooting)')
            return 'REBOOTING'
        if chunk:
            buf += chunk; last = time.time()
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                text = line.decode(errors='replace').rstrip('\r')
                print(text)
                if stop_on_reboot and (
                        'Restarting system' in text or
                        'The system is going down' in text or
                        'reboot: Restarting' in text or
                        'Rebooting in' in text):
                    print('[runner] reboot detected, closing port...')
                    return 'REBOOTING'
        dec = buf.decode(errors='replace')
        if dec.endswith('# ') or dec.endswith('$ '):
            if buf: print(buf.decode(errors='replace').rstrip())
            return 'DONE'
        if time.time() - t0 > timeout:
            print('[timeout]'); return 'TIMEOUT'
        if not chunk and time.time() - last > 5:
            if buf: print(buf.decode(errors='replace'), end='', flush=True)
            last = time.time()

def wait_for_board(timeout=180):
    print(f'[runner] polling for board online (up to {timeout}s)...')
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            s = serial.Serial(PORT, BAUD, timeout=3.0)
            s.reset_input_buffer()
            s.write(b'\r'); time.sleep(2)
            r = s.read(4096).decode(errors='replace')
            if '=>' in r:
                # U-Boot prompt - send boot command
                print('[runner] U-Boot detected, sending boot...')
                s.write(b'boot\r')
                # now wait for Linux login (up to 90s more)
                buf = ''; dl = time.time() + 90
                while time.time() < dl:
                    d = s.read(4096)
                    if d:
                        chunk = d.decode(errors='replace')
                        buf += chunk
                        print(chunk, end='', flush=True)
                        if 'login' in buf[-100:]:
                            break
                if 'login' in buf.lower():
                    s.write(b'root\r'); time.sleep(3)
                    s.read(4096)
                    s.write(b'stty echo\r'); time.sleep(0.5); s.read(4096)
                    s.write(b'\r'); time.sleep(0.5); s.read(4096)
                    print(f'\n[runner] board online + logged in after {int(time.time()-t0)}s')
                    return s
                s.close(); continue
            if 'login' in r.lower():
                # Linux login prompt
                s.write(b'root\r'); time.sleep(3)
                r2 = s.read(4096).decode(errors='replace')
                if 'assword' in r2:
                    s.write(b'\r'); time.sleep(2); s.read(4096)
                # wait for shell prompt
                deadline2 = time.time() + 30
                buf = r2
                while time.time() < deadline2:
                    d = s.read(4096).decode(errors='replace')
                    if d: buf += d
                    if buf.rstrip().endswith('#') or buf.rstrip().endswith('$'): break
                    time.sleep(0.5)
                s.write(b'stty echo\r'); time.sleep(0.5); s.read(4096)
                s.write(b'\r'); time.sleep(0.5); s.read(4096)
                print(f'[runner] board online + logged in after {int(time.time()-t0)}s')
                return s
            if r.rstrip().endswith('#') or r.rstrip().endswith('$'):
                s.write(b'stty echo\r'); time.sleep(0.3); s.read(4096)
                print(f'[runner] board online after {int(time.time()-t0)}s')
                return s
            s.close()
        except Exception:
            pass
        time.sleep(3)
    print('[runner] board did not come back'); sys.exit(1)

# ── main ──────────────────────────────────────────────────────────────────
ap = argparse.ArgumentParser(description='Host runner for ft21 PKCS#11 power-loss test')
ap.add_argument('--port', default=PORT, help=f'Serial port (default: {PORT})')
ap.parse_args()  # just for --help; script uses fixed port

kill_port_holders()
s = open_port()
s.reset_input_buffer()
s.write(b'\r'); time.sleep(1.5)
r = s.read(4096).decode(errors='replace')

# Handle login prompt at startup
if 'login' in r.lower():
    print('[runner] at login prompt, logging in as root...')
    s.write(b'root\r'); time.sleep(3)
    r2 = s.read(4096).decode(errors='replace')
    if 'assword' in r2:
        s.write(b'\r'); time.sleep(2); s.read(4096)
    # wait for shell prompt
    deadline = time.time() + 30
    buf = r2
    while time.time() < deadline:
        d = s.read(4096).decode(errors='replace')
        if d: buf += d
        if buf.rstrip().endswith('#') or buf.rstrip().endswith('$'): break
        time.sleep(0.5)

s.write(b'stty echo\r'); time.sleep(0.3); s.read(65536)
s.write(b'\r'); time.sleep(0.5); s.read(65536)

print('[runner] deploying scripts...')
for src, dst in SCRIPTS:
    print(f'  -> {dst}')
    deploy(s, src, dst)
print('[runner] deploy done\n')

print('=' * 60)
print('[runner] PHASE 1: create persistent key + reboot')
print('=' * 60)
result = stream(s, 'ft21-phase1.sh 2>&1', timeout=120, stop_on_reboot=True)
s.close()

print(f'\n[runner] phase-1 result: {result}')
if result == 'TIMEOUT':
    sys.exit(1)

print('[runner] waiting for board to reboot...')
time.sleep(8)
s = wait_for_board(timeout=120)

print('\n' + '=' * 60)
print('[runner] PHASE 2: verify key survived power loss')
print('=' * 60)
# suppress kernel console messages during test
stream(s, 'dmesg -n 1; ft21-phase2.sh 2>&1; dmesg -n 7', timeout=120)
s.close()
print('\n[runner] ft21 complete.')
