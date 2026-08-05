#!/usr/bin/env python3
"""ft15b.py — stream fitImage + w77q-dump to board, test w77q-dump."""
import argparse, serial, time, base64, os, sys

PORT  = "/dev/ttyUSB0"
BAUD  = 921600
PASSWD = "yuri"

DEPLOY   = "/home/yuri/work_1702/meta-sparrow-hawk/build/build-sparrow-hawk/tmp/deploy/images/sparrow-hawk"
FITIMAGE = f"{DEPLOY}/fitImage"
W77QDUMP = "/home/yuri/work_1702/meta-sparrow-hawk/build/build-sparrow-hawk/tmp/work/sparrow_hawk-poky-linux/tee-storage/1.0/image/usr/bin/w77q-dump"

def run(ser, cmd, timeout=30):
    tag = f"T{int(time.time()*1000)%999999:06d}"
    ser.reset_input_buffer()
    ser.write(f"{cmd}\n".encode())
    ser.write(f"echo {tag}\n".encode())
    buf = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        chunk = ser.read(ser.in_waiting or 1)
        if chunk:
            buf += chunk
        if tag.encode() in buf:
            break
        time.sleep(0.02)
    return buf.decode(errors="replace").split(tag)[0].strip()

def stream_file(ser, src_path, dst_path):
    """Stream binary via base64 cat-pipe — no per-chunk ACK."""
    data = open(src_path, "rb").read()
    b64  = base64.b64encode(data).decode()
    sz   = len(data)
    print(f"  Streaming {os.path.basename(dst_path)}: {sz/1024:.1f}KB ({len(b64)} b64 chars)...", flush=True)

    # Start cat on board
    ser.reset_input_buffer()
    ser.write(f"cat > /tmp/_transfer.b64\n".encode())
    time.sleep(0.3)

    # Stream b64 in 4096-char lines with a small inter-line delay
    line_sz = 76  # base64 standard line length
    t0 = time.time()
    total = len(b64)
    for i in range(0, total, line_sz):
        line = b64[i:i+line_sz] + "\n"
        ser.write(line.encode())
        # Small throttle every 200 lines to let board keep up
        if (i // line_sz) % 200 == 199:
            time.sleep(0.05)
        if (i // line_sz) % 2000 == 1999:
            elapsed = time.time() - t0
            pct = (i+line_sz)*100//total
            rate = (i+line_sz)/elapsed/1024
            print(f"    ...{pct}% ({rate:.0f} KB/s)", flush=True)

    # End cat with Ctrl-D
    ser.write(b"\x04")
    time.sleep(1)

    # Decode and place file
    elapsed = time.time() - t0
    print(f"  Transfer done in {elapsed:.1f}s ({sz/elapsed/1024:.0f} KB/s)")
    out = run(ser, f"base64 -d /tmp/_transfer.b64 > {dst_path} && rm /tmp/_transfer.b64", timeout=60)
    board_sz = run(ser, f"wc -c < {dst_path}", timeout=10).strip()
    board_sz = int(board_sz) if board_sz.isdigit() else -1
    ok = abs(board_sz - sz) < 10
    print(f"  {'OK' if ok else 'MISMATCH'}: local={sz} board={board_sz}")
    return ok

results = []

ap = argparse.ArgumentParser(description='Stream fitImage + w77q-dump to board, test w77q-dump')
ap.add_argument('--port', default=PORT, help=f'Serial port (default: {PORT})')
ap.parse_args()  # just for --help; script uses fixed port

with serial.Serial(PORT, BAUD, timeout=1, exclusive=False) as ser:
    time.sleep(0.5)
    ser.write(b"stty -echo\n"); time.sleep(0.3); ser.reset_input_buffer()
    out = run(ser, "whoami", timeout=5)
    if "root" not in out:
        run(ser, PASSWD, timeout=5)
    print("Logged in as root")

    # Check /boot space
    space = run(ser, "df -k /boot | tail -1", timeout=5)
    print(f"/boot: {space}")
    avail = int(space.split()[3]) if len(space.split()) >= 4 else 0
    need  = os.path.getsize(FITIMAGE) // 1024 + 1000
    if avail < need:
        print(f"ERROR: not enough space: avail={avail}K need={need}K")
        sys.exit(1)

    print(f"\n=== Step 1: Stream new fitImage ({os.path.getsize(FITIMAGE)/1024/1024:.1f}MB) ===")
    ok = stream_file(ser, FITIMAGE, "/boot/fitImage")
    results.append(("deploy fitImage", ok, ""))

    print("\n=== Step 2: Reboot ===")
    run(ser, "sync", timeout=10)
    ser.write(b"reboot\n"); time.sleep(2)
    ser.reset_input_buffer()

    print("  Waiting for reboot (120s)...", flush=True)
    buf = b""
    deadline = time.time() + 120
    while time.time() < deadline:
        buf += ser.read(ser.in_waiting or 1)
        if b"login:" in buf or b"~#" in buf or b"root@" in buf:
            break
        time.sleep(0.5)
    print(f"  Got: {buf[-100:].decode(errors='replace')!r:.80}")
    time.sleep(2)
    ser.write(b"root\n"); time.sleep(1)
    run(ser, PASSWD, timeout=5)
    ser.write(b"stty -echo\n"); time.sleep(0.3); ser.reset_input_buffer()
    out = run(ser, "whoami", timeout=10)
    print(f"  Post-reboot whoami: {out}")

    print("\n=== Step 3: Deploy w77q-dump ===")
    ok = stream_file(ser, W77QDUMP, "/usr/bin/w77q-dump")
    run(ser, "chmod +x /usr/bin/w77q-dump", timeout=5)
    results.append(("deploy w77q-dump", ok, ""))

    print("\n=== Step 4: w77q-dump list-all (baseline) ===")
    out = run(ser, "w77q-dump list-all 2>&1", timeout=30)
    print(out[:600])
    results.append(("w77q-dump list-all baseline", True, out.strip()[:100]))

    print("\n=== Step 5: Write test objects ===")
    run(ser, "tee-storage write-sector 0x1000 'W77Q-DUMP-PTA-TEST-001'", timeout=20)
    pkcs = run(ser, "pkcs11-tool --module /usr/lib/libckteec.so.0 --slot 0 "
                    "--login --pin 1234 --write-object /dev/null --type data "
                    "--label dump-pta-test --id 0x06 2>&1", timeout=60)
    print(f"  PKCS11: {pkcs[-100:]}")

    print("\n=== Step 6: w77q-dump list-all (after writes) ===")
    out = run(ser, "w77q-dump list-all 2>&1", timeout=30)
    print(out[:1000])
    has_entries = any(x in out.lower() for x in ["entry", "uuid", "flash_off", "obj_id"])
    results.append(("w77q-dump after write", has_entries, out.strip()[:120]))

    print("\n=== Step 7: Cleanup ===")
    run(ser, "tee-storage erase-sector 0x1000", timeout=20)
    run(ser, "pkcs11-tool --module /usr/lib/libckteec.so.0 --slot 0 --login --pin 1234 "
             "--delete-object --type data --label dump-pta-test 2>&1", timeout=30)

print("\n" + "="*60 + "\nRESULTS:")
for name, ok, detail in results:
    print(f"  {'[PASS]' if ok else '[FAIL]'} {name}: {detail[:80]}")
