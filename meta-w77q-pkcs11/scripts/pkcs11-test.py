#!/usr/bin/env python3
"""
PKCS#11 interactive test tool for Sparrow Hawk board.

Runs on the host, sends commands to the board via serial.
Provides a menu-driven interface to test all PKCS#11 operations:
  - Token management (init, info, list slots)
  - PIN management (init, change, lock/unlock)
  - RSA key operations (generate, sign, verify, delete)
  - ECDSA key operations (generate, sign, verify, delete)
  - Object management (list, delete)
  - Persistence test (reboot + verify keys survive)

Usage:
    ./scripts/pkcs11-test.py [--port /dev/ttyUSB0] [--baud 921600]
"""

import argparse
import sys
import time

import serial

# ── Defaults ─────────────────────────────────────────────────────────────────
MODULE = "/usr/lib/libckteec.so"
DEFAULT_TOKEN_LABEL = "test-token"
DEFAULT_SO_PIN = "12345678"
DEFAULT_USER_PIN = "876543210"

# ── Serial helpers (reused from serial-shell.py) ─────────────────────────────

def read_until_prompt(ser, timeout=10):
    """Read serial output until shell prompt or timeout."""
    output = ""
    deadline = time.time() + timeout
    idle_start = None

    while time.time() < deadline:
        n = ser.in_waiting
        if n:
            chunk = ser.read(n).decode(errors="replace")
            output += chunk
            idle_start = None
            stripped = output.rstrip()
            if stripped.endswith("# ") or stripped.endswith("$ ") or stripped.endswith("=> "):
                break
        else:
            if idle_start is None:
                idle_start = time.time()
            elif time.time() - idle_start > 1.5:
                if output.strip():
                    break
            time.sleep(0.02)
    return output


def send_command(ser, cmd, timeout=15, quiet=False):
    """Send a command to the board and return its output."""
    ser.reset_input_buffer()

    for byte in (cmd + "\n").encode():
        ser.write(bytes([byte]))
        time.sleep(0.002)
    ser.flush()

    output = read_until_prompt(ser, timeout)
    output = output.replace("\r", "")
    lines = [l for l in output.split("\n") if l.strip()]

    # Remove echoed command and prompt lines
    cmd_words = cmd.split()[:3]
    filtered = []
    for line in lines:
        if cmd_words and cmd_words[0] in line and cmd_words[-1] in line:
            continue
        if line.strip().endswith("#") or line.strip().endswith("$"):
            continue
        filtered.append(line)

    result = "\n".join(filtered)
    if not quiet:
        print(f"\033[90m$ {cmd}\033[0m")
        if result.strip():
            print(result)
        print()
    return result


def pkcs11_cmd(ser, args, timeout=15, quiet=False):
    """Build and run a pkcs11-tool command."""
    cmd = f"pkcs11-tool --module={MODULE} {args}"
    return send_command(ser, cmd, timeout, quiet)


# ── State ────────────────────────────────────────────────────────────────────

class State:
    """Holds current session parameters."""
    def __init__(self):
        self.token_label = DEFAULT_TOKEN_LABEL
        self.so_pin = DEFAULT_SO_PIN
        self.user_pin = DEFAULT_USER_PIN

    def login_args(self):
        return f'--token-label {self.token_label} --login --pin {self.user_pin}'

    def so_login_args(self):
        return f'--token-label {self.token_label} --login-type so --login --pin {self.so_pin}'


# ── Menu sections ────────────────────────────────────────────────────────────

