#!/usr/bin/env python3
"""
deploy-flash-bin.py — Flash flash.bin to Sparrow Hawk SPI NOR
==============================================================
Handles all board states automatically:
  • Board already running Linux  → reboot, catch U-Boot autoboot
  • Board at U-Boot prompt       → use directly
  • Board powered off            → wait for power-on, spam Space

After catching U-Boot it:
  1. Loads flash.bin via Y-Modem (loady + sb --ymodem)
  2. Erases SPI flash
  3. Writes flash.bin to SPI flash
  4. Resets the board

Usage:
  python3 scripts/deploy-flash-bin.py
  python3 scripts/deploy-flash-bin.py --flash path/to/flash.bin
  python3 scripts/deploy-flash-bin.py --port /dev/ttyUSB1
  python3 scripts/deploy-flash-bin.py --no-verify

Requirements:
  pip install pyserial
  apt  install lrzsz   # provides sb (Y-Modem sender)
"""

import argparse
import os
import subprocess
import sys
import time

import serial

# ──────────────────────────────────────────────────────────────────────────────
# Defaults
# ──────────────────────────────────────────────────────────────────────────────
PORT        = '/dev/ttyUSB0'
BAUD        = 921600
LOAD_ADDR   = '0x58000000'
ERASE_SIZE  = '0x230000'
BOOT_TIMEOUT = 120   # seconds to wait for board output after power-on

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_FLASH_BIN = os.path.join(
    BASE,
    'build/build-sparrow-hawk/tmp/deploy/images/sparrow-hawk/flash.bin',
)


# ──────────────────────────────────────────────────────────────────────────────
# Helpers
# ──────────────────────────────────────────────────────────────────────────────

def info(msg):  print(f'\033[1;36m[flash]\033[0m {msg}', flush=True)
def ok(msg):    print(f'\033[1;32m[ ok  ]\033[0m {msg}', flush=True)
def warn(msg):  print(f'\033[1;33m[warn ]\033[0m {msg}', flush=True)
def die(msg):   print(f'\033[1;31m[FAIL ]\033[0m {msg}', flush=True); sys.exit(1)


def kill_port_holders(port):
    r = subprocess.run(['lsof', port], capture_output=True, text=True)
    for line in r.stdout.splitlines()[1:]:
        parts = line.split()
        if len(parts) > 1 and parts[1].isdigit():
            subprocess.run(['kill', parts[1]], check=False)
    if r.stdout.strip():
        time.sleep(1)


def open_port(port, baud):
    kill_port_holders(port)
    for attempt in range(1, 7):
        try:
            s = serial.Serial(
                port, baud, timeout=0.1,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                xonxoff=False, rtscts=False, dsrdtr=False,
            )
            s.dtr = False
            s.rts = False
            return s
        except serial.SerialException as e:
            warn(f'open attempt {attempt}/6 failed: {e}')
            time.sleep(2)
    die(f'Cannot open {port}')


def read_for(s, seconds):
    """Read all available bytes for `seconds`, return accumulated bytes."""
    buf = b''
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            data = s.read(256)
        except serial.SerialException:
            break
        if data:
            buf += data
    return buf


def wait_prompt(s, prompt=b'=>', timeout=30):
    """Read until `prompt` appears; return accumulated buffer or None."""
    buf = b''
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            data = s.read(256)
        except serial.SerialException:
            break
        if data:
            buf += data
            sys.stdout.write(data.decode('ascii', errors='replace'))
            sys.stdout.flush()
        if prompt in buf:
            return buf
    return None


def uboot_cmd(s, cmd, success=b'=>', timeout=60):
    """Send a U-Boot command, stream output, return buffer when prompt returns."""
    s.reset_input_buffer()
    s.write(cmd.encode() + b'\r')
    buf = b''
    deadline = time.time() + timeout
    while time.time() < deadline:
        data = s.read(256)
        if data:
            buf += data
            sys.stdout.write(data.decode('ascii', errors='replace'))
            sys.stdout.flush()
        if success in buf[len(cmd):]:
            return buf
    return buf


# ──────────────────────────────────────────────────────────────────────────────
# Board state detection
# ──────────────────────────────────────────────────────────────────────────────

