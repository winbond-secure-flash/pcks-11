#!/usr/bin/env python3
"""w77q_fs_test_menu.py — Interactive W77Q flash filesystem test suite.

Mirrors the structure of pkcs-test-menu.py for the w77q_fs secure-storage
backend running inside OP-TEE on the W77Q SPI NOR flash section.

Deploy as /usr/bin/w77q_fs_test_menu.py on the Sparrow Hawk board.
Usage: w77q_fs_test_menu.py [--auto]
  --auto : run all tests non-interactively

Requires: tee-supplicant running, w77q-fs-test (or w77q_fs_test.py) deployed
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path

# ── ANSI colours ──────────────────────────────────────────────────────────────
RED    = '\033[0;31m'
GREEN  = '\033[0;32m'
YELLOW = '\033[1;33m'
CYAN   = '\033[0;36m'
BOLD   = '\033[1m'
NC     = '\033[0m'

# ── Stats & log ───────────────────────────────────────────────────────────────
pass_count = fail_count = skip_count = 0
logfile_path = f"/tmp/w77q-fs-test-{datetime.now().strftime('%Y%m%d-%H%M%S')}.log"


def _log(msg: str):
    with open(logfile_path, "a") as f:
        f.write(msg + "\n")


def ok(msg: str):
    global pass_count
    pass_count += 1
    print(f"  {GREEN}[PASS]{NC} {msg}")
    _log(f"[PASS] {msg}")


def no(msg: str):
    global fail_count
    fail_count += 1
    print(f"  {RED}[FAIL]{NC} {msg}")
    _log(f"[FAIL] {msg}")


def sk(msg: str):
    global skip_count
    skip_count += 1
    print(f"  {YELLOW}[SKIP]{NC} {msg}")
    _log(f"[SKIP] {msg}")


def hdr(msg: str):
    print(f"\n{BOLD}{CYAN}══ {msg} ══{NC}")
    _log(f"\n══ {msg} ══")


def _grep(pattern: str, text: str) -> bool:
    return bool(re.search(pattern, text, re.IGNORECASE))


# ── Command-edit helper (termios, no readline, no tty module) ─────────────────
def _termios_input_with_prefill(prompt: str, prefill: str) -> str:
    import termios
    fd = sys.stdin.fileno()
    old_attrs = termios.tcgetattr(fd)

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
        cur_row   = cursor_v[0] // cols
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
            elif ch == b'\x03':
                raise KeyboardInterrupt
            elif ch == b'\x15':
                buf.clear(); pos = 0; redraw()
            elif ch in (b'\x7f', b'\x08'):
                if pos > 0:
                    buf.pop(pos - 1); pos -= 1; redraw()
            elif ch == b'\x1b':
                nxt = os.read(fd, 1)
                if nxt == b'[':
                    code = os.read(fd, 1)
                    if code == b'D':
                        if pos > 0: pos -= 1; redraw()
                    elif code == b'C':
                        if pos < len(buf): pos += 1; redraw()
                    elif code == b'H':
                        pos = 0; redraw()
                    elif code == b'F':
                        pos = len(buf); redraw()
                    elif b'0' <= code <= b'9':
                        seq = code
                        while True:
                            c = os.read(fd, 1); seq += c
                            if c == b'~' or len(seq) > 12: break
                        num = seq.split(b';')[0].rstrip(b'~')
                        if num in (b'1', b'7'):   pos = 0;        redraw()
                        elif num in (b'4', b'8'): pos = len(buf); redraw()
                        elif num == b'3':
                            if pos < len(buf): buf.pop(pos); redraw()
                elif nxt == b'O':
                    code = os.read(fd, 1)
                    if code == b'H':   pos = 0;        redraw()
                    elif code == b'F': pos = len(buf); redraw()
            elif b' ' <= ch <= b'~':
                buf.insert(pos, ch.decode()); pos += 1; redraw()
    finally:
        end_v = plen + len(buf)
        if cursor_v[0] != end_v:
            end_row, end_col = end_v // cols, end_v % cols
            cur_row = cursor_v[0] // cols
            out = ''
            if end_row > cur_row:   out += f'\x1b[{end_row - cur_row}B'
            elif end_row < cur_row: out += f'\x1b[{cur_row - end_row}A'
            out += '\r' + (f'\x1b[{end_col}C' if end_col else '')
            sys.stdout.write(out)
        termios.tcsetattr(fd, termios.TCSADRAIN, old_attrs)
        sys.stdout.write('\n')
        sys.stdout.flush()

    return ''.join(buf)


def input_with_prefill(prompt: str, prefill: str) -> str:
    try:
        val = _termios_input_with_prefill(prompt, prefill)
        return val if val.strip() else prefill
    except Exception:
        pass
    sys.stdout.write(f"  {CYAN}current:{NC} {prefill}\n")
    sys.stdout.write(f"  {prompt}(Enter=keep): ")
    sys.stdout.flush()
    val = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', sys.stdin.readline()).rstrip("\n")
    return val if val.strip() else prefill


def _prompt(msg: str) -> str:
    sys.stdout.write(msg)
    sys.stdout.flush()
    try:
        return sys.stdin.readline().rstrip("\n")
    except (EOFError, KeyboardInterrupt):
        raise KeyboardInterrupt


# ── Runtime globals ───────────────────────────────────────────────────────────
AUTO = False

# Prefer the binary; fall back to the Python script
_W77Q_FS_CMD = (
    "w77q-fs-test" if shutil.which("w77q-fs-test")
    else "python3 /usr/bin/w77q_fs_test.py"
)
# Diag commands are only in the Python script (C binary doesn't expose them)
_DIAG_CMD = "python3 /usr/bin/w77q_fs_test.py"

TC_NAMES = ["basic", "update", "rename", "enum", "large", "overwrite", "truncate"]

TC_DESC = {
    "basic":     "create / read-back / delete",
    "update":    "write dataA, reopen, overwrite dataB, verify",
    "rename":    "rename object; old ID gone, new ID readable",
    "enum":      "create 10 objects, enumerate, verify count",
    "large":     "8 KB pattern payload — write and full byte-verify",
    "overwrite": "TEE_DATA_FLAG_OVERWRITE replaces existing object",
    "truncate":  "truncate to half length; verify size & content",
}


# ── Command runner ────────────────────────────────────────────────────────────
def run_cmd(cmd_str: str) -> tuple[str, int]:
    print(f"\n  {CYAN}▶ {NC}{BOLD}{cmd_str}{NC}", flush=True)
    _log(f"  CMD: {cmd_str}")

    if not AUTO:
        print(f"  {BOLD}[Enter]{NC}=run  {BOLD}[e]{NC}=edit  {BOLD}[s]{NC}=skip  ",
              end="", flush=True)
        try:
            ans = _prompt("» ").strip().lower()
        except KeyboardInterrupt:
            print("\nAborted.")
            sys.exit(1)

        if ans == "s":
            print(f"  {YELLOW}⏭  skipped by user{NC}")
            _log("  OUT: (skipped)")
            return "(skipped)", 0

        if ans == "e":
            try:
                edited = input_with_prefill("Edit » ", cmd_str).strip()
                if edited:
                    cmd_str = edited
                    print(f"  {CYAN}▶ (edited){NC} {BOLD}{cmd_str}{NC}")
                    _log(f"  CMD(edited): {cmd_str}")
            except (EOFError, KeyboardInterrupt):
                pass

    out_f = f"/tmp/.w77q_{os.getpid()}.out"
    rc_f  = f"/tmp/.w77q_{os.getpid()}.rc"
    subprocess.run(
        f'{{ {cmd_str}; _rc=$?; echo $_rc >"{rc_f}"; }} 2>&1'
        f' | tee "{out_f}" | sed "s/^/  /"',
        shell=True,
    )
    output = ""
    try:
        output = Path(out_f).read_text(errors="replace")
    except OSError:
        pass
    rc = 1
    try:
        rc = int(Path(rc_f).read_text().strip())
    except (OSError, ValueError):
        pass
    Path(out_f).unlink(missing_ok=True)
    Path(rc_f).unlink(missing_ok=True)
    _log(f"  OUT: {output.rstrip()}")
    return output, rc


# ── Flash state helpers ───────────────────────────────────────────────────────
_W77Q_FS_TA_UUID = "f0e1d2c3-b4a5-9687-8796-a5b4c3d2e1f0"
_PKCS11_TA_UUID  = "dac902fd-6c30-c748-a49c-bbd827ae86ee"


def _oid_printable(raw: str) -> str:
    return "".join(c if 32 <= ord(c) < 127 else "." for c in raw)


def _ta_short(uuid: str) -> str:
    if uuid == _W77Q_FS_TA_UUID:                          return "w77q-fs"
    if uuid == _PKCS11_TA_UUID:                           return "PKCS#11"
    if uuid == "00000000-0000-0000-0000-000000000000":    return "system"
    return uuid[:7]


def _w77q_parse_objects(text: str) -> list[dict]:
    objects = []
    for line in text.splitlines():
        m = re.search(
            r'\[(\d+)\]\s+ta_uuid=(\S+)\s+flash_off=(0x[\da-fA-F]+)'
            r'\s+data_size=\s*(\d+)\s+obj_id=(.*)',
            line,
        )
        if m:
            objects.append({
                "idx":     int(m.group(1)),
                "ta_uuid": m.group(2),
                "offset":  m.group(3),
                "size":    int(m.group(4)),
                "obj_id":  m.group(5).strip(),
            })
    return objects


def _w77q_read_raw_bytes(offset: str, size: int) -> bytes:
    """Read raw flash bytes for an object; return b'' on failure."""
    cap = min(size, 256)
    r = subprocess.run(
        ["w77q-dump", "read-raw", offset, str(cap)],
        capture_output=True, timeout=10,
    )
    # Parse hex from output lines like: "0000  4a 42 ...  |JBOW|"
    raw = bytearray()
    for line in r.stdout.splitlines():
        line = line.decode(errors="replace") if isinstance(line, bytes) else line
        if line.startswith("["):
            continue
        # e.g.  "0000  4a 42 4f 57 ..."
        m = re.match(r'\s*[0-9a-fA-F]{4}\s+((?:[0-9a-fA-F]{2}\s+)+)', line)
        if m:
            for h in m.group(1).split():
                try:
                    raw.append(int(h, 16))
                except ValueError:
                    pass
    return bytes(raw)


def _fmt_hex_lines(data: bytes, indent: str = "    ") -> list[str]:
    """Format bytes as hex+ASCII lines (16 bytes per row)."""
    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        hex_part = " ".join(f"{b:02x}" for b in chunk)
        # Two groups of 8 with a gap
        if len(chunk) > 8:
            hex_part = hex_part[:23] + "  " + hex_part[23:]
        asc_part = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"{indent}{i:04x}  {hex_part:<49}  |{asc_part}|")
    return lines


def _w77q_snap_full(label: str) -> dict:
    """
    Run w77q-dump list-all, print LUT table + hex dump of every object.
    Returns: {"objects": list[dict], "raw": {offset_str: bytes}}
    """
    if not shutil.which("w77q-dump"):
        return {"objects": [], "raw": {}}

    r = subprocess.run(["w77q-dump", "list-all"],
                       capture_output=True, text=True, errors="replace", timeout=15)
    objects = _w77q_parse_objects(r.stdout)
    first   = next((l.strip() for l in r.stdout.splitlines() if l.strip()), "")

    print(f"\n  {CYAN}{'─'*58}{NC}")
    print(f"  {CYAN}  FLASH DUMP [{label.upper()}]   {first}{NC}")
    print(f"  {CYAN}{'─'*58}{NC}")

    if not objects:
        print(f"  {YELLOW}  (no objects){NC}")
        return {"objects": [], "raw": {}}

    # LUT table
    print(f"  {'IDX':>3}  {'OFFSET':>10}  {'SIZE':>6}  {'TA':>7}  OBJ-ID")
    print(f"  {'─'*3}  {'─'*10}  {'─'*6}  {'─'*7}  {'─'*36}")
    for o in objects:
        print(f"  {o['idx']:>3}  {o['offset']:>10}  {o['size']:>6}"
              f"  {_ta_short(o['ta_uuid']):>7}  {_oid_printable(o['obj_id'])}")

    # Hex dumps
    raw_map: dict[str, bytes] = {}
    print()
    for o in objects:
        raw = _w77q_read_raw_bytes(o["offset"], o["size"])
        raw_map[o["offset"]] = raw
        cap = min(o["size"], 256)
        suffix = f"  (+{o['size']-cap} more bytes)" if o["size"] > cap else ""
        print(f"  [{o['idx']:>3}] {_oid_printable(o['obj_id']):<36}"
              f"  {o['offset']}  {cap}/{o['size']} B{suffix}")
        for line in _fmt_hex_lines(raw):
            print(line)

    return {"objects": objects, "raw": raw_map}


def _w77q_compare(before: dict, after: dict):
    """
    Compare two full flash snapshots (from _w77q_snap_full).
    Shows:  + added objects (with hex dump)
            - removed objects
            ~ changed objects (same offset, different bytes — side-by-side diff)
            = unchanged count
    """
    b_objs = {o["offset"]: o for o in before["objects"]}
    a_objs = {o["offset"]: o for o in after["objects"]}
    b_raw  = before["raw"]
    a_raw  = after["raw"]

    added   = [a_objs[k] for k in a_objs if k not in b_objs]
    removed = [b_objs[k] for k in b_objs if k not in a_objs]
    common  = [k for k in b_objs if k in a_objs]
    changed = [k for k in common if b_raw.get(k) != a_raw.get(k)]
    unchanged = len(common) - len(changed)

    print(f"\n  {CYAN}{'═'*58}{NC}")
    print(f"  {CYAN}  DIFF  before → after{NC}")
    print(f"  {CYAN}{'═'*58}{NC}")
    print(f"  {GREEN}+{NC} added:     {len(added):>3}   "
          f"{RED}-{NC} removed:   {len(removed):>3}   "
          f"{YELLOW}~{NC} changed:   {len(changed):>3}   "
          f"= unchanged: {unchanged:>3}")

    # ── Removed ──
    if removed:
        print(f"\n  {RED}── REMOVED ──{NC}")
        for o in sorted(removed, key=lambda x: x["offset"]):
            print(f"  {RED}  - [{o['idx']:>3}] {_oid_printable(o['obj_id'])}"
                  f"  {o['offset']}  {o['size']} B{NC}")

    # ── Added ──
    if added:
        print(f"\n  {GREEN}── ADDED ──{NC}")
        for o in sorted(added, key=lambda x: x["offset"]):
            print(f"  {GREEN}  + [{o['idx']:>3}] {_oid_printable(o['obj_id'])}"
                  f"  {o['offset']}  {o['size']} B{NC}")
            for line in _fmt_hex_lines(a_raw.get(o["offset"], b"")):
                print(f"  {GREEN}{line}{NC}")

    # ── Changed ──
    if changed:
        print(f"\n  {YELLOW}── CHANGED (content diff) ──{NC}")
        for off in sorted(changed):
            o   = a_objs[off]
            byt_b = b_raw.get(off, b"")
            byt_a = a_raw.get(off, b"")
            print(f"\n  {YELLOW}  ~ [{o['idx']:>3}] {_oid_printable(o['obj_id'])}"
                  f"  {off}  {o['size']} B{NC}")
            # Side-by-side: show BEFORE line, then AFTER line for changed rows
            b_lines = _fmt_hex_lines(byt_b, indent="")
            a_lines = _fmt_hex_lines(byt_a, indent="")
            max_rows = max(len(b_lines), len(a_lines))
            for i in range(max_rows):
                bl = b_lines[i] if i < len(b_lines) else ""
                al = a_lines[i] if i < len(a_lines) else ""
                if bl != al:
                    print(f"  {RED}  - {bl}{NC}")
                    print(f"  {GREEN}  + {al}{NC}")
                else:
                    print(f"      {bl}")

    if not added and not removed and not changed:
        print(f"\n  {GREEN}  no changes — flash state identical before and after{NC}")


# ── Section 1: Environment check ─────────────────────────────────────────────
def test_env():
    hdr("1. Environment check")

    # tee-supplicant (may run as tee-supplicant or optee-supplicant)
    r = subprocess.run(
        "pgrep -x tee-supplicant || pgrep -x optee-supplicant || "
        "pgrep -f tee-supplicant",
        shell=True, capture_output=True)
    if r.returncode == 0:
        ok("tee-supplicant is running")
    else:
        no("tee-supplicant not running — OP-TEE calls will fail")

    # libteec
    for lib in ("/usr/lib/libteec.so.2", "/usr/lib/libteec.so"):
        if Path(lib).exists():
            ok(f"libteec found: {lib}")
            break
    else:
        no("libteec not found — install optee-client")

    # command
    if shutil.which("w77q-fs-test"):
        ok(f"w77q-fs-test binary: {shutil.which('w77q-fs-test')}")
    elif Path("/usr/bin/w77q_fs_test.py").exists():
        ok("w77q_fs_test.py Python script: /usr/bin/w77q_fs_test.py")
    else:
        no("neither w77q-fs-test nor w77q_fs_test.py found")

    # optional w77q-dump
    if shutil.which("w77q-dump"):
        ok("w77q-dump present (flash diff available)")
    else:
        sk("w77q-dump not found — flash diffs will be skipped")

    # TA reachability: quick ping via TC_BASIC
    print(f"\n  {CYAN}Probing TA reachability (TC_BASIC quick run)…{NC}")
    r = subprocess.run(
        f"{_W77Q_FS_CMD} basic",
        shell=True, capture_output=True, text=True, timeout=20,
    )
    combined = r.stdout + r.stderr
    if r.returncode == 0 and "PASS" in combined:
        ok("TA reachable — TC_BASIC PASS")
    else:
        no(f"TA probe failed (rc={r.returncode}): {combined.strip()[:120]}")


# ── Generic single-TC section ─────────────────────────────────────────────────
def _run_single_tc(num: int, name: str):
    hdr(f"{num}. TC_{name.upper()} — {TC_DESC[name]}")
    tc_idx = TC_NAMES.index(name)

    # Step 1 — pre-create TC's initial objects so they're visible in flash
    # diag-setup returns 0=done, 2=not-applicable (TC_OVERWRITE), 1=error
    _, rc_setup = run_cmd(f"{_DIAG_CMD} diag-setup {tc_idx}")
    setup_ok = (rc_setup == 0)   # False for TC_OVERWRITE (rc=2) and errors

    snap_pre = _w77q_snap_full("setup" if setup_ok else "before")

    # Step 2 — run the actual TC (will overwrite/delete the pre-created objects)
    out, rc = run_cmd(f"{_W77Q_FS_CMD} {name}")
    snap_post = _w77q_snap_full("after")
    _w77q_compare(snap_pre, snap_post)

    # Step 3 — silent cleanup (in case TC failed and left objects behind)
    subprocess.run(f"{_DIAG_CMD} diag-teardown {tc_idx}",
                   shell=True, capture_output=True, timeout=15)

    if rc == 0 and _grep(r"PASS", out):
        ok(f"TC_{name.upper()}: PASS")
    elif "(skipped)" in out:
        sk(f"TC_{name.upper()}: skipped by user")
    else:
        no(f"TC_{name.upper()}: FAIL  (rc={rc})")


def test_tc_basic():    _run_single_tc(2, "basic")
def test_tc_update():   _run_single_tc(3, "update")
def test_tc_rename():   _run_single_tc(4, "rename")
def test_tc_enum():     _run_single_tc(5, "enum")
def test_tc_large():    _run_single_tc(6, "large")
def test_tc_overwrite():_run_single_tc(7, "overwrite")
def test_tc_truncate(): _run_single_tc(8, "truncate")


# ── Section 9: All TCs with before/after flash diff ──────────────────────────
def test_all_tc():
    hdr("9. All test cases — w77q_fs full suite")

    print(f"\n  {BOLD}[Enter]{NC}=run all  {BOLD}[name]{NC}=single test  {BOLD}[q]{NC}=skip")
    print(f"  Available: {CYAN}{' '.join(TC_NAMES)}{NC}")

    if AUTO:
        tests_to_run = TC_NAMES
    else:
        try:
            choice = _prompt("  Test name: ").strip().lower()
        except KeyboardInterrupt:
            choice = "q"
        if choice == "q":
            sk("w77q-fs full suite skipped")
            return
        elif choice in TC_NAMES:
            tests_to_run = [choice]
        else:
            tests_to_run = TC_NAMES

    snap_initial = _w77q_snap_full("initial")
    total_pass = total_fail = 0

    for name in tests_to_run:
        print(f"\n  {CYAN}┌── w77q-fs: {name} — {TC_DESC[name]} ──{NC}")
        snap_pre  = _w77q_snap_full("before")
        out, rc   = run_cmd(f"{_W77Q_FS_CMD} {name}")
        snap_post = _w77q_snap_full("after")
        _w77q_compare(snap_pre, snap_post)

        if rc == 0 and _grep(r"PASS", out):
            total_pass += 1
            print(f"  {CYAN}└── {GREEN}[PASS]{NC} w77q-fs: {name}")
        else:
            total_fail += 1
            print(f"  {CYAN}└── {RED}[FAIL]{NC} w77q-fs: {name}  rc={rc}")

    snap_final = _w77q_snap_full("final")
    print(f"\n  {CYAN}── Overall diff (initial → final) ──{NC}")
    _w77q_compare(snap_initial, snap_final)

    if total_fail == 0 and total_pass > 0:
        ok(f"w77q-fs suite: all {total_pass}/{len(tests_to_run)} PASSED")
    elif total_fail > 0:
        no(f"w77q-fs suite: {total_fail} FAILED, {total_pass} passed")
    else:
        no("w77q-fs suite: no results — was it skipped?")


# ── Section 10: Flash dump demonstration — create / inspect / delete ─────────
def test_flash_diag():
    hdr("10. Flash dump demo — create wft.diag → inspect → delete")

    if not shutil.which("w77q-dump"):
        sk("w77q-dump not available — skipping demo")
        return

    print(f"\n  {CYAN}This section creates a persistent test object (wft.diag),{NC}")
    print(f"  {CYAN}takes a flash snapshot so you can see it in the LUT and{NC}")
    print(f"  {CYAN}hex-dump its raw bytes, then deletes it.{NC}")

    # Always use the Python script for diag commands — the C binary doesn't
    # expose diag-create/diag-delete as named arguments.
    # (_DIAG_CMD is now a module-level constant)

    # Step 1 — baseline
    snap_before = _w77q_snap_full("baseline")

    # Step 2 — create wft.diag
    print(f"\n  {BOLD}Step 1: create wft.diag{NC}")
    out_create, rc_create = run_cmd(f"{_DIAG_CMD} diag-create")
    if rc_create != 0:
        no("diag-create failed — is TA rebuilt with TC_DIAG_CREATE support?")
        return

    # Step 3 — snap AFTER create and compare
    snap_after_create = _w77q_snap_full("after create")
    _w77q_compare(snap_before, snap_after_create)

    added = [o for o in snap_after_create["objects"]
             if o["offset"] not in {x["offset"] for x in snap_before["objects"]}]
    if added:
        ok(f"wft.diag visible in flash LUT: {len(added)} object(s) added")
    else:
        no("wft.diag not found in flash LUT after create — unexpected")

    # Step 4 — delete wft.diag
    print(f"\n  {BOLD}Step 2: delete wft.diag{NC}")
    out_delete, rc_delete = run_cmd(f"{_DIAG_CMD} diag-delete")

    # Step 5 — snap AFTER delete and compare
    snap_after_delete = _w77q_snap_full("after delete")
    print(f"\n  {CYAN}── diff: baseline → after-delete ──{NC}")
    _w77q_compare(snap_before, snap_after_delete)

    if rc_create == 0 and rc_delete == 0:
        ok("flash diag demo complete: create PASS, delete PASS")
    else:
        no(f"flash diag demo: create rc={rc_create}  delete rc={rc_delete}")


def print_summary():
    total = pass_count + fail_count + skip_count
    print(f"\n{BOLD}══════════════════════════════════════════════{NC}")
    print(f"{BOLD}  Test Summary{NC}")
    print(f"{BOLD}══════════════════════════════════════════════{NC}")
    print(f"  Total : {total}")
    print(f"  {GREEN}PASS{NC}  : {pass_count}")
    print(f"  {RED}FAIL{NC}  : {fail_count}")
    print(f"  {YELLOW}SKIP{NC}  : {skip_count}")
    print(f"\n  Log saved to: {logfile_path}\n")
    if fail_count == 0:
        print(f"  {GREEN}{BOLD}All tests passed!{NC}\n")
    else:
        print(f"  {RED}{BOLD}{fail_count} test(s) FAILED — see log{NC}\n")


# ── Menu ──────────────────────────────────────────────────────────────────────
_SECTIONS: dict[str, callable] = {
    "1": test_env,
    "2": test_tc_basic,
    "3": test_tc_update,
    "4": test_tc_rename,
    "5": test_tc_enum,
    "6": test_tc_large,
    "7": test_tc_overwrite,
    "8": test_tc_truncate,
    "9": test_all_tc,
    "10": test_flash_diag,
}


def show_menu():
    print(f"\n{BOLD}{CYAN}╔══════════════════════════════════════════════╗{NC}")
    print(f"{BOLD}{CYAN}║   W77Q Flash FS Test Suite                   ║{NC}")
    print(f"{BOLD}{CYAN}║   Sparrow Hawk — OP-TEE secure storage        ║{NC}")
    print(f"{BOLD}{CYAN}╚══════════════════════════════════════════════╝{NC}")
    print(f"\n  Command: {YELLOW}{_W77Q_FS_CMD}{NC}")
    print(f"\n  {BOLD}Individual test cases:{NC}")
    print("   1) Environment check")
    print(f"   2) basic      — {TC_DESC['basic']}")
    print(f"   3) update     — {TC_DESC['update']}")
    print(f"   4) rename     — {TC_DESC['rename']}")
    print(f"   5) enum       — {TC_DESC['enum']}")
    print(f"   6) large      — {TC_DESC['large']}")
    print(f"   7) overwrite  — {TC_DESC['overwrite']}")
    print(f"   8) truncate   — {TC_DESC['truncate']}")
    print(f"\n  {BOLD}Batch:{NC}")
    print("   9) All test cases with flash diff")
    print("  10) Flash dump demo — create wft.diag → hex inspect → delete")
    print(f"\n  {BOLD}Run all:{NC}")
    print("   a) Run ALL sections (1–9)")
    print("   q) Quit")


def main():
    global AUTO

    parser = argparse.ArgumentParser(
        description="W77Q flash filesystem interactive test suite for Sparrow Hawk"
    )
    parser.add_argument("--auto", action="store_true",
                        help="Run all tests non-interactively")
    args = parser.parse_args()
    AUTO = args.auto

    _log(f"Log: {logfile_path}")
    _log(f"Started: {datetime.now()}")
    _log(f"Command: {_W77Q_FS_CMD}")
    print(f"Log: {logfile_path}")

    if AUTO:
        for key in sorted(_SECTIONS, key=int):
            _SECTIONS[key]()
        print_summary()
        sys.exit(fail_count)

    while True:
        show_menu()
        try:
            choice = _prompt("\n  Choice: ").strip().lower()
        except KeyboardInterrupt:
            print()
            break

        if choice in _SECTIONS:
            _SECTIONS[choice]()
        elif choice == "a":
            for key in sorted(_SECTIONS, key=int):
                _SECTIONS[key]()
        elif choice in ("q", "quit"):
            break
        else:
            print("  Invalid choice.")
            continue

        try:
            _prompt(f"\n  {BOLD}Press Enter to return to menu…{NC} ")
        except KeyboardInterrupt:
            pass

    print_summary()


if __name__ == "__main__":
    main()