def menu_token(ser, st):
    """Token management submenu."""
    while True:
        print("\n\033[1;36m═══ Token Management ═══\033[0m")
        print("  1) List slots / tokens")
        print("  2) Initialize new token")
        print("  3) Init user PIN")
        print("  4) Change user PIN")
        print("  5) Change SO PIN")
        print("  6) List supported mechanisms")
        print("  0) Back")
        choice = input("\n\033[1mSelect: \033[0m").strip()

        if choice == "1":
            pkcs11_cmd(ser, "-T")

        elif choice == "2":
            slot = input("  Slot number [0]: ").strip() or "0"
            label = input(f"  Token label [{st.token_label}]: ").strip() or st.token_label
            so_pin = input(f"  SO PIN [{st.so_pin}]: ").strip() or st.so_pin
            pkcs11_cmd(ser, f"--init-token --slot {slot} --label {label} --so-pin {so_pin}")
            st.token_label = label
            st.so_pin = so_pin

        elif choice == "3":
            user_pin = input(f"  New user PIN [{st.user_pin}]: ").strip() or st.user_pin
            pkcs11_cmd(ser, f"{st.so_login_args()} --init-pin --new-pin {user_pin}")
            st.user_pin = user_pin

        elif choice == "4":
            old_pin = input(f"  Current PIN [{st.user_pin}]: ").strip() or st.user_pin
            new_pin = input("  New PIN: ").strip()
            if new_pin:
                pkcs11_cmd(ser, f"--token-label {st.token_label} --login --pin {old_pin} --change-pin --new-pin {new_pin}")
                st.user_pin = new_pin

        elif choice == "5":
            old_pin = input(f"  Current SO PIN [{st.so_pin}]: ").strip() or st.so_pin
            new_pin = input("  New SO PIN: ").strip()
            if new_pin:
                pkcs11_cmd(ser, f"--token-label {st.token_label} --login-type so --login --pin {old_pin} --change-pin --new-pin {new_pin}")
                st.so_pin = new_pin

        elif choice == "6":
            pkcs11_cmd(ser, f"--token-label {st.token_label} -M")

        elif choice == "0":
            return


def menu_rsa(ser, st):
    """RSA operations submenu."""
    while True:
        print("\n\033[1;33m═══ RSA Operations ═══\033[0m")
        print("  1) Generate RSA-2048 keypair")
        print("  2) Sign data (SHA256-RSA-PKCS)")
        print("  3) Verify signature (PKCS#11)")
        print("  4) Verify signature (OpenSSL)")
        print("  5) Verify with BAD signature")
        print("  6) Export public key to DER")
        print("  7) Delete RSA keypair")
        print("  0) Back")
        choice = input("\n\033[1mSelect: \033[0m").strip()

        if choice == "1":
            label = input("  Key label [rsa]: ").strip() or "rsa"
            kid = input("  Key ID [10]: ").strip() or "10"
            bits = input("  Key size [2048]: ").strip() or "2048"
            pkcs11_cmd(ser,
                f"{st.login_args()} --keypairgen --key-type RSA:{bits} --label {label} --id {kid} --usage-sign",
                timeout=30)

        elif choice == "2":
            label = input("  Key label [rsa]: ").strip() or "rsa"
            kid = input("  Key ID [10]: ").strip() or "10"
            data = input("  Test data [Hello W77Q RSA]: ").strip() or "Hello W77Q RSA"
            send_command(ser, f'echo "{data}" > /tmp/rsa_testdata.txt')
            pkcs11_cmd(ser,
                f"{st.login_args()} --sign --mechanism SHA256-RSA-PKCS --input-file /tmp/rsa_testdata.txt --output-file /tmp/rsa_sig.bin --id {kid}")
            print("\033[1;32m  → Signature saved to /tmp/rsa_sig.bin\033[0m")

        elif choice == "3":
            kid = input("  Key ID [10]: ").strip() or "10"
            pkcs11_cmd(ser,
                f"{st.login_args()} --verify --mechanism SHA256-RSA-PKCS --input-file /tmp/rsa_testdata.txt --signature-file /tmp/rsa_sig.bin --id {kid}")

        elif choice == "4":
            kid = input("  Key ID [10]: ").strip() or "10"
            # Export pubkey then verify with openssl
            pkcs11_cmd(ser,
                f"{st.login_args()} --read-object --type pubkey --id {kid} --output-file /tmp/rsa_pub.der",
                quiet=True)
            send_command(ser,
                "openssl dgst -sha256 -verify /tmp/rsa_pub.der -keyform DER -signature /tmp/rsa_sig.bin /tmp/rsa_testdata.txt")

        elif choice == "5":
            kid = input("  Key ID [10]: ").strip() or "10"
            send_command(ser,
                'cp /tmp/rsa_sig.bin /tmp/rsa_sig_bad.bin && printf "\\xff" | dd of=/tmp/rsa_sig_bad.bin bs=1 seek=0 count=1 conv=notrunc 2>/dev/null',
                quiet=True)
            print("  Testing with corrupted signature (first byte = 0xFF)...")
            pkcs11_cmd(ser,
                f"{st.login_args()} --verify --mechanism SHA256-RSA-PKCS --input-file /tmp/rsa_testdata.txt --signature-file /tmp/rsa_sig_bad.bin --id {kid}")

        elif choice == "6":
            kid = input("  Key ID [10]: ").strip() or "10"
            pkcs11_cmd(ser,
                f"{st.login_args()} --read-object --type pubkey --id {kid} --output-file /tmp/rsa_pub.der")
            print("\033[1;32m  → Public key saved to /tmp/rsa_pub.der\033[0m")

        elif choice == "7":
            kid = input("  Key ID to delete [10]: ").strip() or "10"
            confirm = input("  Delete PRIVATE + PUBLIC key? [y/N]: ").strip().lower()
            if confirm == "y":
                pkcs11_cmd(ser, f"{st.login_args()} --delete-object --type privkey --id {kid}")
                pkcs11_cmd(ser, f"{st.login_args()} --delete-object --type pubkey --id {kid}")
                print("\033[1;31m  → RSA keypair deleted\033[0m")

        elif choice == "0":
            return


