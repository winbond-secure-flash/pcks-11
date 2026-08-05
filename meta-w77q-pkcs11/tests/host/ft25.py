#!/usr/bin/env python3
"""ft25.py — W77Q RSA Encrypt/Decrypt host runner.

Optionally deploys ft25-target.sh + common.sh to the board, then
runs the script and streams all output live.  Parses the summary
line and exits 0 if all checks pass, 1 otherwise.

Checks performed on-board:
  FT25.1 — RSA-2048 key pair generation
  FT25.2 — Export public key DER from token
  FT25.3 — Encrypt plaintext with public key (openssl RSA-PKCS1)
  FT25.4 — Decrypt ciphertext with private key (PKCS#11 RSA-PKCS)
  FT25.5 — Roundtrip integrity: decrypted == original plaintext
  FT25.6 — Private key non-extractable (CKA_SENSITIVE=TRUE)
"""
import argparse, serial, time, re, sys, os, subprocess

PORT = '/dev/ttyUSB0'
BAUD = 921600
BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

ap = argparse.ArgumentParser(
    description='RSA keygen + encrypt/decrypt: deploy ft25-target.sh and run it')
ap.add_argument('--port',   default=PORT, help=f'Serial port (default: {PORT})')
ap.add_argument('--deploy', action='store_true',
                help='deploy ft25-target.sh + common.sh to /usr/bin before running')
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
    lines_on_board = out.split('\n')[0].strip()
    log(f'  {dest_path}: {lines_on_board}')


def stream_command(cmd, timeout=600):
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

        if not chunk and time.time() - last_activity > 5:
            if buf:
                print(buf.decode(errors='replace'), end='', flush=True)
            last_activity = time.time()

    return '\n'.join(output_lines)


# ── setup ──────────────────────────────────────────────────────────────────
s.write(b"\r"); time.sleep(0.3)
read_until("# ", 8); s.reset_input_buffer()
s.write(b"stty echo\r"); time.sleep(0.2); s.read(65536)

log("\n════════════════════════════════════════════════════")
log("  FT25 — W77Q RSA Encrypt/Decrypt")
log("════════════════════════════════════════════════════")

# ── deploy (optional) ──────────────────────────────────────────────────────
if args.deploy:
    log("\n[deploy] deploying scripts...")
    for src, dst in [
        ('scripts/target/common.sh',      '/usr/bin/tee-tests-common.sh'),
        ('scripts/target/ft25-target.sh', '/usr/bin/ft25-target.sh'),
    ]:
        full_src = os.path.join(BASE, src)
        log(f'  -> {dst}')
        deploy_script(full_src, dst)
    log("[deploy] done\n")

# ── run ft25-target.sh ─────────────────────────────────────────────────────
log("\n[run] /usr/bin/ft25-target.sh\n" + "=" * 50)
t0 = time.time()
output = stream_command('ft25-target.sh 2>&1', timeout=600)
elapsed = time.time() - t0
log(f"\n  completed in {elapsed:.0f}s")

# ── parse result ───────────────────────────────────────────────────────────
summary_m = re.search(r'Summary[^\n]*\((\d+)/(\d+) passed\)', output, re.IGNORECASE)
pass_m    = re.search(r'\[PASS\].*PASSED.*\((\d+)/(\d+)\)', output)

log("\n════════════════════════════════════════════════════")
if summary_m:
    passed = int(summary_m.group(1))
    total  = int(summary_m.group(2))
    if passed == total:
        log(f"  FT25: PASS  ({passed}/{total} checks)")
        rc = 0
    else:
        log(f"  FT25: FAIL  ({passed}/{total} checks passed)")
        rc = 1
elif pass_m:
    log(f"  FT25: PASS  ({pass_m.group(1)}/{pass_m.group(2)} checks)")
    rc = 0
else:
    log("  FT25: FAIL  (no summary line found in output)")
    rc = 1
log("════════════════════════════════════════════════════")

s.write(b"stty echo\r")
s.close()
sys.exit(rc)