def probe_board_state(s):
    """
    Probe the board state by sending CR and reading the response.
    Tries multiple times to handle a quiet login prompt or U-Boot idle.
    Returns: ('linux'|'uboot'|'silent', buf)
    """
    time.sleep(0.3)  # let the port settle after open
    for attempt in range(4):
        s.reset_input_buffer()
        s.write(b'\r\n')
        buf = read_for(s, 2.0)
        if b'login:' in buf or b'# ' in buf or b'$ ' in buf:
            return 'linux', buf
        if b'=>' in buf:
            return 'uboot', buf
        time.sleep(0.5)

    # Last attempt: try a U-Boot command in case it is at => with no echo
    s.reset_input_buffer()
    s.write(b'version\r')
    buf = read_for(s, 2.0)
    if b'U-Boot' in buf:
        return 'uboot', buf

    return 'silent', b''


# ──────────────────────────────────────────────────────────────────────────────
# Catch U-Boot autoboot
# ──────────────────────────────────────────────────────────────────────────────

def catch_uboot_from_linux(s):
    """Log in as root, issue reboot, intercept U-Boot autoboot."""
    info('Board is running Linux — logging in and rebooting...')

    # Ensure we have a shell prompt
    s.reset_input_buffer()
    s.write(b'\r')
    buf = read_for(s, 1)
    if b'login:' in buf:
        s.write(b'root\r')
        buf2 = read_for(s, 3)
        # Consume motd / warning banner
        if b'#' not in buf2:
            time.sleep(2)
            read_for(s, 2)

    s.reset_input_buffer()
    s.write(b'\r')
    time.sleep(0.5)
    s.read(256)  # consume prompt

    info('Sending reboot...')
    s.write(b'reboot\r')
    s = _spam_and_catch(s, timeout=180)
    return s


def catch_uboot_from_poweron(s, timeout):
    """Wait for any output from the board, then spam Space to stop autoboot."""
    info(f'Waiting for board output (timeout {timeout}s) — power on the board now...')
    deadline = time.time() + timeout
    while time.time() < deadline:
        data = s.read(64)
        if data:
            sys.stdout.write(data.decode('ascii', errors='replace'))
            sys.stdout.flush()
            info('Board is alive — spamming Space...')
            s = _spam_and_catch(s, timeout=60)
            return s
    die('No board output received. Check power and serial connection.')


def _spam_and_catch(s, timeout=180):
    """
    Read board output while spamming Space; return when U-Boot => prompt appears.

    Behaviour:
      - Silently reads output until U-Boot SPL/U-Boot header is seen, then
        intensifies spamming.
      - Resets the idle timer each time new output arrives, so a slow shutdown
        or watchdog delay does not cause a premature timeout.
      - Handles brief FTDI USB disconnects (common on Renesas board reset) by
        reopening the port transparently.
      - Dies if no new output for `idle_limit` seconds.
    """
    port = s.port
    baud = s.baudrate
    buf = b''
    saw_uboot = False
    last_activity = time.time()
    idle_limit = timeout

    while True:
        # ── Read ──────────────────────────────────────────────────────────────
        try:
            data = s.read(64)
        except serial.SerialException:
            # FTDI glitch on board reset — close and reopen
            warn('Serial port glitch (board reset?) — reconnecting...')
            try:
                s.close()
            except Exception:
                pass
            time.sleep(1.5)
            for attempt in range(10):
                try:
                    s = serial.Serial(
                        port, baud, timeout=0.1,
                        bytesize=serial.EIGHTBITS,
                        parity=serial.PARITY_NONE,
                        stopbits=serial.STOPBITS_ONE,
                        xonxoff=False, rtscts=False, dsrdtr=False,
                    )
                    s.dtr = False
                    s.rts = False
                    info('Reconnected.')
                    last_activity = time.time()  # reset idle timer
                    break
                except serial.SerialException:
                    time.sleep(1)
            else:
                die('Cannot reconnect to serial port after board reset.')
            continue

        if data:
            buf += data
            last_activity = time.time()
            sys.stdout.write(data.decode('ascii', errors='replace'))
            sys.stdout.flush()

        # ── Detect U-Boot and adjust spam intensity ────────────────────────
        if b'U-Boot' in buf and not saw_uboot:
            saw_uboot = True
            info('U-Boot detected — spamming Space aggressively...')
            idle_limit = 30  # need prompt within 30 s of last output

        # ── Spam ──────────────────────────────────────────────────────────────
        try:
            if saw_uboot or b'autoboot' in buf:
                s.write(b' ')
            else:
                time.sleep(0.1)
                s.write(b' ')
        except serial.SerialException:
            pass  # write failures during glitch are OK; read loop will reconnect

        # ── Check for success ─────────────────────────────────────────────────
        if b'=>' in buf:
            ok('U-Boot prompt caught.')
            return s  # return (possibly reopened) serial object

        if time.time() - last_activity > idle_limit:
            die(f'No board output for {idle_limit}s — autoboot not caught.\n'
                '  Tip: check serial connection or try power-cycling manually.')