def menu_ecdsa(ser, st):
    """ECDSA operations submenu."""
    while True:
        print("\n\033[1;35m═══ ECDSA Operations ═══\033[0m")
        print("  1) Generate EC P-256 keypair")
        print("  2) Sign data (ECDSA-SHA256)")
        print("  3) Verify signature (PKCS#11)")
        print("  4) Verify with BAD signature")
        print("  5) Delete EC keypair")
        print("  0) Back")
        choice = input("\n\033[1mSelect: \033[0m").strip()

        if choice == "1":
            label = input("  Key label [ec-p256]: ").strip() or "ec-p256"
            kid = input("  Key ID [20]: ").strip() or "20"
            curve = input("  Curve [prime256v1]: ").strip() or "prime256v1"
            pkcs11_cmd(ser,
                f"{st.login_args()} --keypairgen --key-type EC:{curve} --label {label} --id {kid} --usage-sign",
                timeout=20)

        elif choice == "2":
            kid = input("  Key ID [20]: ").strip() or "20"
            data = input("  Test data [Hello W77Q ECDSA]: ").strip() or "Hello W77Q ECDSA"
            send_command(ser, f'echo "{data}" > /tmp/ec_testdata.txt')
            pkcs11_cmd(ser,
                f"{st.login_args()} --sign --mechanism ECDSA-SHA256 --input-file /tmp/ec_testdata.txt --output-file /tmp/ec_sig.bin --id {kid}")
            print("\033[1;32m  → Signature saved to /tmp/ec_sig.bin\033[0m")

        elif choice == "3":
            kid = input("  Key ID [20]: ").strip() or "20"
            pkcs11_cmd(ser,
                f"{st.login_args()} --verify --mechanism ECDSA-SHA256 --input-file /tmp/ec_testdata.txt --signature-file /tmp/ec_sig.bin --id {kid}")

        elif choice == "4":
            kid = input("  Key ID [20]: ").strip() or "20"
            send_command(ser,
                'cp /tmp/ec_sig.bin /tmp/ec_sig_bad.bin && printf "\\xff" | dd of=/tmp/ec_sig_bad.bin bs=1 seek=0 count=1 conv=notrunc 2>/dev/null',
                quiet=True)
            print("  Testing with corrupted signature (first byte = 0xFF)...")
            pkcs11_cmd(ser,
                f"{st.login_args()} --verify --mechanism ECDSA-SHA256 --input-file /tmp/ec_testdata.txt --signature-file /tmp/ec_sig_bad.bin --id {kid}")

        elif choice == "5":
            kid = input("  Key ID to delete [20]: ").strip() or "20"
            confirm = input("  Delete PRIVATE + PUBLIC key? [y/N]: ").strip().lower()
            if confirm == "y":
                pkcs11_cmd(ser, f"{st.login_args()} --delete-object --type privkey --id {kid}")
                pkcs11_cmd(ser, f"{st.login_args()} --delete-object --type pubkey --id {kid}")
                print("\033[1;31m  → EC keypair deleted\033[0m")

        elif choice == "0":
            return


