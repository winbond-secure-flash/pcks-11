#!/usr/bin/env python3
"""
run_command.py - Execute a shell command N times and display all input/output.

Usage:
    python3 run_command.py -n <times> -- <command> [args...]
    python3 run_command.py -n 3 -- echo hello
    python3 run_command.py -n 2 -- ls -la /tmp
    python3 run_command.py -n 3 --no-edit -- echo hello   # skip editing
"""

import argparse
import os
import shlex
import subprocess
import sys
import time


def _termios_input_with_prefill(prompt: str, prefill: str) -> str:
    """
    Minimal line editor using termios raw mode — no readline, no tty module.
    Supports: printable chars, Backspace, Delete, Left/Right/Home/End, Ctrl-U.
    """
    import termios
    fd = sys.stdin.fileno()
    old_attrs = termios.tcgetattr(fd)

    # Inline tty.setraw — avoids dependency on the tty module
    new_attrs = termios.tcgetattr(fd)
    new_attrs[0] &= ~(termios.BRKINT | termios.ICRNL | termios.INPCK |
                      termios.ISTRIP | termios.IXON)
    new_attrs[1] &= ~termios.OPOST
    new_attrs[2]  = (new_attrs[2] & ~(termios.CSIZE | termios.PARENB)) | termios.CS8
    new_attrs[3] &= ~(termios.ECHO | termios.ICANON | termios.IEXTEN | termios.ISIG)
    new_attrs[6][termios.VMIN]  = 1
    new_attrs[6][termios.VTIME] = 0

    full_prompt = f"  {prompt}"
    plen = len(full_prompt)
    buf: list[str] = list(prefill)
    pos = len(buf)

    try:
        cols = os.get_terminal_size(fd).columns
    except OSError:
        cols = 80

    cursor_v   = [plen + pos]
    prev_end_v = [plen + len(buf)]

    def redraw() -> None:
        target_v  = plen + pos
        new_end_v = plen + len(buf)
        n_clear   = max(0, prev_end_v[0] - new_end_v)

        cur_row = cursor_v[0] // cols
        out = (f'\x1b[{cur_row}A' if cur_row > 0 else '') + '\r'
        out += full_prompt + ''.join(buf) + (' ' * n_clear)

        after_v    = new_end_v + n_clear
        rows_up    = after_v // cols - target_v // cols
        target_col = target_v % cols
        if rows_up > 0:
            out += f'\x1b[{rows_up}A'
        out += '\r'
        if target_col > 0:
            out += f'\x1b[{target_col}C'
        sys.stdout.write(out)
        sys.stdout.flush()
        cursor_v[0]   = target_v
        prev_end_v[0] = new_end_v

    sys.stdout.write(full_prompt + prefill)
    sys.stdout.flush()

    try:
        termios.tcsetattr(fd, termios.TCSAFLUSH, new_attrs)
        while True:
            ch = os.read(fd, 1)
            if ch in (b'\r', b'\n'):
                break
            elif ch == b'\x03':                        # Ctrl-C
                raise KeyboardInterrupt
            elif ch == b'\x15':                        # Ctrl-U — clear
                buf.clear()
                pos = 0
                redraw()
            elif ch in (b'\x7f', b'\x08'):             # Backspace
                if pos > 0:
                    buf.pop(pos - 1)
                    pos -= 1
                    redraw()
            elif ch == b'\x1b':                        # Escape sequence
                nxt = os.read(fd, 1)
                if nxt == b'[':                        # CSI
                    code = os.read(fd, 1)
                    if code == b'D':                   # Left
                        if pos > 0:
                            pos -= 1;   redraw()
                    elif code == b'C':                 # Right
                        if pos < len(buf):
                            pos += 1;   redraw()
                    elif code == b'H':                 # Home (CSI H)
                        pos = 0;        redraw()
                    elif code == b'F':                 # End  (CSI F)
                        pos = len(buf); redraw()
                    elif b'0' <= code <= b'9':         # Tilde sequences
                        seq = code
                        while True:
                            c = os.read(fd, 1)
                            seq += c
                            if c == b'~' or len(seq) > 12:
                                break
                        num = seq.split(b';')[0].rstrip(b'~')
                        if num in (b'1', b'7'):        # Home
                            pos = 0;        redraw()
                        elif num in (b'4', b'8'):      # End
                            pos = len(buf); redraw()
                        elif num == b'3':              # Delete
                            if pos < len(buf):
                                buf.pop(pos); redraw()
                elif nxt == b'O':                      # SS3
                    code = os.read(fd, 1)
                    if code == b'H':
                        pos = 0;        redraw()
                    elif code == b'F':
                        pos = len(buf); redraw()
            elif b' ' <= ch <= b'~':                   # Printable ASCII
                char = ch.decode()
                buf.insert(pos, char)
                pos += 1
                redraw()
    finally:
        end_v = plen + len(buf)
        if cursor_v[0] != end_v:
            end_row, end_col = end_v // cols, end_v % cols
            cur_row = cursor_v[0] // cols
            out = ''
            if end_row > cur_row:
                out += f'\x1b[{end_row - cur_row}B'
            elif end_row < cur_row:
                out += f'\x1b[{cur_row - end_row}A'
            out += '\r' + (f'\x1b[{end_col}C' if end_col else '')
            sys.stdout.write(out)
        termios.tcsetattr(fd, termios.TCSADRAIN, old_attrs)
        sys.stdout.write('\n')
        sys.stdout.flush()

    return ''.join(buf)


