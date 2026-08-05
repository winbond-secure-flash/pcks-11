#!/usr/bin/env python3
"""ft26.py — PKCS#11 full-integration test host runner.

Covers: C_FindObjects filters, C_GetAttributeValue completeness,
sensitive-attribute denial, multi-object CRUD, AES-GCM (if available),
RSA-4096 sign/verify (optional), required mechanism presence, and
unsupported-mechanism error paths.

Usage:
  python3 scripts/ft26.py [--port /dev/ttyUSB0] [--deploy] [--no-rsa4096]
"""
import argparse, serial, time, re, sys, os, subprocess

PORT = '/dev/ttyUSB0'
BAUD = 921600
BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

ap = argparse.ArgumentParser(description='PKCS#11 full-integration test')
ap.add_argument('--port',       default=PORT)
ap.add_argument('--deploy',     action='store_true',
                help='deploy scripts to /usr/bin before running')
ap.add_argument('--no-rsa4096', action='store_true',
                help='skip RSA-4096 keygen (slow, ~5 min)')
args = ap.parse_args()
PORT = args.port


def log(m=""): print(m, flush=True)


def kill_port_holders():
    r = subprocess.run(['lsof', PORT], capture_output=True, text=True)
    for line in r.stdout.splitlines()[1:]:
        parts = line.split()
        if len(parts) > 1 and parts[1].isdigit():
            subprocess.run(['kill', parts[1]])
    if r.stdout.strip():
        time.sleep(1)


kill_port_holders()
s = serial.Serial(PORT, BAUD, timeout=0.1, exclusive=False)


def read_until(m, timeout=30):
    buf = b""
    t0 = time.time()
    while time.time() - t0 < timeout:
        buf += s.read(4096)
        if m.encode() in buf:
            return buf.decode(errors="replace")
    return buf.decode(errors="replace")


def deploy_script(src_path, dest_path):
    lines = open(src_path).readlines()
    s.write(f"cat > {dest_path} << 'ENDSCRIPT'\n".encode())
    time.sleep(0.4); s.read(65536)
    for line in lines:
        safe = line.rstrip('\n').encode('ascii', errors='replace').decode('ascii')
        s.write((safe + '\n').encode('ascii'))
        time.sleep(0.03)
    time.sleep(0.5)
    s.write(b'ENDSCRIPT\r'); time.sleep(1); s.read(65536)
    s.write(f'chmod +x {dest_path}\r'.encode()); time.sleep(0.4); s.read(65536)
    s.reset_input_buffer()
    s.write(f'wc -l {dest_path}\r'.encode()); time.sleep(1)
    out = s.read(65536).decode(errors='replace').strip()
    log(f'  {dest_path}: {out.split(chr(10))[0].strip()}')


def stream_command(cmd, timeout=900):
    s.reset_input_buffer()
    s.write((cmd + '\r').encode())
    time.sleep(0.5)
    buf = b''
    t0 = time.time()
    last_activity = time.time()
    output_lines = []
    while True:
        try:
            chunk = s.read(4096)
        except Exception:
            time.sleep(0.1)
            continue
        if chunk:
            buf += chunk
            last_activity = time.time()
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                text = line.decode(errors='replace').rstrip('\r')
                print(text, flush=True)
                output_lines.append(text)
        decoded = buf.decode(errors='replace')
        if decoded.endswith('# ') or decoded.endswith('$ '):
            if buf:
                text = buf.decode(errors='replace').rstrip()
                print(text, flush=True)
                output_lines.append(text)
            break
        if time.time() - t0 > timeout:
            log('[timeout] command exceeded limit')
            break
        if not chunk and time.time() - last_activity > 10:
            if buf:
                print(buf.decode(errors='replace'), end='', flush=True)
            last_activity = time.time()
    return '\n'.join(output_lines)


# ── setup ──────────────────────────────────────────────────────────────────
s.write(b"\r"); time.sleep(0.3)
read_until("# ", 8); s.reset_input_buffer()
s.write(b"stty echo\r"); time.sleep(0.2); s.read(65536)

log("\n════════════════════════════════════════════════════════════")
log("  FT26 — PKCS#11 Full Integration: Search · Attrs · Mechanisms")
log("════════════════════════════════════════════════════════════")

# ── deploy ─────────────────────────────────────────────────────────────────
if args.deploy:
    log("\n[deploy] deploying scripts...")
    for src, dst in [
        ('scripts/target/common.sh',      '/usr/bin/tee-tests-common.sh'),
        ('scripts/target/ft26-target.sh', '/usr/bin/ft26-target.sh'),
    ]:
        log(f'  -> {dst}')
        deploy_script(os.path.join(BASE, src), dst)
    log("[deploy] done\n")

# ── run ────────────────────────────────────────────────────────────────────
extra = '--no-rsa4096' if args.no_rsa4096 else ''
cmd = f'ft26-target.sh {extra} 2>&1'.strip()
timeout = 420 if args.no_rsa4096 else 900

log(f"\n[run] /usr/bin/{cmd}\n" + "=" * 60)
t0 = time.time()
output = stream_command(cmd, timeout=timeout)
elapsed = time.time() - t0
log(f"\n  completed in {elapsed:.0f}s")

# ── parse result ───────────────────────────────────────────────────────────
summary_m = re.search(r'Summary[^\n]*\((\d+)/(\d+) passed', output, re.IGNORECASE)
pass_m    = re.search(r'\[PASS\]\s+FT26\s+PASSED\s+\((\d+)/(\d+)\)', output)
fail_m    = re.search(r'\[FAIL\]\s+FT26\s+FAILED', output)

log("\n════════════════════════════════════════════════════════════")
if pass_m or (summary_m and summary_m.group(1) == summary_m.group(2)):
    total = int((pass_m or summary_m).group(2))
    ok    = int((pass_m or summary_m).group(1))
    log(f"  FT26: PASS  ({ok}/{total})")
    rc = 0
elif fail_m or summary_m:
    ok    = int(summary_m.group(1)) if summary_m else 0
    total = int(summary_m.group(2)) if summary_m else '?'
    log(f"  FT26: FAIL  ({ok}/{total})")
    rc = 1
else:
    log("  FT26: FAIL  (no summary line found)")
    rc = 1
log("════════════════════════════════════════════════════════════")

s.write(b"stty echo\r")
s.close()
sys.exit(rc)