def menu_objects(ser, st):
    """Object management submenu."""
    while True:
        print("\n\033[1;37m═══ Object Management ═══\033[0m")
        print("  1) List all objects")
        print("  2) Delete object by type + ID")
        print("  0) Back")
        choice = input("\n\033[1mSelect: \033[0m").strip()

        if choice == "1":
            pkcs11_cmd(ser, f"{st.login_args()} --list-objects")

        elif choice == "2":
            otype = input("  Type (privkey/pubkey/cert/data) [privkey]: ").strip() or "privkey"
            kid = input("  Object ID: ").strip()
            if kid:
                confirm = input(f"  Delete {otype} id={kid}? [y/N]: ").strip().lower()
                if confirm == "y":
                    pkcs11_cmd(ser, f"{st.login_args()} --delete-object --type {otype} --id {kid}")

        elif choice == "0":
            return


def menu_pin_test(ser, st):
    """PIN counter / lockout test submenu."""
    while True:
        print("\n\033[1;31m═══ PIN Counter Test ═══\033[0m")
        print("  1) Login with correct PIN")
        print("  2) Login with WRONG PIN (increments counter)")
        print("  3) Check token flags (shows COUNT_LOW, FINAL_TRY, LOCKED)")
        print("  4) Unlock user PIN via SO")
        print("  0) Back")
        choice = input("\n\033[1mSelect: \033[0m").strip()

        if choice == "1":
            pkcs11_cmd(ser, f"{st.login_args()} --list-objects", quiet=False)
            print("\033[1;32m  → If no error above, login succeeded (counter reset)\033[0m")

        elif choice == "2":
            wrong_pin = input("  Wrong PIN to try [0000]: ").strip() or "0000"
            pkcs11_cmd(ser,
                f"--token-label {st.token_label} --login --pin {wrong_pin} --list-objects")

        elif choice == "3":
            pkcs11_cmd(ser, "-T")

        elif choice == "4":
            new_pin = input(f"  New user PIN [{st.user_pin}]: ").strip() or st.user_pin
            pkcs11_cmd(ser, f"{st.so_login_args()} --init-pin --new-pin {new_pin}")
            st.user_pin = new_pin
            print(f"\033[1;32m  → User PIN reset to {new_pin}\033[0m")

        elif choice == "0":
            return


def menu_persistence(ser, st):
    """Reboot persistence test."""
    print("\n\033[1;34m═══ Persistence Test ═══\033[0m")
    print("  This will:")
    print("    1. List current objects")
    print("    2. Reboot the board")
    print("    3. Wait for boot")
    print("    4. Verify token + objects still exist")
    print("    5. Sign + verify with persisted key")

    confirm = input("\n  Proceed? [y/N]: ").strip().lower()
    if confirm != "y":
        return

    # 1. Show current state
    print("\n\033[1m── Before reboot ──\033[0m")
    pkcs11_cmd(ser, f"{st.login_args()} --list-objects")

    # 2. Reboot
    print("\n\033[1m── Rebooting... ──\033[0m")
    send_command(ser, "reboot", timeout=3, quiet=True)

    # 3. Wait for boot
    print("  Waiting 40 seconds for boot...", end="", flush=True)
    time.sleep(40)
    # Send a newline to get a prompt
    ser.reset_input_buffer()
    ser.write(b"\n")
    time.sleep(1)
    read_until_prompt(ser, timeout=5)
    print(" done")

    # 4. Check token
    print("\n\033[1m── After reboot: token info ──\033[0m")
    pkcs11_cmd(ser, "-T")

    print("\n\033[1m── After reboot: list objects ──\033[0m")
    result = pkcs11_cmd(ser, f"{st.login_args()} --list-objects")

    # 5. Sign + verify with RSA if it exists
    if "RSA" in result:
        print("\n\033[1m── Post-reboot sign + verify (RSA) ──\033[0m")
        send_command(ser, 'echo "persistence test" > /tmp/persist_test.txt', quiet=True)
        pkcs11_cmd(ser,
            f"{st.login_args()} --sign --mechanism SHA256-RSA-PKCS --input-file /tmp/persist_test.txt --output-file /tmp/persist_sig.bin --id 10")
        pkcs11_cmd(ser,
            f"{st.login_args()} --verify --mechanism SHA256-RSA-PKCS --input-file /tmp/persist_test.txt --signature-file /tmp/persist_sig.bin --id 10")

    if "EC" in result:
        print("\n\033[1m── Post-reboot sign + verify (ECDSA) ──\033[0m")
        send_command(ser, 'echo "persistence test ec" > /tmp/persist_ec_test.txt', quiet=True)
        pkcs11_cmd(ser,
            f"{st.login_args()} --sign --mechanism ECDSA-SHA256 --input-file /tmp/persist_ec_test.txt --output-file /tmp/persist_ec_sig.bin --id 20")
        pkcs11_cmd(ser,
            f"{st.login_args()} --verify --mechanism ECDSA-SHA256 --input-file /tmp/persist_ec_test.txt --signature-file /tmp/persist_ec_sig.bin --id 20")

    print("\n\033[1;32m═══ Persistence test complete ═══\033[0m")