def input_with_prefill(prompt: str, prefill: str) -> str:
    """
    Prompt for a new value with *prefill* pre-loaded in the editing buffer.
    Uses a termios raw-mode line editor (no readline — safe on all boards).
    Falls back to a display-and-retype prompt when termios is unavailable.
    """
    try:
        val = _termios_input_with_prefill(prompt, prefill)
        return val if val.strip() else prefill
    except Exception:
        pass
    sys.stdout.write(f"  current: {prefill}\n")
    sys.stdout.write(f"  {prompt}(Enter=keep): ")
    sys.stdout.flush()
    import re
    val = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', sys.stdin.readline()).rstrip("\n")
    return val if val.strip() else prefill


def prompt_edit_command(cmd: list[str], run_index: int, total: int) -> list[str] | None:
    """
    Prompt the user to edit the command before a run.
    Returns the (possibly modified) command list, or None to skip the run.
    """
    print(f"\n  Edit command for run [{run_index}/{total}]  (Enter=keep, empty=skip, Ctrl-C=abort)")
    try:
        line = input_with_prefill("  cmd> ", shlex.join(cmd)).strip()
    except KeyboardInterrupt:
        print("\nAborted.")
        sys.exit(1)

    if line == "":
        print("  Skipping this run.")
        return None
    return shlex.split(line)


def run_command(cmd: list[str], run_index: int, total: int) -> int:
    """Run a single command and stream its output. Returns the exit code."""
    sep = "─" * 60
    print(f"\n{sep}")
    print(f"  Run [{run_index}/{total}]  Command: {' '.join(cmd)}")
    print(sep)

    start = time.monotonic()
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )

    for line in proc.stdout:
        print(line, end="")

    proc.wait()
    elapsed = time.monotonic() - start

    status = "✓ OK" if proc.returncode == 0 else f"✗ FAILED (exit {proc.returncode})"
    print(f"\n  {status}  ({elapsed:.3f}s)")
    return proc.returncode


def main():
    parser = argparse.ArgumentParser(
        description="Execute a shell command N times and show all output.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "-n", "--times",
        type=int,
        default=1,
        metavar="N",
        help="Number of times to run the command (default: 1)",
    )
    parser.add_argument(
        "-e", "--edit",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Interactively edit the command before each run (default: enabled, use --no-edit to disable)",
    )
    parser.add_argument(
        "--stop-on-error",
        action="store_true",
        help="Stop immediately if any run exits with a non-zero code",
    )
    parser.add_argument(
        "command",
        nargs=argparse.REMAINDER,
        help="Command to execute (use -- to separate from script options)",
    )

    args = parser.parse_args()

    # Strip leading '--' separator if present
    cmd = args.command
    if cmd and cmd[0] == "--":
        cmd = cmd[1:]

    if not cmd:
        parser.error("No command specified. Example: run_command.py -n 3 -- echo hello")

    if args.times < 1:
        parser.error("-n/--times must be >= 1")

    print(f"Command : {' '.join(cmd)}")
    print(f"Runs    : {args.times}")
    if args.edit:
        print("Mode    : interactive (command editable before each run)")

    exit_codes = []
    for i in range(1, args.times + 1):
        current_cmd = cmd
        if args.edit:
            current_cmd = prompt_edit_command(cmd, i, args.times)
            if current_cmd is None:       # user chose to skip
                continue
            cmd = current_cmd             # carry edited command to next run
        rc = run_command(current_cmd, i, args.times)
        exit_codes.append(rc)
        if args.stop_on_error and rc != 0:
            print(f"\n[stop-on-error] Aborting after run {i}.")
            break

    # Summary
    sep = "═" * 60
    print(f"\n{sep}")
    print(f"  SUMMARY  ({len(exit_codes)} of {args.times} runs executed)")
    print(sep)
    for idx, rc in enumerate(exit_codes, start=1):
        status = "✓" if rc == 0 else f"✗ exit={rc}"
        print(f"  Run {idx:>3}: {status}")

    failures = sum(1 for rc in exit_codes if rc != 0)
    print(f"\n  Passed: {len(exit_codes) - failures}  Failed: {failures}")
    sys.exit(0 if failures == 0 else 1)


if __name__ == "__main__":
    main()