# ──────────────────────────────────────────────────────────────────────────────
# SPI flash programming
# ──────────────────────────────────────────────────────────────────────────────

def program_spi(s, flash_bin, port, baud, load_addr, erase_size):
    # Enter loady and hand off to sb
    info(f'Starting Y-Modem receive: loady {load_addr}')
    s.reset_input_buffer()
    s.write(b'\r')
    wait_prompt(s, b'=>', timeout=10)

    s.reset_input_buffer()
    s.write(f'loady {load_addr}\r'.encode())
    # Wait for Y-Modem ready indicator (0x43 = 'C' repeated)
    buf = b''
    deadline = time.time() + 30
    ready = False
    while time.time() < deadline:
        data = s.read(256)
        if data:
            buf += data
            sys.stdout.write(data.decode('ascii', errors='replace'))
            sys.stdout.flush()
        if b'Ready for binary' in buf or b'\x43\x43' in buf or b'CCC' in buf:
            ready = True
            break
    if not ready:
        die('loady did not enter Y-Modem receive mode.')

    ok('U-Boot ready for Y-Modem transfer.')
    s.close()

    info(f'Sending {flash_bin} via sb --ymodem ...')
    r = subprocess.run(
        f'sb --ymodem "{flash_bin}" > {port} < {port}',
        shell=True,
    )
    if r.returncode != 0:
        die(f'sb --ymodem failed (exit {r.returncode}). Is lrzsz installed?')
    ok('Y-Modem transfer complete.')

    # Reopen port for SPI programming
    time.sleep(0.5)
    s2 = open_port(port, baud)

    info('Probing SPI flash...')
    out = uboot_cmd(s2, 'sf probe', success=b'SF:', timeout=15)
    if b'SF:' not in out:
        s2.close()
        die('sf probe failed.')
    ok('SPI flash probed.')

    info(f'Erasing {erase_size} bytes at offset 0...')
    out = uboot_cmd(s2, f'sf erase 0 {erase_size}', success=b'Erased: OK', timeout=60)
    if b'Erased: OK' not in out:
        s2.close()
        die('sf erase failed.')
    ok('Erase complete.')

    info('Writing flash.bin to SPI flash...')
    out = uboot_cmd(s2, f'sf write {load_addr} 0 $filesize',
                    success=b'Written: OK', timeout=60)
    if b'Written: OK' not in out:
        s2.close()
        die('sf write failed.')
    ok('Write complete.')

    # Read back filesize for reporting
    s2.reset_input_buffer()
    s2.write(b'printenv filesize\r')
    time.sleep(0.5)
    env_out = read_for(s2, 1).decode('ascii', errors='replace')
    for line in env_out.splitlines():
        if 'filesize=' in line:
            hexval = line.split('=')[1].strip()
            try:
                nbytes = int(hexval, 16)
                ok(f'Written {nbytes:,} bytes ({nbytes / 1024:.1f} KB).')
            except ValueError:
                pass
            break

    info('Resetting board...')
    s2.write(b'reset\r')
    time.sleep(0.5)
    s2.close()
    ok('Board reset.')