def menu_quick_test(ser, st):
    """Run all tests in sequence — full automated validation."""
    print("\n\033[1;32m═══ Full Automated Test ═══\033[0m")
    print("  This will run all PKCS#11 tests end-to-end:")
    print("    - Token init  - PIN management")
    print("    - RSA keygen, sign, verify (good + bad)")
    print("    - ECDSA keygen, sign, verify (good + bad)")
    print("    - Object listing and cleanup")

    slot = input("  Slot to use [0]: ").strip() or "0"
    confirm = input("  This will RE-INITIALIZE the token. Proceed? [y/N]: ").strip().lower()
    if confirm != "y":
        return

    results = []

    def check(name, output, expect_pass=True):
        passed = False
        if expect_pass:
            passed = "error" not in output.lower() and "failed" not in output.lower()
        else:
            passed = "invalid" in output.lower() or "failed" in output.lower()
        status = "\033[1;32mPASS\033[0m" if passed else "\033[1;31mFAIL\033[0m"
        results.append((name, passed))
        print(f"  [{status}] {name}")
        return passed

    # Token init
    print("\n\033[1m── 1. Token Init ──\033[0m")
    out = pkcs11_cmd(ser, f"--init-token --slot {slot} --label {st.token_label} --so-pin {st.so_pin}")
    check("Init token", out)

    out = pkcs11_cmd(ser, f"{st.so_login_args()} --init-pin --new-pin {st.user_pin}")
    check("Init user PIN", out)

    out = pkcs11_cmd(ser, "-T")
    check("Token visible", out)

    # RSA
    print("\n\033[1m── 2. RSA-2048 ──\033[0m")
    out = pkcs11_cmd(ser,
        f"{st.login_args()} --keypairgen --key-type RSA:2048 --label rsa --id 10 --usage-sign",
        timeout=30)
    check("RSA keygen", out)

    send_command(ser, 'echo "test rsa data" > /tmp/auto_rsa.txt', quiet=True)
    out = pkcs11_cmd(ser,
        f"{st.login_args()} --sign --mechanism SHA256-RSA-PKCS --input-file /tmp/auto_rsa.txt --output-file /tmp/auto_rsa_sig.bin --id 10")
    check("RSA sign", out)

    out = pkcs11_cmd(ser,
        f"{st.login_args()} --verify --mechanism SHA256-RSA-PKCS --input-file /tmp/auto_rsa.txt --signature-file /tmp/auto_rsa_sig.bin --id 10")
    check("RSA verify (good sig)", out)

    send_command(ser,
        'cp /tmp/auto_rsa_sig.bin /tmp/auto_rsa_sig_bad.bin && printf "\\xff" | dd of=/tmp/auto_rsa_sig_bad.bin bs=1 seek=0 count=1 conv=notrunc 2>/dev/null',
        quiet=True)
    out = pkcs11_cmd(ser,
        f"{st.login_args()} --verify --mechanism SHA256-RSA-PKCS --input-file /tmp/auto_rsa.txt --signature-file /tmp/auto_rsa_sig_bad.bin --id 10")
    check("RSA verify (bad sig → reject)", out, expect_pass=False)

    # ECDSA
    print("\n\033[1m── 3. ECDSA P-256 ──\033[0m")
    out = pkcs11_cmd(ser,
        f"{st.login_args()} --keypairgen --key-type EC:prime256v1 --label ec-p256 --id 20 --usage-sign",
        timeout=20)
    check("EC keygen", out)

    send_command(ser, 'echo "test ec data" > /tmp/auto_ec.txt', quiet=True)
    out = pkcs11_cmd(ser,
        f"{st.login_args()} --sign --mechanism ECDSA-SHA256 --input-file /tmp/auto_ec.txt --output-file /tmp/auto_ec_sig.bin --id 20")
    check("EC sign", out)

    out = pkcs11_cmd(ser,
        f"{st.login_args()} --verify --mechanism ECDSA-SHA256 --input-file /tmp/auto_ec.txt --signature-file /tmp/auto_ec_sig.bin --id 20")
    check("EC verify (good sig)", out)

    send_command(ser,
        'cp /tmp/auto_ec_sig.bin /tmp/auto_ec_sig_bad.bin && printf "\\xff" | dd of=/tmp/auto_ec_sig_bad.bin bs=1 seek=0 count=1 conv=notrunc 2>/dev/null',
        quiet=True)
    out = pkcs11_cmd(ser,
        f"{st.login_args()} --verify --mechanism ECDSA-SHA256 --input-file /tmp/auto_ec.txt --signature-file /tmp/auto_ec_sig_bad.bin --id 20")
    check("EC verify (bad sig → reject)", out, expect_pass=False)

    # List objects
    print("\n\033[1m── 4. List Objects ──\033[0m")
    out = pkcs11_cmd(ser, f"{st.login_args()} --list-objects")
    check("List objects", out)

    # Summary
    print("\n\033[1m═══════════════════════════════════════\033[0m")
    passed = sum(1 for _, p in results if p)
    total = len(results)
    color = "\033[1;32m" if passed == total else "\033[1;31m"
    print(f"  {color}Results: {passed}/{total} passed\033[0m")
    for name, p in results:
        status = "\033[32m✓\033[0m" if p else "\033[31m✗\033[0m"
        print(f"    {status} {name}")
    print()


