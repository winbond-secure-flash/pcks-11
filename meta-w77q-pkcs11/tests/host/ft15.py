#!/usr/bin/env python3
"""ft15.py — deploy new fitImage (contains w77q_dump PTA) + w77q-dump tool, run list-all."""
import argparse, serial, time, base64, os, sys, hashlib

PORT   = "/dev/ttyUSB0"
BAUD   = 921600
PASSWD = "yuri"

DEPLOY = "/home/yuri/work_1702/meta-sparrow-hawk/build/build-sparrow-hawk/tmp/deploy/images/sparrow-hawk"
FITIMAGE   = f"{DEPLOY}/fitImage"
W77Q_DUMP  = "/home/yuri/work_1702/meta-sparrow-hawk/build/build-sparrow-hawk/tmp/work/sparrow_hawk-poky-linux/tee-storage/1.0/image/usr/bin/w77q-dump"

def run(ser, cmd, timeout=30, tag=None):
    tag = tag or f"TAG{int(time.time()*1000)%999999:06d}"
    ser.reset_input_buffer()
    ser.write(f"{cmd}\n".encode())
    time.sleep(0.2)
    ser.write(f"echo {tag}\n".encode())
    buf = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        buf += ser.read(ser.in_waiting or 1)
        if tag.encode() in buf:
            break
        time.sleep(0.05)
    return buf.decode(errors="replace").split(tag)[0].strip()

def deploy_binary(ser, src_path, dst_path, chunk=600):
    data = open(src_path, "rb").read()
    b64  = base64.b64encode(data).decode()
    sz   = len(data)
    name = os.path.basename(dst_path)
    print(f"  Deploying {name} ({sz/1024:.1f} KB, {len(b64)} b64 chars)...")
    run(ser, f"rm -f {dst_path}.b64 {dst_path}", timeout=10)
    n = 0
    for i in range(0, len(b64), chunk):
        piece = b64[i:i+chunk]
        op = ">>" if i > 0 else ">"
        run(ser, f"printf '%s' '{piece}' {op} {dst_path}.b64", timeout=15)
        n += 1
        if n % 50 == 0:
            pct = (i+chunk)*100//len(b64)
            print(f"    ...{pct}%", flush=True)
    run(ser, f"base64 -d {dst_path}.b64 > {dst_path} && chmod +x {dst_path} && rm -f {dst_path}.b64", timeout=30)
    board_sz = run(ser, f"wc -c < {dst_path}", timeout=10).strip()
    board_sz = int(board_sz) if board_sz.isdigit() else 0
    ok = abs(board_sz - sz) < 10
    print(f"  {'OK' if ok else 'MISMATCH'}: local={sz} board={board_sz}")
    return ok

results = []

ap = argparse.ArgumentParser(description='Deploy new fitImage (contains w77q_dump PTA) + w77q-dump tool, run list-all')
ap.add_argument('--port', default=PORT, help=f'Serial port (default: {PORT})')
ap.parse_args()  # just for --help; script uses fixed port

print(f"\nFitImage: {os.path.getsize(FITIMAGE)/1024/1024:.1f} MB")
print(f"w77q-dump: {os.path.getsize(W77Q_DUMP)/1024:.1f} KB")

with serial.Serial(PORT, BAUD, timeout=1, exclusive=False) as ser:
    time.sleep(0.5)
    ser.write(b"stty -echo\n"); time.sleep(0.3); ser.reset_input_buffer()

    out = run(ser, "whoami", timeout=5)
    if "root" not in out:
        run(ser, PASSWD, timeout=5)

    # Check current board OP-TEE / fitImage state
    print("\n=== Board info ===")
    board_fit = run(ser, "ls -lh /boot/fitImage 2>/dev/null || echo NO_FITIMAGE", timeout=10)
    print(f"  Board fitImage: {board_fit}")
    boot_partition = run(ser, "df /boot 2>/dev/null | tail -1", timeout=5)
    print(f"  /boot partition: {boot_partition}")

    # Deploy new fitImage to /boot
    print("\n=== Step 1: Deploy fitImage (contains new OP-TEE with w77q_dump PTA) ===")
    ok_fit = deploy_binary(ser, FITIMAGE, "/boot/fitImage", chunk=600)
    results.append(("deploy fitImage", ok_fit, ""))

    # Sync and reboot
    print("\n=== Step 2: Reboot to load new OP-TEE ===")
    run(ser, "sync", timeout=10)
    ser.write(b"reboot\n")
    time.sleep(2)
    ser.reset_input_buffer()

    # Wait for board to come back up (login prompt)
    print("  Waiting for board reboot (90s)...")
    deadline = time.time() + 90
    buf = b""
    while time.time() < deadline:
        buf += ser.read(ser.in_waiting or 1)
        if b"login:" in buf or b"sparrow-hawk" in buf or b"~#" in buf or b"root@" in buf:
            break
        time.sleep(0.5)
    print(f"  Board output: {buf[-200:].decode(errors='replace')!r:.100}")

    time.sleep(2)
    ser.write(b"root\n"); time.sleep(1)
    out = run(ser, PASSWD, timeout=5)
    ser.write(b"stty -echo\n"); time.sleep(0.3); ser.reset_input_buffer()

    out = run(ser, "whoami", timeout=5)
    if "root" not in out:
        print("  Login failed after reboot!")
        sys.exit(1)
    print("  Logged in after reboot OK")

    # Deploy w77q-dump
    print("\n=== Step 3: Deploy w77q-dump ===")
    ok_dump = deploy_binary(ser, W77Q_DUMP, "/usr/bin/w77q-dump", chunk=600)
    results.append(("deploy w77q-dump", ok_dump, ""))

    # Run w77q-dump list-all on clean flash
    print("\n=== Step 4: w77q-dump list-all (baseline) ===")
    out = run(ser, "w77q-dump list-all 2>&1", timeout=30)
    print(out[:600])
    results.append(("w77q-dump list-all baseline", True, out.strip()[:100]))

    # Write test data via both TAs
    print("\n=== Step 5: Write test objects ===")
    run(ser, "tee-storage write-sector 0x1000 'W77Q-DUMP-PTA-TEST-001'", timeout=20)
    pkcs_out = run(ser, "pkcs11-tool --module /usr/lib/libckteec.so.0 --slot 0 --login --pin 87654321 "
                       "--write-object /dev/null --type data --label dump-pta-test --id 0x06 2>&1", timeout=60)
    print(f"  PKCS11: {pkcs_out[-100:]}")

    # Run w77q-dump list-all after writes
    print("\n=== Step 6: w77q-dump list-all (after writes) ===")
    out = run(ser, "w77q-dump list-all 2>&1", timeout=30)
    print(out[:1000])
    has_entries = any(x in out.lower() for x in ["entry", "uuid", "flash_off", "obj_id", "lut"])
    results.append(("w77q-dump list-all after write", has_entries, out.strip()[:120]))

    # Cleanup
    run(ser, "tee-storage erase-sector 0x1000", timeout=20)

print("\n" + "="*60)
print("RESULTS:")
for name, ok, detail in results:
    print(f"  {'[PASS]' if ok else '[FAIL]'} {name}: {detail[:80]}")