# ──────────────────────────────────────────────────────────────────────────────
# Verify: wait for Linux login
# ──────────────────────────────────────────────────────────────────────────────

def verify_boot(port, baud, timeout=120):
    info(f'Waiting for Linux login prompt (timeout {timeout}s)...')
    s = open_port(port, baud)
    buf = b''
    deadline = time.time() + timeout
    while time.time() < deadline:
        data = s.read(256)
        if data:
            buf += data
            sys.stdout.write(data.decode('ascii', errors='replace'))
            sys.stdout.flush()
        if b'login:' in buf:
            break

    if b'login:' not in buf:
        s.close()
        warn('Login prompt not seen in boot log — board may still be booting.')
        return

    # Log in and check OP-TEE
    info('Logging in to verify OP-TEE...')
    s.write(b'root\r')
    time.sleep(2)
    read_for(s, 2)
    s.write(b'\r')
    time.sleep(0.5)
    s.read(256)

    s.reset_input_buffer()
    s.write(b'dmesg | grep -i optee\r')
    time.sleep(3)
    dmesg = read_for(s, 2).decode('ascii', errors='replace')
    s.close()

    print()
    if 'initialized driver' in dmesg:
        ok('OP-TEE driver initialized — flash successful!')
    elif 'optee' in dmesg.lower():
        warn('OP-TEE present but "initialized driver" not found — check boot log.')
    else:
        warn('OP-TEE not found in dmesg. Boot log above may have more detail.')


# ──────────────────────────────────────────────────────────────────────────────
# Main
# ──────────────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description='Flash flash.bin to Sparrow Hawk SPI NOR flash.',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument('--flash',      default=DEFAULT_FLASH_BIN,
                        metavar='FILE', help='Path to flash.bin')
    parser.add_argument('--port',       default=PORT,
                        help=f'Serial port (default: {PORT})')
    parser.add_argument('--baud',       default=BAUD, type=int,
                        help=f'Baud rate (default: {BAUD})')
    parser.add_argument('--load-addr',  default=LOAD_ADDR,
                        help=f'Y-Modem load address (default: {LOAD_ADDR})')
    parser.add_argument('--erase-size', default=ERASE_SIZE,
                        help=f'SPI erase size in hex (default: {ERASE_SIZE})')
    parser.add_argument('--boot-timeout', default=BOOT_TIMEOUT, type=int,
                        help=f'Seconds to wait for board output (default: {BOOT_TIMEOUT})')
    parser.add_argument('--no-verify',  action='store_true',
                        help='Skip post-flash boot verification')
    args = parser.parse_args()

    flash_bin = os.path.abspath(args.flash)
    if not os.path.isfile(flash_bin):
        die(f'flash.bin not found: {flash_bin}\n'
            '  Run:  source build/poky/oe-init-build-env build/build-sparrow-hawk\n'
            '        bitbake -c cleansstate u-boot && bitbake u-boot')

    size = os.path.getsize(flash_bin)
    info(f'flash.bin: {flash_bin}  ({size:,} bytes / {size/1024:.1f} KB)')
    info(f'Port: {args.port}  Baud: {args.baud}')

    # Check sb is available
    if subprocess.run(['which', 'sb'], capture_output=True).returncode != 0:
        die('sb not found — install lrzsz:  sudo apt install lrzsz')

    # ── Phase 1: Detect board state and get to U-Boot prompt ──────────────────
    s = open_port(args.port, args.baud)
    state, buf = probe_board_state(s)
    info(f'Board state: {state}')

    if state == 'uboot':
        ok('Already at U-Boot prompt.')
    elif state == 'linux':
        s = catch_uboot_from_linux(s)
    else:
        s = catch_uboot_from_poweron(s, args.boot_timeout)

    # ── Phase 2: Program SPI flash ────────────────────────────────────────────
    program_spi(s, flash_bin, args.port, args.baud, args.load_addr, args.erase_size)

    # ── Phase 3: Verify (optional) ────────────────────────────────────────────
    if not args.no_verify:
        verify_boot(args.port, args.baud, timeout=args.boot_timeout)

    print()
    ok('deploy-flash-bin complete.')


if __name__ == '__main__':
    main()