def menu_raw(ser, st):
    """Send a raw command to the board."""
    cmd = input("  Command: ").strip()
    if cmd:
        send_command(ser, cmd)


# ── Main menu ────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="PKCS#11 test tool for Sparrow Hawk")
    parser.add_argument("--port", default="/dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=921600)
    parser.add_argument("--token-label", default=DEFAULT_TOKEN_LABEL)
    parser.add_argument("--so-pin", default=DEFAULT_SO_PIN)
    parser.add_argument("--user-pin", default=DEFAULT_USER_PIN)
    args = parser.parse_args()

    st = State()
    st.token_label = args.token_label
    st.so_pin = args.so_pin
    st.user_pin = args.user_pin

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.1)
    except serial.SerialException as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

    time.sleep(0.3)
    print(f"\033[1mConnected to {args.port} @ {args.baud} baud\033[0m")
    print(f"Token: {st.token_label}  |  User PIN: {st.user_pin}  |  SO PIN: {st.so_pin}\n")

    while True:
        print("\033[1;36m╔══════════════════════════════════════╗\033[0m")
        print("\033[1;36m║     PKCS#11 Test Tool — W77Q/OP-TEE ║\033[0m")
        print("\033[1;36m╚══════════════════════════════════════╝\033[0m")
        print("  1) Token Management")
        print("  2) RSA Operations")
        print("  3) ECDSA Operations")
        print("  4) Object Management")
        print("  5) PIN Counter / Lockout Test")
        print("  6) Persistence Test (reboot)")
        print("  7) \033[1;32mFull Automated Test\033[0m (runs everything)")
        print("  8) Raw command")
        print("  0) Exit")

        choice = input("\n\033[1mSelect: \033[0m").strip()

        if choice == "1":
            menu_token(ser, st)
        elif choice == "2":
            menu_rsa(ser, st)
        elif choice == "3":
            menu_ecdsa(ser, st)
        elif choice == "4":
            menu_objects(ser, st)
        elif choice == "5":
            menu_pin_test(ser, st)
        elif choice == "6":
            menu_persistence(ser, st)
        elif choice == "7":
            menu_quick_test(ser, st)
        elif choice == "8":
            menu_raw(ser, st)
        elif choice == "0":
            break

    ser.close()
    print("Disconnected.")


if __name__ == "__main__":
    main()
