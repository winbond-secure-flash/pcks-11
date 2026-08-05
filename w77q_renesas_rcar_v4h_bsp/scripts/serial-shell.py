#!/usr/bin/env python3
"""
Send commands to the board over serial and print the output.

Usage:
    # Single command:
    ./scripts/serial-shell.py 'pkcs11-tool --module=/usr/lib/libckteec.so -L'

    # Multiple commands:
    ./scripts/serial-shell.py 'echo hello' 'ls /tmp' 'uname -a'

    # From a script file (one command per line):
    ./scripts/serial-shell.py -f myscript.sh

    # Interactive mode (type commands, Enter to send):
    ./scripts/serial-shell.py -i

Options:
    --port     Serial port (default: /dev/ttyUSB0)
    --baud     Baud rate (default: 921600)
    --timeout  Seconds to wait for output after each command (default: 10)
"""

import argparse
import sys
import time

import serial


def parse_args():
    p = argparse.ArgumentParser(description="Send commands to board over serial")
    p.add_argument("commands", nargs="*", help="Commands to run on the board")
    p.add_argument("-f", "--file", help="Read commands from file (one per line)")
    p.add_argument("-i", "--interactive", action="store_true",
                   help="Interactive mode: type commands, Enter to send")
    p.add_argument("--port", default="/dev/ttyUSB0")
    p.add_argument("--baud", type=int, default=921600)
    p.add_argument("--timeout", type=float, default=10,
                   help="Seconds to wait for command output (default: 10)")
    return p.parse_args()


def read_until_prompt(ser, timeout=10):
    """Read serial output until we see a shell prompt or timeout."""
    output = ""
    deadline = time.time() + timeout
    idle_start = None

    while time.time() < deadline:
        n = ser.in_waiting
        if n:
            chunk = ser.read(n).decode(errors='replace')
            output += chunk
            idle_start = None
            # Check for shell prompt at end
            stripped = output.rstrip()
            if stripped.endswith('# ') or stripped.endswith('$ ') or stripped.endswith('=> '):
                break
        else:
            if idle_start is None:
                idle_start = time.time()
            elif time.time() - idle_start > 1.0:
                # 1 second of no output after getting some — command done
                if output.strip():
                    break
            time.sleep(0.02)

    return output


def send_command(ser, cmd, timeout=10):
    """Send a single command and return its output."""
    # Flush any pending input
    ser.reset_input_buffer()

    # Send command byte-by-byte with pacing
    for byte in (cmd + '\n').encode():
        ser.write(bytes([byte]))
        time.sleep(0.002)
    ser.flush()

    # Read response
    output = read_until_prompt(ser, timeout)

    # Clean up: remove \r, split into lines
    output = output.replace('\r', '')
    lines = [l for l in output.split('\n') if l.strip()]

    # Remove echoed command (board echoes what we sent)
    cmd_words = cmd.split()[:3]  # first 3 words to match
    filtered = []
    for line in lines:
        # Skip lines that look like the echoed command
        if cmd_words and cmd_words[0] in line and cmd_words[-1] in line:
            continue
        # Skip prompt lines
        if line.strip().endswith('#') or line.strip().endswith('$'):
            continue
        filtered.append(line)

    return '\n'.join(filtered)


def run_interactive(ser, timeout):
    """Interactive mode — read commands from stdin, send to board."""
    print(f"Connected to {ser.port} @ {ser.baudrate} baud")
    print("Type commands and press Enter. Ctrl+D or 'exit' to quit.")
    print("-" * 50)

    # Get initial prompt
    ser.write(b'\n')
    time.sleep(0.3)
    initial = ser.read(ser.in_waiting or 1).decode(errors='replace')
    # Show prompt
    sys.stdout.write(initial)
    sys.stdout.flush()

    while True:
        try:
            # Python input() handles full line including paste
            cmd = input()
        except (EOFError, KeyboardInterrupt):
            break

        if cmd.strip() == 'exit':
            break

        # Send and show output
        ser.reset_input_buffer()
        for byte in (cmd + '\n').encode():
            ser.write(bytes([byte]))
            time.sleep(0.002)
        ser.flush()

        # Print output as it arrives
        output = read_until_prompt(ser, timeout)
        sys.stdout.write(output)
        sys.stdout.flush()

    print("\nDisconnected.")


def main():
    args = parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.1)
    except serial.SerialException as e:
        print(f"Error opening {args.port}: {e}", file=sys.stderr)
        sys.exit(1)

    time.sleep(0.3)

    # Gather commands
    commands = []
    if args.file:
        with open(args.file) as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith('#'):
                    commands.append(line)
    elif args.commands:
        # Join all positional args into one string, then split by
        # actual command boundaries. This handles terminal paste
        # wrapping where one command gets split across multiple args.
        raw = ' '.join(args.commands)
        # Collapse whitespace
        commands = [' '.join(raw.split())]
    elif args.interactive:
        run_interactive(ser, args.timeout)
        ser.close()
        return
    else:
        # No args — interactive mode
        run_interactive(ser, args.timeout)
        ser.close()
        return

    # Execute commands — join any newlines from terminal paste wrapping
    for cmd in commands:
        cmd = ' '.join(cmd.split())  # collapse all whitespace into single spaces
        print(f"\033[1;34m>>> {cmd}\033[0m")
        output = send_command(ser, cmd, args.timeout)
        if output.strip():
            print(output)

    ser.close()


if __name__ == "__main__":
    main()
