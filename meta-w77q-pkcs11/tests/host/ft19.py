#!/usr/bin/env python3
"""ft19.py — RSA TEE keygen + encrypt (pubkey) + decrypt (privkey) host runner.

Optionally deploys ft19-target.sh to the board, then runs it and streams
all output live.  Exits 0 on PASS, 1 on FAIL.

Workflow (on-board ft19-target.sh):
  1. Create plaintext file
  2. Ensure PKCS#11 token initialised
  3. Generate RSA-2048 keypair in TEE (private key never leaves TEE)
  4. Export public key DER -> PEM (openssl)
  5. Encrypt with public key (openssl rsautl)
  6. Decrypt with TEE private key (pkcs11-tool C_Decrypt / RSA-PKCS)
  7. Compare decrypted == plaintext (cmp byte-exact)
  8. w77q-dump list-all (confirm key persisted in flash)
"""
import argparse, serial, time, re, sys, os, subprocess

PORT = '/dev/ttyUSB0'
BAUD = 921600
BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

ap = argparse.ArgumentParser(
    description='RSA TEE encrypt/decrypt: deploy ft19-target.sh and run it')
ap.add_argument('--port',   default=PORT, help=f'Serial port (default: {PORT})')
ap.add_argument('--deploy', action='store_true',
                help='deploy ft19-target.sh to /usr/bin before running')
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
log("  FT19 — RSA TEE keygen + encrypt (pubkey) + decrypt (privkey)")
log("════════════════════════════════════════════════════")

# ── deploy (optional) ──────────────────────────────────────────────────────
if args.deploy:
    log("\n[deploy] deploying ft19-target.sh...")
    deploy_script(os.path.join(BASE, 'scripts/target/ft19-target.sh'),
                  '/usr/bin/ft19-target.sh')
    log("[deploy] done\n")

# ── run ft19-target.sh ─────────────────────────────────────────────────────
log("\n[run] /usr/bin/ft19-target.sh\n" + "=" * 50)
t0 = time.time()
output = stream_command('ft19-target.sh 2>&1', timeout=600)
elapsed = time.time() - t0
log(f"\n  completed in {elapsed:.0f}s")

# ── parse result ───────────────────────────────────────────────────────────
log("\n════════════════════════════════════════════════════")
if 'Plaintext match            : PASS' in output or 'PASS' in output.split('\n')[-3:]:
    log("  FT19: PASS")
    rc = 0
elif 'FAIL' in output:
    log("  FT19: FAIL")
    rc = 1
else:
    log("  FT19: FAIL  (no result line found)")
    rc = 1
log("════════════════════════════════════════════════════")

s.write(b"stty echo\r")
s.close()
sys.exit(rc)
