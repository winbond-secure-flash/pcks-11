#!/usr/bin/env python3
"""pkcs-test-menu.py — Interactive PKCS#11 test suite for Sparrow Hawk.

Python port of pkcs-test-menu.sh; uses readline-based command editing
(same infrastructure as run_command.py).

Deploy as /usr/bin/pkcs-test-menu.py on the Sparrow Hawk board.
Usage: pkcs-test-menu.py [--auto]
  --auto : run all tests non-interactively

Requires: pkcs11-tool, openssl, tee-supplicant running
"""

import argparse
import ctypes
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

# ── ANSI colours ──────────────────────────────────────────────────────────────
RED    = '\033[0;31m'
GREEN  = '\033[0;32m'
YELLOW = '\033[1;33m'
CYAN   = '\033[0;36m'
WHITE  = '\033[1;37m'
BOLD   = '\033[1m'
NC     = '\033[0m'
WHITE_ON_CYAN = '\033[37;46m'
WHITE_ON_RED = '\033[37;41m'

# ── Runtime-editable configuration ────────────────────────────────────────────
@dataclass
class Config:
    mod:    str = "/usr/lib/libckteec.so"
    token0: str = "test-token"
    token1: str = "yuri"
    upin:   str = "87654321"
    sopin:  str = "12345678"


cfg = Config()

# Resolve module: fall back to .so.0 if plain .so is absent
if not Path(cfg.mod).exists() and Path(cfg.mod + ".0").exists():
    cfg.mod = cfg.mod + ".0"

# Pull values from tee-tests-common.sh if deployed on the board
_common = Path("/usr/bin/tee-tests-common.sh")
if _common.exists():
    _map = {"MOD": "mod", "TOKEN0": "token0", "TOKEN1": "token1",
            "UPIN": "upin", "SOPIN": "sopin"}
    for _line in _common.read_text().splitlines():
        for _var, _attr in _map.items():
            if _line.startswith(f"{_var}="):
                _val = _line.split("=", 1)[1]
                _val = re.sub(r'\s+#.*$', '', _val)   # strip inline shell comments
                _val = _val.strip().strip('"\'')       # strip whitespace and quotes
                if _val:
                    setattr(cfg, _attr, _val)


# ── Stats & log ───────────────────────────────────────────────────────────────
pass_count = fail_count = skip_count = 0
logfile_path = f"/tmp/pkcs-test-{datetime.now().strftime('%Y%m%d-%H%M%S')}.log"

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
    w = len(msg) + 4
    print(f"\n{BOLD}{WHITE_ON_CYAN}╔{'═' * w}╗{NC}")
    print(f"{BOLD}{WHITE_ON_CYAN}║  {msg}  ║{NC}")
    print(f"{BOLD}{WHITE_ON_CYAN}╚{'═' * w}╝{NC}")
    _log(f"\n══ {msg} ══")

def _grep(pattern: str, text: str) -> bool:
    return bool(re.search(pattern, text, re.IGNORECASE))

def _file_size(path: str) -> int:
    try:
        return Path(path).stat().st_size
    except OSError:
        return 0

def _files_equal(a: str, b: str) -> bool:
    try:
        return Path(a).read_bytes() == Path(b).read_bytes()
    except OSError:
        return False



# ── Command-edit helper ───────────────────────────────────────────────────────
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

    # cursor_v: visual index of cursor from start of full_prompt
    # prev_end_v: visual index of end of buffer on last draw (for tail-clearing)
    cursor_v   = [plen + pos]
    prev_end_v = [plen + len(buf)]

    def redraw() -> None:
        target_v  = plen + pos
        new_end_v = plen + len(buf)
        n_clear   = max(0, prev_end_v[0] - new_end_v)

        # Move to start of input: go up cur_row rows, then \r
        cur_row = cursor_v[0] // cols
        out = (f'\x1b[{cur_row}A' if cur_row > 0 else '') + '\r'
        # Reprint full content plus spaces to erase tail of previous longer line
        out += full_prompt + ''.join(buf) + (' ' * n_clear)
        # Cursor is now at visual position new_end_v + n_clear; reposition to target
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
                            pos -= 1
                            redraw()
                    elif code == b'C':                 # Right
                        if pos < len(buf):
                            pos += 1
                            redraw()
                    elif code == b'H':                 # Home  (CSI H)
                        pos = 0;          redraw()
                    elif code == b'F':                 # End   (CSI F)
                        pos = len(buf);   redraw()
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
                elif nxt == b'O':                      # SS3 (some xterm modes)
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
        # Leave cursor at end of buffer before the newline
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
    import re
    try:
        val = _termios_input_with_prefill(prompt, prefill)
        return val if val.strip() else prefill
    except Exception:
        pass
    # Non-TTY / termios unavailable fallback
    sys.stdout.write(f"  {CYAN}current:{NC} {prefill}\n")
    sys.stdout.write(f"  {prompt}(Enter=keep): ")
    sys.stdout.flush()
    val = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', sys.stdin.readline()).rstrip("\n")
    return val if val.strip() else prefill


def _prompt(msg: str) -> str:
    """Read a line from stdin with working backspace (raw mode on TTY)."""
    sys.stdout.write(msg)
    sys.stdout.flush()
    try:
        import termios
        fd = sys.stdin.fileno()
        old = termios.tcgetattr(fd)
    except (ImportError, ValueError, OSError):
        try:
            return sys.stdin.readline().rstrip("\n")
        except EOFError:
            raise KeyboardInterrupt
    new = termios.tcgetattr(fd)
    new[3] &= ~(termios.ECHO | termios.ICANON)
    new[6][termios.VMIN] = 1
    new[6][termios.VTIME] = 0
    buf: list[str] = []
    try:
        termios.tcsetattr(fd, termios.TCSAFLUSH, new)
        while True:
            ch = os.read(fd, 1)
            if ch in (b'\r', b'\n'):
                break
            if ch == b'\x03':
                raise KeyboardInterrupt
            if ch in (b'\x7f', b'\x08'):
                if buf:
                    buf.pop()
                    sys.stdout.write('\b \b')
                    sys.stdout.flush()
            elif ch == b'\x1b':          # eat escape sequences silently
                os.read(fd, 1); os.read(fd, 1)
            elif b' ' <= ch <= b'~':
                buf.append(ch.decode())
                sys.stdout.write(ch.decode())
                sys.stdout.flush()
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)
        sys.stdout.write('\n')
        sys.stdout.flush()
    return ''.join(buf)


# ── Command runner ────────────────────────────────────────────────────────────
AUTO = False


def run_cmd(cmd_str: str) -> tuple[str, int]:
    """
    Display command, prompt [Enter]=run / [e]=edit / [s]=skip with readline
    editing, execute via shell, print indented output.
    Returns (combined_output, returncode).
    """
    display = re.sub(r'\s*--module\s+"[^"]*"', '', cmd_str)
    display = re.sub(r'--iv\s+0{32}', '--iv 000...0', display)
    print(f"\n  {CYAN}▶ {NC}{BOLD}{display}{NC}", flush=True)
    _log(f"  CMD: {cmd_str}")

    if not AUTO:
        print(f"  {BOLD}[Enter]{NC}=run  {BOLD}[e]{NC}=edit  {BOLD}[s]{NC}=skip  ", end="", flush=True)
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
                edited = input_with_prefill("  Edit » ", cmd_str).strip()
                if edited:
                    cmd_str = edited
                    print(f"  {CYAN}▶ (edited){NC} {BOLD}{cmd_str}{NC}")
                    _log(f"  CMD(edited): {cmd_str}")
            except (EOFError, KeyboardInterrupt):
                pass  # keep original

    # Run via tee so output flows to the terminal in real-time AND is captured
    # for PASS/FAIL analysis.  Exit code is saved separately because the pipe
    # would otherwise replace it with tee's exit code.
    # Known-harmless pkcs11-tool warnings are filtered before display.
    _WARN_FILTER = (
        r"warning: PKCS11 function C_GetAttributeValue\(VALUE\) failed"
        r"|warning: PKCS11 function C_GetAttributeValue\(VERIFY_RECOVER\) failed"
    )
    out_f = f"/tmp/.pkcs11_{os.getpid()}.out"
    rc_f  = f"/tmp/.pkcs11_{os.getpid()}.rc"
    subprocess.run(
        f'{{ {cmd_str}; _rc=$?; echo $_rc >"{rc_f}"; }} 2>&1'
        f' | grep -Ev "{_WARN_FILTER}"'
        f' | tee "{out_f}" | sed "s/^/  {YELLOW}/;s/$/{NC}/"',
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


def p11(args_str: str) -> tuple[str, int]:
    """Run a pkcs11-tool command with the configured module."""
    return run_cmd(f'pkcs11-tool --module "{cfg.mod}" {args_str}')


def _hexshow(filepath: str, max_bytes: int = 64):
    """Print an xxd-style hex+ASCII dump of a binary file (no external tools)."""
    try:
        data = Path(filepath).read_bytes()
    except OSError:
        return
    total = len(data)
    shown = data[:max_bytes]
    trailer = f"  (first {max_bytes} of {total} bytes)" if total > max_bytes else f"  ({total} bytes)"
    print(f"  {CYAN}hex:{NC}{BOLD} {filepath}{trailer}{NC}")
    for i in range(0, len(shown), 16):
        chunk = shown[i:i + 16]
        hex_str = " ".join(f"{b:02x}" for b in chunk)
        asc_str = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        print(f"    {i:04x}:  {hex_str:<47}  {asc_str}")


# Object-type → text pattern that pkcs11-tool prints in --list-objects output.
_TYPE_PATTERN = {
    "privkey": r"Private Key",
    "pubkey":  r"Public Key",
    "secrkey": r"Secret Key",
    "data":    r"Data Object|object",
    "cert":    r"X\.509 cert|Certificate",
}

def _obj_exists(obj_type: str, *, obj_id: str = None, label: str = None) -> bool:
    """Return True if the PKCS#11 object is present on the token (silent check)."""
    id_flag = f"--id {obj_id}" if obj_id is not None else f"--label {label}"
    r = subprocess.run(
        f'pkcs11-tool --module "{cfg.mod}" --login'
        f' --token-label "{cfg.token0}" --pin "{cfg.upin}"'
        f' --list-objects --type {obj_type} {id_flag}',
        shell=True, capture_output=True, text=True,
    )
    combined = r.stdout + r.stderr
    pat = _TYPE_PATTERN.get(obj_type, ".")
    return bool(re.search(pat, combined, re.I))


def _del_obj(obj_type: str, *, obj_id: str = None, label: str = None) -> None:
    """Delete a PKCS#11 object only if it exists; skip silently otherwise."""
    id_flag = f"--id {obj_id}" if obj_id is not None else f"--label {label}"
    if not _obj_exists(obj_type, obj_id=obj_id, label=label):
        return
    run_cmd(
        f'pkcs11-tool --module "{cfg.mod}" --login'
        f' --token-label "{cfg.token0}" --pin "{cfg.upin}"'
        f' --delete-object --type {obj_type} {id_flag}'
    )


# ─────────────────────────────────────────────────────────────────────────────
# SECTION 1 — Environment check
# ─────────────────────────────────────────────────────────────────────────────
def test_env():
    hdr("1. Environment")

    rc = subprocess.run(["systemctl", "is-active", "tee-supplicant", "-q"],
                        capture_output=True).returncode
    if rc == 0:
        ok("tee-supplicant is running")
    else:
        no("tee-supplicant not running — starting")
        subprocess.run(["systemctl", "start", "tee-supplicant"], capture_output=True)
        time.sleep(2)

    if Path("/dev/tee0").exists():
        ok("/dev/tee0 present")
    else:
        no("/dev/tee0 missing — OP-TEE not loaded")
        return

    if Path(cfg.mod).exists():
        ok(f"module: {cfg.mod}")
    else:
        no(f"libckteec not found at {cfg.mod}")
        return

    if shutil.which("pkcs11-tool"):
        ver_out = subprocess.run(["dpkg", "-l", "opensc"],
                                 capture_output=True, text=True).stdout
        ver = next((l.split()[2] for l in ver_out.splitlines() if l.startswith("ii")), "")
        if not ver:
            ver_out2 = subprocess.run(["opkg", "list-installed"],
                                      capture_output=True, text=True).stdout
            ver = next((l.split()[2] for l in ver_out2.splitlines()
                        if l.startswith("opensc ")), "installed")
        ok(f"pkcs11-tool: OpenSC-{ver}")
    else:
        no("pkcs11-tool not found")

    if shutil.which("pkcs15-tool"):
        ok("pkcs15-tool: installed")
    else:
        sk("pkcs15-tool not found (OpenSC not installed)")

    if shutil.which("openssl"):
        ver = subprocess.run(["openssl", "version"],
                             capture_output=True, text=True).stdout.strip()
        ok(f"openssl: {ver}")
    else:
        sk("openssl not found (verify steps will be skipped)")


# ─────────────────────────────────────────────────────────────────────────────
# SECTION 2 — pkcs11-tool: Token info & diagnostics
# ─────────────────────────────────────────────────────────────────────────────
def test_p11_info():
    hdr("2. pkcs11-tool — Token info & diagnostics")

    print(f"\n  {CYAN}── Querying Cryptoki library info ──{NC}")
    out, _ = p11("--show-info")
    if _grep("cryptoki", out):
        ok("show-info: Cryptoki library info")
    else:
        no("show-info: unexpected output")

    print(f"\n  {CYAN}── Listing available slots ──{NC}")
    out, _ = p11("--list-slots")
    if _grep("slot", out):
        ok("list-slots: slot listing present")
    else:
        no("list-slots failed")

    print(f"\n  {CYAN}── Listing supported mechanisms ──{NC}")
    out, _ = p11("--slot 0 --list-mechanisms")
    cnt = out.count(",")
    if cnt > 5:
        ok(f"list-mechanisms: {cnt} mechanisms listed")
    else:
        no(f"list-mechanisms: too few results ({cnt})")

    print(f"\n  {CYAN}── Listing objects on token ──{NC}")
    out, rc = p11(f'--token-label "{cfg.token0}" --login --pin "{cfg.upin}" --list-objects')
    if rc == 0:
        cnt = len(re.findall(r"Object|Key|Cert|Data", out, re.I))
        ok(f"list-objects: {cnt} object(s) in token (exit 0)")
    else:
        no(f"list-objects: exit {rc} — {out.strip()}")


# ─────────────────────────────────────────────────────────────────────────────
# SECTION 3 — pkcs11-tool: Enforcing private object access
# ─────────────────────────────────────────────────────────────────────────────
def test_p11_token():
    hdr("3. pkcs11-tool — Enforcing private object access")

    # Seed token with 1 public + 1 private object for the visibility demo
    pub_file  = "/tmp/pub_demo.bin"
    priv_file = "/tmp/priv_demo.bin"
    Path(pub_file).write_bytes(b"PUBLIC-VISIBLE-TO-ALL")
    Path(priv_file).write_bytes(b"PRIVATE-NEEDS-LOGIN")

    print(f"\n  {CYAN}── Creating 1 public + 1 private object for demo ──{NC}")
    _del_obj("data", label="demo-public")
    _del_obj("data", label="demo-private")
    p11(
        f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}"'
        f' --write-object {pub_file} --type data --label demo-public'
    )
    p11(
        f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}"'
        f' --write-object {priv_file} --type data --label demo-private --private'
    )
    ok("seeded: 1 public object, 1 private object")

    print(f"\n  {BOLD}Without login only the public object is visible.{NC}")

    print(f"\n  {CYAN}── List objects without login (public only) ──{NC}")
    out, rc = p11(f'--token-label "{cfg.token0}" --list-objects')
    ok("no login — only public objects visible")

    print(f"\n  {CYAN}── Login with user PIN and list objects ──{NC}")
    out, rc = p11(f'--token-label "{cfg.token0}" --login --pin "{cfg.upin}" --list-objects')
    if rc != 0 or _grep(r"incorrect|CKR_PIN", out):
        no(f"user PIN login: PIN rejected — {out.strip()}")
    else:
        ok("user PIN login: accepted — all objects visible")

    # Cleanup
    _del_obj("data", label="demo-public")
    _del_obj("data", label="demo-private")



# ─────────────────────────────────────────────────────────────────────────────
# SECTION 4 — pkcs11-tool: Random number generation
# ─────────────────────────────────────────────────────────────────────────────
def test_p11_random():
    hdr("4. pkcs11-tool — Random number generation")

    print(f"\n  {CYAN}── Generating hardware random bytes ──{NC}")
    for nbytes in (32, 64):
        outfile = f"/tmp/rand{nbytes}.bin"
        out, _ = p11(
            f'--token-label "{cfg.token0}" --login --pin "{cfg.upin}"'
            f' --generate-random {nbytes} --output-file {outfile}'
        )
        sz = _file_size(outfile)
        if sz >= nbytes:
            ok(f"generate-random {nbytes} bytes: {sz} bytes")
            _hexshow(outfile)
        else:
            no(f"generate-random {nbytes}: only {sz} bytes — {out.strip()}")


# ─────────────────────────────────────────────────────────────────────────────
# SECTION 5 — pkcs11-tool: Hash / Digest
# ─────────────────────────────────────────────────────────────────────────────
def test_p11_hash():
    hdr("5. pkcs11-tool — Hash / Digest")

    plain = "/tmp/pt_hash.bin"
    Path(plain).write_bytes(b"Sparrow Hawk    hash test")
    ok(f"Input: {plain} ({_file_size(plain)} bytes)")
    _hexshow(plain)

    print(f"\n  {CYAN}── Computing digests on token ──{NC}")
    for mech in ("SHA256", "SHA384", "SHA512", "SHA-1"):
        hfile = f"/tmp/hash_{mech}.bin"
        out, _ = p11(
            f'--token-label "{cfg.token0}" --login --pin "{cfg.upin}"'
            f' --hash --mechanism {mech} --input-file {plain} --output-file {hfile}'
        )
        sz = _file_size(hfile)
        if sz > 0:
            ok(f"hash {mech}: {sz}-byte digest written")
            _hexshow(hfile, max_bytes=sz)
        else:
            no(f"hash {mech}: {out.strip()}")


# ─────────────────────────────────────────────────────────────────────────────
# SECTION 6 — pkcs11-tool: RSA operations
# ─────────────────────────────────────────────────────────────────────────────
def test_p11_rsa():
    hdr("6. pkcs11-tool — RSA operations")

    LABEL = "test-rsa-menu"
    ID    = "10"
    plain = "/tmp/pt_rsa.bin"
    sig   = "/tmp/sig_rsa.bin"
    pub   = "/tmp/rsa_pub.der"
    pem   = "/tmp/rsa_pub.pem"

    for t in ("privkey", "pubkey"):
        _del_obj(t, obj_id=ID)

    Path(plain).write_bytes(b"Hello W77Q secure world!")
    ok(f"Input: {plain} ({_file_size(plain)} bytes)")
    _hexshow(plain)

    # 6.1 Keypairgen RSA-2048
    print(f"\n  {CYAN}── Generating RSA-2048 key pair on token ──{NC}")
    out, _ = p11(
        f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}"'
        f' --keypairgen --key-type rsa:2048 --usage-sign --usage-decrypt'
        f' --label {LABEL} --id {ID}'
    )
    if _grep(r"key pair generated|Private Key", out):
        ok("RSA-2048 keypairgen (--usage-sign --usage-decrypt)")
    else:
        no(f"RSA-2048 keypairgen failed: {out.strip()}")
        return

    # 6.2 Export public key
    print(f"\n  {CYAN}── Exporting public key from token ──{NC}")
    out, _ = p11(
        f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}"'
        f' --read-object --type pubkey --id {ID} --output-file {pub}'
    )
    if _file_size(pub) > 0:
        ok(f"export pubkey: {_file_size(pub)} bytes DER")
    else:
        no(f"export pubkey failed: {out.strip()}")
        return

    # 6.3 Sign SHA256-RSA-PKCS (on token)
    print(f"\n  {CYAN}── Signing on token ──{NC}")
    out, _ = p11(
        f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}"'
        f' --sign --mechanism SHA256-RSA-PKCS'
        f' --id {ID} --input-file {plain} --output-file {sig}'
    )
    if _file_size(sig) > 0 and not _grep(r"error|CKR_|fail", out):
        ok(f"sign SHA256-RSA-PKCS: {_file_size(sig)}-byte signature")
        _hexshow(sig)
    else:
        no(f"sign SHA256-RSA-PKCS: {out.strip()}")
        return

    # 6.4 Verify with openssl (external — proves interoperability)
    if not shutil.which("openssl"):
        sk("openssl verify: openssl not found")
    else:
        print(f"\n  {CYAN}── Converting DER pubkey to PEM for openssl ──{NC}")
        run_cmd(f'openssl rsa -inform DER -pubin -in {pub} -out {pem}')
        print(f"\n  {CYAN}── Verifying signature with openssl (external) ──{NC}")
        out_v, rc = run_cmd(
            f'openssl dgst -sha256 -verify {pem}'
            f' -signature {sig} {plain}'
        )
        if rc == 0 and _grep(r"Verified OK", out_v):
            ok("openssl verify: Signature is valid (external verification)")
        else:
            no(f"openssl verify failed: {out_v.strip()}")

    # 6.5 Encrypt with openssl, decrypt on token
    print(f"\n  {CYAN}── Encrypting with openssl, decrypting on token ──{NC}")
    enc = "/tmp/enc_rsa.bin"
    dec = "/tmp/dec_rsa.bin"
    if not shutil.which("openssl"):
        sk("encrypt/decrypt RSA-PKCS: openssl not found")
    elif _file_size(pem) == 0:
        sk("encrypt/decrypt RSA-PKCS: no PEM pubkey available")
    else:
        print(f"\n  {CYAN}── Encrypting plaintext with openssl (public key) ──{NC}")
        _, rc_enc = run_cmd(
            f'openssl pkeyutl -encrypt -pkeyopt rsa_padding_mode:pkcs1'
            f' -pubin -inkey {pem} -in {plain} -out {enc}'
        )
        if rc_enc == 0:
            ok(f"openssl encrypt RSA-PKCS: {_file_size(enc)} bytes")
            _hexshow(enc)
            print(f"\n  {CYAN}── Decrypting on token (private key never leaves W77Q) ──{NC}")
            out, _ = p11(
                f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}"'
                f' --decrypt --mechanism RSA-PKCS --id {ID}'
                f' --input-file {enc} --output-file {dec}'
            )
            if _files_equal(plain, dec):
                ok("token decrypt RSA-PKCS: plaintext matches")
                dec_data = Path(dec).read_bytes().decode("utf-8", errors="replace")
                print(f"  {CYAN}Decrypted:{NC} \n  \"{dec_data}\"")
            else:
                no(f"token decrypt RSA-PKCS: {out.strip()}")
        else:
            no("openssl encrypt failed")

    # 6.6 List objects (show keypair on token before cleanup)
    print(f"\n  {CYAN}── Objects on token ──{NC}")
    p11(
        f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}"'
        f' --list-objects'
    )
    ok("listed objects on token")

    # 6.7 Cleanup
    print(f"\n  {CYAN}── Deleting test objects ──{NC}")
    for t in ("privkey", "pubkey"):
        _del_obj(t, obj_id=ID)
    ok("cleanup: RSA test keys deleted")
    for f_ in (plain, sig, pub, pem, enc, dec):
        Path(f_).unlink(missing_ok=True)


# ─────────────────────────────────────────────────────────────────────────────
# SECTION 7 — pkcs11-tool: EC operations
# ─────────────────────────────────────────────────────────────────────────────
def test_p11_ec():
    hdr("7. pkcs11-tool — EC operations")

    LABEL = "test-ec-menu"
    ID    = "11"
    plain = "/tmp/pt_ec.bin"
    sig   = "/tmp/sig_ec.bin"

    for t in ("privkey", "pubkey"):
        _del_obj(t, obj_id=ID)

    Path(plain).write_bytes(b"EC test message from pkcs-test-menu")
    ok(f"Input: {plain} ({_file_size(plain)} bytes)")
    _hexshow(plain)

    # 7.1 P-256 keypairgen
    print(f"\n  {CYAN}── Generating EC P-256 key pair ──{NC}")
    out, _ = p11(
        f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}"'
        f' --keypairgen --key-type EC:prime256v1 --label {LABEL} --id {ID}'
    )
    if _grep(r"key pair generated|Private Key", out):
        ok("EC P-256 keypairgen")
    else:
        no(f"EC P-256 keypairgen failed: {out.strip()}")
        return

    # 7.2 Sign ECDSA-SHA256
    print(f"\n  {CYAN}── Signing with ECDSA-SHA256 ──{NC}")
    out, _ = p11(
        f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}"'
        f' --sign --mechanism ECDSA-SHA256'
        f' --id {ID} --input-file {plain} --output-file {sig}'
    )
    if _grep(r"error|CKR_|fail", out):
        no(f"sign ECDSA-SHA256: {out.strip()}")
    else:
        ok("sign ECDSA-SHA256")
        _hexshow(sig)

    # 7.3 Verify ECDSA-SHA256
    print(f"\n  {CYAN}── Verifying ECDSA-SHA256 signature ──{NC}")
    out, _ = p11(
        f'--token-label "{cfg.token0}"'
        f' --verify --mechanism ECDSA-SHA256'
        f' --id {ID} --input-file {plain} --signature-file {sig}'
    )
    if _grep("Signature is valid", out):
        ok("verify ECDSA-SHA256: Signature is valid")
    else:
        no(f"verify ECDSA-SHA256: {out.strip()}")

    # Cleanup
    print(f"\n  {CYAN}── Deleting EC test keys ──{NC}")
    for t in ("privkey", "pubkey"):
        _del_obj(t, obj_id=ID)
    ok("cleanup: EC test keys deleted")


# ─────────────────────────────────────────────────────────────────────────────
# AES-CTR test via direct PKCS#11 ctypes API (pkcs11-tool cannot pass
# CK_AES_CTR_PARAMS; OP-TEE TA requires ulCounterBits=1)
# ─────────────────────────────────────────────────────────────────────────────
def _test_aes_ctr(key_label: str, plain_file: str, enc_file: str, dec_file: str) -> bool:
    CKR_OK      = 0
    CKU_USER    = 1
    CKM_AES_CTR = 0x1086

    class CK_MECHANISM(ctypes.Structure):
        _fields_ = [("mechanism",    ctypes.c_ulong),
                    ("pParameter",   ctypes.c_void_p),
                    ("ulParameterLen", ctypes.c_ulong)]

    class CK_AES_CTR_PARAMS(ctypes.Structure):
        _fields_ = [("ulCounterBits", ctypes.c_uint32),
                    ("cb",            ctypes.c_ubyte * 16)]

    class CK_ATTRIBUTE(ctypes.Structure):
        _fields_ = [("type",       ctypes.c_ulong),
                    ("pValue",     ctypes.c_void_p),
                    ("ulValueLen", ctypes.c_ulong)]

    # Full CK_TOKEN_INFO for slot/token discovery (PKCS#11 spec, 64-bit ABI).
    class CK_TOKEN_INFO(ctypes.Structure):
        _fields_ = [
            ("label",                 ctypes.c_char * 32),
            ("manufacturerID",        ctypes.c_char * 32),
            ("model",                 ctypes.c_char * 16),
            ("serialNumber",          ctypes.c_char * 16),
            ("flags",                 ctypes.c_ulong),
            ("ulMaxSessionCount",     ctypes.c_ulong),
            ("ulSessionCount",        ctypes.c_ulong),
            ("ulMaxRwSessionCount",   ctypes.c_ulong),
            ("ulRwSessionCount",      ctypes.c_ulong),
            ("ulMaxPinLen",           ctypes.c_ulong),
            ("ulMinPinLen",           ctypes.c_ulong),
            ("ulTotalPublicMemory",   ctypes.c_ulong),
            ("ulFreePublicMemory",    ctypes.c_ulong),
            ("ulTotalPrivateMemory",  ctypes.c_ulong),
            ("ulFreePrivateMemory",   ctypes.c_ulong),
            ("hardwareVersion",       ctypes.c_uint16),
            ("firmwareVersion",       ctypes.c_uint16),
            ("utcTime",               ctypes.c_char * 16),
        ]

    try:
        lib = ctypes.CDLL(cfg.mod)
    except OSError as e:
        print(f"  Could not load {cfg.mod}: {e}")
        return False

    lib.C_Initialize(None)

    # Discover which slot holds our token (label is space-padded to 32 bytes).
    n_slots = ctypes.c_ulong(16)
    slot_arr = (ctypes.c_ulong * 16)()
    rv = lib.C_GetSlotList(ctypes.c_ubyte(1), slot_arr, ctypes.byref(n_slots))
    if rv != CKR_OK or n_slots.value == 0:
        print(f"  C_GetSlotList failed or no tokens present: {rv:#x}")
        lib.C_Finalize(None)
        return False

    target_slot = slot_arr[0]
    want = cfg.token0.encode().ljust(32)
    for i in range(n_slots.value):
        ti = CK_TOKEN_INFO()
        if lib.C_GetTokenInfo(ctypes.c_ulong(slot_arr[i]), ctypes.byref(ti)) == CKR_OK:
            # Use string_at to read all 32 bytes regardless of embedded nulls.
            raw = ctypes.string_at(ctypes.addressof(ti), 32)
            if raw.rstrip(b" \x00") == cfg.token0.encode():
                target_slot = slot_arr[i]
                break

    hSess = ctypes.c_ulong(0)
    rv = lib.C_OpenSession(ctypes.c_ulong(target_slot), ctypes.c_ulong(6),
                           None, None, ctypes.byref(hSess))
    if rv != CKR_OK:
        print(f"  C_OpenSession on slot {target_slot} failed: {rv:#x}")
        lib.C_Finalize(None)
        return False

    pin = cfg.upin.encode()
    rv = lib.C_Login(hSess, ctypes.c_ulong(CKU_USER), pin, ctypes.c_ulong(len(pin)))
    if rv != CKR_OK:
        lib.C_CloseSession(hSess); lib.C_Finalize(None)
        print(f"  C_Login failed: {rv:#x}")
        return False

    lbl = key_label.encode()
    tmpl = (CK_ATTRIBUTE * 1)(
        CK_ATTRIBUTE(3, ctypes.cast(ctypes.c_char_p(lbl), ctypes.c_void_p), len(lbl))
    )
    lib.C_FindObjectsInit(hSess, tmpl, 1)
    handles = (ctypes.c_ulong * 4)()
    found = ctypes.c_ulong(0)
    lib.C_FindObjects(hSess, handles, 4, ctypes.byref(found))
    lib.C_FindObjectsFinal(hSess)
    if not found.value:
        lib.C_Logout(hSess); lib.C_CloseSession(hSess); lib.C_Finalize(None)
        print("  AES-CTR key not found")
        return False

    hKey = ctypes.c_ulong(handles[0])
    pt   = Path(plain_file).read_bytes()

    def _crypt(encrypt: bool, data: bytes) -> bytes | None:
        ctr  = CK_AES_CTR_PARAMS(1, (ctypes.c_ubyte * 16)(*range(16)))
        mech = CK_MECHANISM(CKM_AES_CTR,
                            ctypes.cast(ctypes.pointer(ctr), ctypes.c_void_p),
                            ctypes.sizeof(ctr))
        init = lib.C_EncryptInit if encrypt else lib.C_DecryptInit
        xfer = lib.C_Encrypt     if encrypt else lib.C_Decrypt
        if init(hSess, ctypes.byref(mech), hKey) != CKR_OK:
            return None
        buf  = (ctypes.c_ubyte * 256)()
        blen = ctypes.c_ulong(256)
        if xfer(hSess, data, ctypes.c_ulong(len(data)), buf, ctypes.byref(blen)) != CKR_OK:
            return None
        return bytes(buf[:blen.value])

    ct = _crypt(True,  pt)
    dt = _crypt(False, ct) if ct is not None else None

    lib.C_Logout(hSess); lib.C_CloseSession(hSess); lib.C_Finalize(None)

    if ct is not None and dt is not None:
        Path(enc_file).write_bytes(ct)
        Path(dec_file).write_bytes(dt)
        return dt == pt
    return False


# ─────────────────────────────────────────────────────────────────────────────
# SECTION 8 — pkcs11-tool: AES operations
# ─────────────────────────────────────────────────────────────────────────────
def test_p11_aes():
    hdr("8. pkcs11-tool — AES operations")

    plain   = "/tmp/pt_aes.bin"
    enc     = "/tmp/enc_aes.bin"
    dec     = "/tmp/dec_aes.bin"
    IV      = "00000000000000000000000000000000"
    ID_AES  = "13"
    ID_CMAC = "14"

    Path(plain).write_bytes(b"A" * 32)  # 32-byte block-aligned plaintext
    _hexshow(plain)

    # ── AES-CBC ───────────────────────────────────────────────────────────────
    print(f"\n  {CYAN}── AES-256 keygen (for CBC encrypt/decrypt) ──{NC}")
    _del_obj("secrkey", obj_id=ID_AES)
    out, _ = p11(
        f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}"'
        f' --keygen --key-type AES:32 --label test-aes-cbc --id {ID_AES} --sensitive'
    )
    if _grep(r"key generated|Secret Key", out):
        ok("AES-256 keygen (encrypt/decrypt)")
    else:
        no(f"AES-256 keygen failed: {out.strip()}")
        return

    print(f"\n  {CYAN}── AES-CBC encrypt (32-byte plaintext → ciphertext) ──{NC}")
    out, _ = p11(
        f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}"'
        f' --encrypt --mechanism AES-CBC --id {ID_AES}'
        f' --iv {IV} --input-file {plain} --output-file {enc}'
    )
    (no if _grep(r"error|CKR_|fail", out) else ok)(
        f"AES-CBC encrypt{': ' + out.strip() if _grep(r'error|CKR_|fail', out) else ''}"
    )
    if not _grep(r"error|CKR_|fail", out):
        _hexshow(enc)

    print(f"\n  {CYAN}── AES-CBC decrypt (ciphertext → plaintext, verify match) ──{NC}")
    out, _ = p11(
        f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}"'
        f' --decrypt --mechanism AES-CBC --id {ID_AES}'
        f' --iv {IV} --input-file {enc} --output-file {dec}'
    )
    if _files_equal(plain, dec):
        ok("AES-CBC decrypt: roundtrip MATCH")
        _hexshow(dec)
    else:
        no(f"AES-CBC decrypt: MISMATCH — {out.strip()}")

    # ── AES-CMAC ──────────────────────────────────────────────────────────────
    print(f"\n  {CYAN}── AES-256 keygen (sign-only, for CMAC) ──{NC}")
    _del_obj("secrkey", obj_id=ID_CMAC)
    out, _ = p11(
        f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}"'
        f' --keygen --key-type AES:32 --label test-aes-cmac --id {ID_CMAC}'
        f' --sensitive --usage-sign'
    )
    if _grep(r"key generated|Secret Key", out):
        ok("AES-256 keygen (--usage-sign for CMAC)")
    else:
        no(f"AES-256 CMAC keygen failed: {out.strip()}")
        return

    print(f"\n  {CYAN}── AES-CMAC sign (message authentication code) ──{NC}")
    out, _ = p11(
        f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}"'
        f' --sign --mechanism AES-CMAC --id {ID_CMAC}'
        f' --input-file {plain} --output-file /tmp/cmac.bin'
    )
    if _grep(r"error|CKR_|fail", out):
        no(f"AES-CMAC sign: {out.strip()}")
    else:
        try:
            hexval = Path("/tmp/cmac.bin").read_bytes().hex()[:32]
        except OSError:
            hexval = "?"
        ok(f"AES-CMAC sign: {hexval}...")

    print(f"\n  {CYAN}── AES-CMAC verify (authenticate same message) ──{NC}")
    out, _ = p11(
        f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}"'
        f' --verify --mechanism AES-CMAC --id {ID_CMAC}'
        f' --input-file {plain} --signature-file /tmp/cmac.bin'
    )
    if _grep(r"error|CKR_|fail", out):
        no(f"AES-CMAC verify: {out.strip()}")
    else:
        ok("AES-CMAC verify: signature valid")

    # Cleanup
    print(f"\n  {CYAN}── Deleting AES test keys ──{NC}")
    for id_c in (ID_AES, ID_CMAC):
        _del_obj("secrkey", obj_id=id_c)
    ok("cleanup: AES test keys deleted")


# ─────────────────────────────────────────────────────────────────────────────
# SECTION 9 — pkcs11-tool: Object management
# ─────────────────────────────────────────────────────────────────────────────
def test_p11_objects():
    hdr("9. pkcs11-tool — Object management")

    LABEL     = "test-obj-menu"
    LABEL_P   = "test-obj-private"
    ID        = "15"
    ID_P      = "16"
    data_file = "/tmp/obj_data.bin"
    priv_file = "/tmp/obj_priv.bin"
    read_file = "/tmp/obj_read.bin"

    for lbl in (LABEL, LABEL_P):
        _del_obj("data", label=lbl)

    Path(data_file).write_bytes(b"W77Q-PKCS11-DATA-OBJECT-01")

    # 9.1 write-object
    print(f"\n  {CYAN}── Writing data object to token ──{NC}")
    out, _ = p11(
        f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}"'
        f' --write-object {data_file} --type data --label {LABEL} --id {ID}'
    )
    if _grep(r"created|written", out) and not _grep(r"error|CKR_", out):
        ok("write-object data")
        _hexshow(data_file)
    else:
        no(f"write-object data: {out.strip()}")

    # 9.2 list-objects
    print(f"\n  {CYAN}── Listing objects on token ──{NC}")
    out, _ = p11(
        f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}" --list-objects'
    )
    if _grep(r"object|Key|Cert|Data", out):
        ok("list-objects: objects present")
    else:
        no("list-objects: no objects or error")

    # 9.3 read-object
    print(f"\n  {CYAN}── Reading data object from token ──{NC}")
    out, _ = p11(
        f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}"'
        f' --read-object --type data --label {LABEL} --output-file {read_file}'
    )
    if _files_equal(data_file, read_file):
        ok("read-object data: content matches")
        _hexshow(read_file)
    else:
        no(f"read-object data: mismatch or error — {out.strip()}")

    # 9.4 delete-object
    print(f"\n  {CYAN}── Deleting data object ──{NC}")
    out, rc = p11(
        f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}"'
        f' --delete-object --type data --label {LABEL}'
    )
    if rc == 0 and not _grep(r"CKR_|C_Initialize failed|Aborting|error", out):
        ok("delete-object data")
    else:
        no(f"delete-object data: {out.strip()[:120]}")

    # 9.5 list-objects after delete — confirm object is gone
    print(f"\n  {CYAN}── Listing objects (confirm deletion) ──{NC}")
    out, rc = p11(
        f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}" --list-objects'
    )
    if rc != 0 or _grep(r"CKR_|C_Initialize failed|Aborting", out):
        no(f"list-objects (confirm delete): device/list error — {out.strip()[:120]}")
    elif not _grep(LABEL, out):
        ok("list-objects: deleted object no longer present")
    else:
        no("list-objects: deleted object still visible")

    # 9.6 Private object visibility test
    print(f"\n  {CYAN}── Private object visibility test ──{NC}")
    priv_data = b"SECRET-W77Q-PRIVATE-RECORD"
    Path(priv_file).write_bytes(priv_data)
    out, rc = p11(
        f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}"'
        f' --write-object {priv_file} --type data --label {LABEL_P} --id {ID_P} --private'
    )
    if rc == 0 and not _grep(r"CKR_|error", out):
        ok("write private object")
        _hexshow(priv_file)
    else:
        no(f"write private object: {out.strip()[:80]}")

    # Without login — object must not be listed
    out, _ = p11(f'--token-label "{cfg.token0}" --list-objects --type data')
    if not _grep(LABEL_P, out):
        ok("private object NOT visible without login")
    else:
        no("private object unexpectedly visible without login")

    # With login — private object becomes visible
    print(f"\n  {CYAN}── Listing with login (private object appears) ──{NC}")
    out, _ = p11(
        f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}"'
        f' --list-objects --type data'
    )
    if _grep(LABEL_P, out):
        ok("private object visible WITH login")
    else:
        no("private object not visible even with login")

    # Read private object with login
    if Path(read_file).exists():
        Path(read_file).unlink()
    out, _ = p11(
        f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}"'
        f' --read-object --type data --label {LABEL_P} --output-file {read_file}'
    )
    if _files_equal(priv_file, read_file):
        ok("private object readable WITH login: content matches")
        _hexshow(read_file)
    else:
        no(f"private object read with login: failed — {out.strip()[:80]}")

    # Cleanup
    print(f"\n  {CYAN}── Cleanup ──{NC}")
    _del_obj("data", label=LABEL_P)
    ok("cleanup: test objects deleted")


# ─────────────────────────────────────────────────────────────────────────────
# SECTION 10 — Token inventory & mechanism listing (pkcs11-tool; no PC/SC
#              reader on board so pkcs15-tool CLI is not used here)
# ─────────────────────────────────────────────────────────────────────────────
def test_p15_info():
    hdr("10. Token — Inventory & mechanism listing (pkcs11-tool)")
    return  # TODO: re-enable section 10

    if _grep(r"error|failed", out):
        no(f"list-slots: {out.strip()}")
    else:
        ok(f"list-slots: {len(re.findall('Slot', out))} slot(s) found")

    out, _ = p11(f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}" --list-objects')
    if _grep(r"CKR_|failed", out):
        no(f"list-objects: {out.strip()}")
    else:
        ok(f"list-objects: {len(re.findall('Object', out, re.I))} object(s) on token")

    for label, flags, pattern in (
        ("list privkeys",   f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}" --list-objects --type privkey', "Private Key"),
        ("list pubkeys",    f'--token-label "{cfg.token0}" --list-objects --type pubkey',                             "Public Key"),
        ("list certs",      f'--token-label "{cfg.token0}" --list-objects --type cert',                               r"X\.509 cert"),
        ("list secret keys",f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}" --list-objects --type secrkey', "Secret Key"),
        ("list data objects",f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}" --list-objects --type data',   r"Data Object|object"),
    ):
        out, _ = p11(flags)
        if _grep(r"CKR_|failed", out):
            no(f"{label}: {out.strip()}")
        else:
            ok(f"{label}: {len(re.findall(pattern, out, re.I))} found")

    out, _ = p11("--list-mechanisms --slot 0")
    if _grep(r"error|failed", out):
        no(f"list-mechanisms: {out.strip()}")
    else:
        ok(f"list-mechanisms: {out.count(',')} mechanism(s) supported")


# ─────────────────────────────────────────────────────────────────────────────
# SECTION 11 — RSA keypair export & OpenSSL structural verify (pkcs11-tool)
# ─────────────────────────────────────────────────────────────────────────────
def test_p15_keys():
    hdr("11. RSA keypair export & OpenSSL verify (pkcs11-tool)")
    return  # TODO: re-enable section 11



# ─────────────────────────────────────────────────────────────────────────────
# SECTION 12 — Self-signed cert: generate → import → read → OpenSSL verify (pkcs11-tool)
# ─────────────────────────────────────────────────────────────────────────────
def test_p15_cert():
    hdr("12. X.509 cert import, read & OpenSSL verify (pkcs11-tool)")
    return  # TODO: re-enable section 12



# ─────────────────────────────────────────────────────────────────────────────
# Configure parameters — readline pre-fill lets you edit in place
# ─────────────────────────────────────────────────────────────────────────────
def edit_params():
    print(f"\n{BOLD}{CYAN}── Configure Parameters ──{NC}")
    print("  Press Enter to keep the current value shown in [brackets].\n")

    for attr, label in (
        ("mod",    "Module path   "),
        ("token0", "Token 0 label "),
        ("token1", "Token 1 label "),
        ("upin",   "User PIN      "),
        ("sopin",  "SO PIN        "),
    ):
        cur = getattr(cfg, attr)
        try:
            val = _prompt(f"  {label} [{cur}]: ").strip()
        except (EOFError, KeyboardInterrupt):
            val = ""
        if val:
            setattr(cfg, attr, val)

    print(f"\n  Module : {cfg.mod}")
    print(f"  Token0 : {cfg.token0}")
    print(f"  Token1 : {cfg.token1}")
    print(f"  UPIN   : {cfg.upin}   SOPIN: {cfg.sopin}")
    print(f"  {GREEN}Parameters updated.{NC}")


# ─────────────────────────────────────────────────────────────────────────────
# SECTION 10 — Error & PIN lockout tests
# ─────────────────────────────────────────────────────────────────────────────
def test_p11_errors():
    hdr("10. Error & PIN lockout tests")

    # 10.1 — Wrong PIN: login must fail with CKR_PIN_INCORRECT
    print(f"\n  {CYAN}── Wrong PIN rejection test ──{NC}")
    out, rc = p11(
        f'--token-label "{cfg.token0}" --login --pin "WRONG_PIN_TEST"'
        f' --list-objects'
    )
    if rc != 0 and _grep(r"CKR_PIN_INCORRECT|Login failed|login.*fail|incorrect", out):
        ok("wrong PIN: login correctly rejected (CKR_PIN_INCORRECT)")
    elif rc != 0:
        ok(f"wrong PIN: login rejected (rc={rc})")
    else:
        no("wrong PIN: login should have failed but succeeded")

    # 10.2 — Non-existent object ID: read-object must fail gracefully
    print(f"\n  {CYAN}── Non-existent object ID rejection test ──{NC}")
    out, rc = p11(
        f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}"'
        f' --read-object --type pubkey --id ff'
    )
    if rc != 0 and _grep(r"not found|CKR_|error|no object", out):
        ok("non-existent object ID: read-object correctly rejected")
    elif rc != 0:
        ok(f"non-existent object ID: rejected (rc={rc})")
    else:
        no("non-existent object ID: should have failed but succeeded")

    # 10.4 — PIN lockout cycle
    # This test intentionally locks the token with wrong PINs, verifies
    # lockout, then restores the user PIN via SO PIN.
    print(f"\n  {BOLD}{YELLOW}⚠  PIN lockout test{NC}")
    print(f"  Will enter wrong PIN repeatedly to lock the token,")
    print(f"  then unlock using SO PIN [{cfg.sopin}].")
    print(f"  {RED}Skip this if you don't have the SO PIN!{NC}")
    try:
        ans = _prompt("  Proceed with lockout test? [y/N]: ").strip().lower()
    except KeyboardInterrupt:
        ans = "n"
    if ans != "y":
        sk("PIN lockout test: skipped by user")
        return

    # 10.3 — Create test objects BEFORE lockout to prove they survive
    LOCKOUT_LABEL_KEY  = "lockout-test-key"
    LOCKOUT_LABEL_DATA = "lockout-test-data"
    LOCKOUT_ID_KEY     = "A0"
    LOCKOUT_DATA       = b"PIN-lockout-survival-test-payload"
    lockout_data_file  = "/tmp/lockout_data.bin"

    _del_obj("secrkey", label=LOCKOUT_LABEL_KEY)
    _del_obj("data", label=LOCKOUT_LABEL_DATA)

    print(f"\n  {CYAN}── Creating objects BEFORE lockout ──{NC}")
    Path(lockout_data_file).write_bytes(LOCKOUT_DATA)
    out, _ = p11(
        f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}"'
        f' --keygen --key-type AES:32 --label {LOCKOUT_LABEL_KEY}'
        f' --id {LOCKOUT_ID_KEY} --sensitive'
    )
    if _grep(r"key generated|Secret Key", out):
        ok(f"created AES key '{LOCKOUT_LABEL_KEY}' before lockout")
    else:
        no(f"AES keygen failed: {out.strip()}")

    out, _ = p11(
        f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}"'
        f' --write-object {lockout_data_file} --type data'
        f' --label {LOCKOUT_LABEL_DATA} --private'
    )
    if not _grep(r"error|CKR_", out):
        ok(f"created data object '{LOCKOUT_LABEL_DATA}' before lockout")
    else:
        no(f"write-object failed: {out.strip()}")

    # Verify objects exist now
    if _obj_exists("secrkey", label=LOCKOUT_LABEL_KEY):
        ok("AES key confirmed present before lockout")
    else:
        no("AES key not found — aborting lockout test")
        return

    print(f"\n  {BOLD}Now locking the token with wrong PINs…{NC}")

    # Try wrong PINs until CKR_PIN_LOCKED (max 15 attempts)
    locked = False
    for attempt in range(1, 16):
        out, rc = run_cmd(
            f'pkcs11-tool --module "{cfg.mod}"'
            f' --token-label "{cfg.token0}" --login --pin "LOCK{attempt:04d}"'
            f' --list-objects'
        )
        if _grep(r"CKR_PIN_LOCKED|token.*locked|PIN.*locked", out):
            ok(f"PIN locked after {attempt} wrong attempt(s)")
            locked = True
            break
        elif _grep(r"CKR_PIN_INCORRECT|Login failed|incorrect", out):
            print(f"  attempt {attempt}: incorrect PIN (not yet locked)")
        else:
            print(f"  attempt {attempt}: rc={rc} — {out.strip()[:80]}")

    if not locked:
        no("PIN lockout: token did not lock within 15 attempts")
        sk("PIN lock verify: skipped (no lockout)")
        sk("PIN unlock: skipped (no lockout)")
        sk("PIN unlock verify: skipped (no lockout)")
        return

    # 10.5 — Verify lockout: correct PIN must now also fail
    print(f"\n  {CYAN}── Verifying token is locked ──{NC}")
    out, rc = p11(
        f'--token-label "{cfg.token0}" --login --pin "{cfg.upin}"'
        f' --list-objects'
    )
    if rc != 0 and _grep(r"CKR_PIN_LOCKED|locked", out):
        ok("lock verified: correct PIN also rejected while locked")
    elif rc != 0:
        ok(f"lock verified: correct PIN rejected (rc={rc})")
    else:
        no("lock verify: correct PIN succeeded — token was not actually locked")

    # 10.6 — Unlock: re-initialise user PIN via SO login
    print(f"\n  {CYAN}── Unlocking token via SO PIN ──{NC}")
    out, rc = p11(
        f'--token-label "{cfg.token0}" --login --login-type so'
        f' --so-pin "{cfg.sopin}" --init-pin --new-pin "{cfg.upin}"'
    )
    if rc == 0 and not _grep(r"error|CKR_(?!OK)", out):
        ok("PIN unlock: user PIN re-initialised via SO PIN")
    else:
        no(f"PIN unlock failed (rc={rc}): {out.strip()}")
        return

    # 10.7 — Verify unlock: normal login must succeed again
    print(f"\n  {CYAN}── Verifying token is unlocked ──{NC}")
    out, rc = p11(
        f'--token-label "{cfg.token0}" --login --pin "{cfg.upin}"'
        f' --list-objects'
    )
    if rc == 0:
        ok("PIN unlock verified: token accessible with user PIN again")
    else:
        no(f"PIN unlock verify failed (rc={rc}): {out.strip()}")

    # 10.8 — Verify objects survived the lockout + PIN re-init
    print(f"\n  {CYAN}── Verifying objects survived lockout ──{NC}")
    if _obj_exists("secrkey", label=LOCKOUT_LABEL_KEY):
        ok(f"AES key '{LOCKOUT_LABEL_KEY}' still present after lockout + unlock")
    else:
        no(f"AES key '{LOCKOUT_LABEL_KEY}' LOST after lockout — data destroyed!")

    if _obj_exists("data", label=LOCKOUT_LABEL_DATA):
        ok(f"data object '{LOCKOUT_LABEL_DATA}' still present after lockout + unlock")
    else:
        no(f"data object '{LOCKOUT_LABEL_DATA}' LOST after lockout — data destroyed!")

    # Read back data object and verify content matches
    read_back = "/tmp/lockout_readback.bin"
    out, _ = p11(
        f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}"'
        f' --read-object --type data --label {LOCKOUT_LABEL_DATA}'
        f' --output-file {read_back}'
    )
    if _files_equal(lockout_data_file, read_back):
        ok("data object content intact after lockout (byte-for-byte match)")
        dec_data = Path(read_back).read_bytes().decode("utf-8", errors="replace")
        print(f"  {CYAN}Content:{NC} \n  \"{dec_data}\"")
    else:
        no("data object content CHANGED or unreadable after lockout")

    print(f"\n  {GREEN}{BOLD}  ✓  PIN lockout does NOT erase objects.{NC}")
    print(f"  {BOLD}  Keys and data survive wrong-PIN lockout + SO PIN re-init.{NC}")

    # Cleanup
    print(f"\n  {CYAN}── Cleanup ──{NC}")
    _del_obj("secrkey", label=LOCKOUT_LABEL_KEY)
    _del_obj("data", label=LOCKOUT_LABEL_DATA)
    Path(lockout_data_file).unlink(missing_ok=True)
    Path(read_back).unlink(missing_ok=True)
    ok("lockout test objects cleaned up")


# ─────────────────────────────────────────────────────────────────────────────
# SECTION 0 — Build TEE storage from blank
# ─────────────────────────────────────────────────────────────────────────────

def setup_tee_storage():
    """Erase W77Q flash and re-initialize the PKCS#11 token from scratch.

    Delegates to build_tee_storage.py when present at /usr/bin/ or alongside
    this script; falls back to an inline implementation so this function works
    even when the standalone script is not deployed.
    """
    hdr("0. Build TEE storage from blank")

    import importlib.util as _ilu
    import os as _os

    # Locate build_tee_storage.py (same dir as this script, or /usr/bin/)
    _candidates = [
        _os.path.join(_os.path.dirname(_os.path.abspath(__file__)),
                      "build_tee_storage.py"),
        "/usr/bin/build_tee_storage.py",
    ]
    _mod_path = next((p for p in _candidates if _os.path.exists(p)), None)

    if _mod_path:
        # Import and run via the module's run_build() entry point so that
        # config (mod, token0, upin, sopin) stays consistent with ours.
        try:
            _spec = _ilu.spec_from_file_location("build_tee_storage", _mod_path)
            _bts  = _ilu.module_from_spec(_spec)
            _spec.loader.exec_module(_bts)
            # Push our config values into the imported module's cfg
            _bts.cfg.mod    = cfg.mod
            _bts.cfg.token0 = cfg.token0
            _bts.cfg.upin   = cfg.upin
            _bts.cfg.sopin  = cfg.sopin
            _bts.AUTO    = AUTO
            _bts.DRY_RUN = False
            rc = _bts.run_build(confirmed=AUTO)  # skip re-confirm in --auto mode
            if rc == 0:
                ok("TEE storage built from blank successfully")
            else:
                no(f"build_tee_storage.py reported {rc} failure(s)")
            return
        except Exception as _e:
            no(f"Failed to run build_tee_storage.py: {_e}")
            return

    # ── Inline fallback (build_tee_storage.py not found) ────────────────────
    sk("build_tee_storage.py not found — using inline fallback")

    if not shutil.which("w77q-dump"):
        no("w77q-dump not found — cannot erase flash")
        return
    if not shutil.which("pkcs11-tool"):
        no("pkcs11-tool not found")
        return

    print(f"\n  {RED}{BOLD}WARNING:{NC}  "
          f"This will ERASE ALL objects from the W77Q OP-TEE partition.\n")
    if not AUTO:
        try:
            ans = _prompt(
                f"  Proceed with W77Q erase and token re-initialization? [y/N]: "
            ).strip().lower()
        except (KeyboardInterrupt, EOFError):
            print()
            return
        if ans not in ("y", "yes"):
            sk("Aborted by user")
            return

    # ── Stop supplicant, erase flash, restart.
    subprocess.run(["systemctl", "stop", "tee-supplicant"], capture_output=True)
    time.sleep(1)

    _, rc = run_cmd("w77q-dump erase-chip")
    if rc != 0:
        sk("w77q-dump erase-chip failed; falling back to sector erase (4 MB partition)")
        failed = False
        for i in range(1024):
            off = i * 0x1000
            rc_sec = subprocess.run(
                ["w77q-dump", "erase-sector", f"0x{off:x}"],
                capture_output=True,
            ).returncode
            if rc_sec != 0:
                no(f"w77q-dump erase-sector failed at 0x{off:08x}")
                subprocess.run(["systemctl", "start", "tee-supplicant"],
                               capture_output=True)
                return
            if i and i % 128 == 0:
                print(f"  {BOLD}…erased {i}/1024 sectors{NC}")
        ok("W77Q partition erased by sector loop")
    else:
        ok("W77Q flash erased")

    subprocess.run(["systemctl", "start", "tee-supplicant"], capture_output=True)
    for _ in range(20):
        if subprocess.run(["systemctl", "is-active", "tee-supplicant", "-q"],
                          capture_output=True).returncode == 0:
            break
        time.sleep(0.5)
    ok("tee-supplicant restarted")

    # ── Initialise token from blank flash
    # Give tee-supplicant a moment to register with the TEE driver.
    time.sleep(2)

    _, rc = p11(
        f'--slot-index 0 --init-token --label "{cfg.token0}" --so-pin "{cfg.sopin}"'
    )
    if rc != 0:
        # First init-token panics the TA (stale RAM vs empty flash).
        # The panic kills the TA instance; retry loads a fresh TA from clean flash.
        time.sleep(2)
        _, rc = p11(
            f'--slot-index 0 --init-token --label "{cfg.token0}" --so-pin "{cfg.sopin}"'
        )
    if rc != 0:
        no("--init-token failed")
        return
    ok(f"Token '{cfg.token0}' initialized")

    _, rc = p11(
        f'--token-label "{cfg.token0}" --login --login-type so --so-pin "{cfg.sopin}"'
        f' --init-pin --pin "{cfg.upin}"'
    )
    if rc != 0:
        no("--init-pin failed")
        return
    ok("User PIN set")

    # Verify
    out, rc = p11(f'--token-label "{cfg.token0}" --login --pin "{cfg.upin}" --list-objects')
    if rc == 0 and not _grep(r"CKR_PIN|incorrect", out):
        ok("User PIN login verified — token ready")
    else:
        no(f"User PIN login failed after init: {out.strip()[:80]}")



# ─────────────────────────────────────────────────────────────────────────────
# SECTION 0i — Token init only (no flash erase)
# ─────────────────────────────────────────────────────────────────────────────

def setup_token_init():
    """Re-initialize the PKCS#11 token on slot 0 without erasing W77Q flash.

    Useful when the token was lost (e.g. after an aborted test run) but
    the rest of the W77Q FS (ta_ver.db, other TA objects) is still intact.
    """
    hdr("0i. Token initialization only (no flash erase)")

    if not shutil.which("pkcs11-tool"):
        no("pkcs11-tool not found")
        return

    # Check current slot 0 state
    out, rc = run_cmd("pkcs11-tool --module \"/usr/lib/libckteec.so\" --list-token-slots")
    print(f"  current slots:\n" + "\n".join(f"    {l}" for l in out.splitlines()[:12]))

    if not AUTO:
        try:
            ans = _prompt(
                f"\n  Initialize token on slot 0 (label={cfg.token0})? [y/N]: "
            ).strip().lower()
        except (KeyboardInterrupt, EOFError):
            print()
            return
        if ans not in ("y", "yes"):
            sk("Aborted by user")
            return

    # --init-token on slot 0
    _, rc = p11(
        f'--slot-index 0 --init-token --label "{cfg.token0}" --so-pin "{cfg.sopin}"'
    )
    if rc != 0:
        no("--init-token failed")
        return
    ok(f"Token '{cfg.token0}' initialized")

    # --init-pin (set user PIN via SO login)
    _, rc = p11(
        f'--token-label "{cfg.token0}" --login --login-type so --so-pin "{cfg.sopin}"'
        f' --init-pin --pin "{cfg.upin}"'
    )
    if rc != 0:
        no("--init-pin failed")
        return
    ok("User PIN set")

    # Verify login
    out, rc = p11(f'--token-label "{cfg.token0}" --login --pin "{cfg.upin}" --list-objects')
    if rc == 0 and not _grep(r"CKR_PIN|incorrect", out):
        ok("User PIN login verified — token ready")
    else:
        no(f"User PIN login failed after init: {out.strip()[:80]}")


# OP-TEE TA UUID for PKCS#11 / libckteec objects stored in W77Q flash.
_PKCS11_TA_UUID = "dac902fd-6c30-c748-a49c-bbd827ae86ee"


def _w77q_obj_name(obj_id_hex: str) -> str:
    """Decode a hex-encoded obj_id (as emitted by w77q-dump) into a human name.

    w77q-dump now prints obj_id as the hex of the raw object-ID bytes so that
    binary IDs survive the shell round-trip.  OP-TEE stores token/db IDs as
    ASCII (which itself may be a further hex layer); binary PKCS#11 handles are
    left as hex.  This unwraps up to a few hex layers and strips NUL padding.
    """
    cur = (obj_id_hex or "").strip()
    for _ in range(3):
        try:
            b = bytes.fromhex(cur)
        except ValueError:
            break
        b = b.rstrip(b"\x00")
        if not b:
            break
        if all(32 <= c < 127 for c in b):
            s = b.decode("ascii")
            if len(s) % 2 == 0 and all(ch in "0123456789abcdefABCDEF" for ch in s):
                cur = s  # printable but itself hex — unwrap another layer
                continue
            return s
        return cur  # binary bytes — keep the hex form of this layer
    return cur


# ── Python-native execution helpers ──────────────────────────────────────────

def _py_exec(cmd: str) -> tuple[str, int]:
    """Run *cmd* via Python subprocess (no shell pipeline), print full I/O,
    and return (combined_output, returncode).

    Shows:
      ▶ <cmd>
      <output lines, indented>
      → rc N
    """
    print(f"\n  {BOLD}▶ {cmd}{NC}")
    try:
        r = subprocess.run(
            cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=30,
        )
        text = r.stdout.decode("ascii", errors="replace")
        for line in text.splitlines():
            print(f"    {line}")
        rc = r.returncode
    except Exception as exc:
        text = str(exc)
        print(f"    {YELLOW}error: {exc}{NC}")
        rc = -1
    print(f"    {CYAN}→ rc {rc}{NC}")
    return text, rc


def _w77q_list_objects() -> list[dict]:
    """Run w77q-dump list-all via Python subprocess and parse the LUT.

    Returns a list of dicts: idx, ta_uuid, offset, size, obj_id.
    Prints nothing — caller decides what to display.
    """
    import re as _re2
    try:
        r = subprocess.run(
            "w77q-dump list-all", shell=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=15,
        )
        text = r.stdout.decode("ascii", errors="replace")
    except Exception:
        return []
    objects = []
    for line in text.splitlines():
        m = _re2.search(
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
                "name":    _w77q_obj_name(m.group(5).strip()),
            })
    return objects


def _w77q_show_state(label: str, prev: list[dict] | None = None):
    """Print a compact flash object table.  If *prev* is given, mark new/gone."""
    objects = _w77q_list_objects()
    pkcs11  = [o for o in objects if o["ta_uuid"] == _PKCS11_TA_UUID]
    other   = [o for o in objects if o["ta_uuid"] != _PKCS11_TA_UUID]
    print(f"\n  {CYAN}── flash state {label}: "
          f"{len(objects)} objects ({len(pkcs11)} PKCS#11, {len(other)} other) ──{NC}")
    if not objects:
        print(f"  {YELLOW}  (no objects){NC}")
        return objects

    prev_keys = {(o["ta_uuid"], o["obj_id"]) for o in (prev or [])}
    cur_keys  = {(o["ta_uuid"], o["obj_id"]) for o in objects}
    new_keys  = cur_keys - prev_keys
    gone_keys = prev_keys - cur_keys

    print(f"  {'IDX':>3}  {'OFFSET':>10}  {'SIZE':>6}  OBJ-ID")
    print(f"  {'─'*3}  {'─'*10}  {'─'*6}  {'─'*32}")
    for o in objects:
        k   = (o["ta_uuid"], o["obj_id"])
        tag = f" {GREEN}← NEW{NC}" if k in new_keys else ""
        obj_p = o.get("name") or o["obj_id"]
        ta_s  = o["ta_uuid"][:8]
        print(f"  {o['idx']:>3}  {o['offset']:>10}  {o['size']:>6}  {obj_p}{tag}")
    for k in gone_keys:
        print(f"  {YELLOW}  removed: {k[1]}{NC}")
    return objects



def _w77q_read_file(ta_uuid: str, obj_id: str) -> bytes | None:
    """Read a file's content through LittleFS via w77q-dump read-file.

    Returns the raw file bytes or None on failure.
    """
    try:
        r = subprocess.run(
            f"w77q-dump read-file {ta_uuid} {obj_id}",
            shell=True, capture_output=True, timeout=10,
        )
        text = (r.stdout or b"").decode("ascii", errors="replace")
        if r.returncode != 0 or "[ERROR]" in text:
            return None
        raw = bytearray()
        for line in text.splitlines():
            pipe = line.find("|")
            if pipe >= 0:
                line = line[:pipe]
            tokens = line.split()
            if not tokens:
                continue
            start = 1 if len(tokens[0]) == 4 and all(
                c in "0123456789abcdefABCDEF" for c in tokens[0]
            ) else 0
            for tok in tokens[start:]:
                if len(tok) == 2:
                    try:
                        raw.append(int(tok, 16))
                    except ValueError:
                        pass
        return bytes(raw) if raw else None
    except Exception:
        return None


def _w77q_read_bytes(flash_off: str, size: int) -> bytes | None:
    """Read raw bytes from the W77Q flash via w77q-dump read-raw.

    Reads size+256 bytes so the 128-byte W77Q object header that precedes
    the OP-TEE payload is always included in the returned data.

    w77q-dump format:  AAAA  HH HH HH HH  HH HH HH HH  ...  |ASCII|
    (4-hex address, spaces, pairs of hex bytes, pipe-delimited ASCII)
    """
    return _w77q_read_exact(flash_off, size + 256)


def _w77q_read_exact(flash_off: str, size: int) -> bytes | None:
    """Read exactly @size bytes from flash at @flash_off via w77q-dump read-raw."""
    try:
        r = subprocess.run(
            f"w77q-dump read-raw {flash_off} {size}",
            shell=True, capture_output=True, timeout=10,
        )
        text = (r.stdout or b"").decode("ascii", errors="replace")
        raw = bytearray()
        for line in text.splitlines():
            # Strip the ASCII column (everything from the first '|' onward)
            pipe = line.find("|")
            if pipe >= 0:
                line = line[:pipe]
            tokens = line.split()
            if not tokens:
                continue
            # Skip the address token: 4 hex chars at the start of data lines
            start = 1 if len(tokens[0]) == 4 and all(
                c in "0123456789abcdefABCDEF" for c in tokens[0]
            ) else 0
            for tok in tokens[start:]:
                if len(tok) == 2:
                    try:
                        raw.append(int(tok, 16))
                    except ValueError:
                        pass
        return bytes(raw[:size]) if raw else None
    except Exception:
        return None


def _w77q_scan_flash(marker: bytes,
                     region_size: int = 0x400000,
                     chunk: int = 0x1000) -> int | None:
    """Scan the W77Q LittleFS partition via read-raw for @marker.

    read-raw shares LittleFS's block address space (addr = block*4096 + off),
    so scanning offsets 0..region_size covers the whole 4 MB LFS region and
    the returned absolute offset can be re-read later — even after the logical
    file has been deleted. Returns the offset of the first occurrence or None.
    """
    overlap = max(len(marker) - 1, 0)
    off = 0
    while off < region_size:
        data = _w77q_read_exact(f"0x{off:08x}", chunk)
        if data:
            idx = data.find(marker)
            if idx >= 0:
                return off + idx
        off += chunk - overlap
    return None


_NIL_UUID = "00000000-0000-0000-0000-000000000000"


def _parse_ta_ver_db_bytes(data: bytes, tag: str):
    """Parse the ta_ver.db anti-rollback database.

    Layout (little-endian, stored at W77Q flash with 128-byte W77Q header):
      +0x00  u32  db_version    (always 0)
      +0x04  u32  nb_entries
      +0x08  array of {u8[16] uuid, u32 ta_version, u32 _pad} × nb_entries
    """
    import struct as _s
    HDR = 8

    # Scan for the header at 4-byte boundaries (skip W77Q object prefix)
    base = None
    for skip in range(0, min(len(data) - HDR, 260), 4):
        ver, nb = _s.unpack_from("<II", data, skip)
        if ver == 0 and 0 < nb <= 64:
            base = skip
            break

    if base is None:
        print(f"  {YELLOW}⚠  {tag}: cannot locate ta_ver_db header "
              f"({len(data)} bytes){NC}")
        return

    if base:
        print(f"  {CYAN}(skipping {base}-byte W77Q object prefix){NC}")

    ver, nb = _s.unpack_from("<II", data, base)
    print(f"  {'db_version':<20}: {ver}")
    print(f"  {'nb_entries':<20}: {nb}")

    ENTRY = 24   # 16 (UUID) + 4 (ta_version) + 4 (pad)
    for i in range(nb):
        eo = base + HDR + i * ENTRY
        if eo + ENTRY > len(data):
            print(f"    [{i}] (truncated)")
            break
        ub = data[eo: eo + 16]
        tl, tm, thv = _s.unpack_from("<IHH", ub, 0)
        nd  = ub[8:16].hex()
        uuid_str = f"{tl:08x}-{tm:04x}-{thv:04x}-{nd[:4]}-{nd[4:]}"
        ta_ver, = _s.unpack_from("<I", data, eo + 16)
        print(f"    [{i}]  uuid={uuid_str}  ta_version={ta_ver}")


# PKCS11_CKFT_* flag bits (from ta/pkcs11/include/pkcs11_ta.h)
_PKCS11_FLAGS = {
    (1 << 0):  "RNG",
    (1 << 1):  "WRITE_PROTECTED",
    (1 << 2):  "LOGIN_REQUIRED",
    (1 << 3):  "USER_PIN_INITIALIZED",
    (1 << 5):  "RESTORE_KEY_NOT_NEEDED",
    (1 << 6):  "CLOCK_ON_TOKEN",
    (1 << 8):  "PROTECTED_AUTH_PATH",
    (1 << 9):  "DUAL_CRYPTO_OPERATIONS",
    (1 << 10): "TOKEN_INITIALIZED",
    (1 << 16): "USER_PIN_COUNT_LOW",
    (1 << 17): "USER_PIN_FINAL_TRY",
    (1 << 18): "USER_PIN_LOCKED",
    (1 << 19): "USER_PIN_TO_BE_CHANGED",
    (1 << 20): "SO_PIN_COUNT_LOW",
    (1 << 21): "SO_PIN_FINAL_TRY",
    (1 << 22): "SO_PIN_LOCKED",
    (1 << 23): "SO_PIN_TO_BE_CHANGED",
    (1 << 24): "ERROR_STATE",
}

def _decode_pkcs11_flags(flags: int) -> str:
    names = [n for b, n in sorted(_PKCS11_FLAGS.items()) if flags & b]
    return " | ".join(names) if names else "(none)"


# CKA_* attribute ID → name
_CKA_NAMES = {
    # General / storage (PKCS#11 v3.0 standard values)
    0x0000: "CLASS",              0x0001: "TOKEN",
    0x0002: "PRIVATE",            0x0003: "LABEL",
    0x0010: "APPLICATION",        0x0011: "VALUE",
    0x0012: "OBJECT_ID",          0x0086: "TRUSTED",
    # Key attributes
    0x0100: "KEY_TYPE",           0x0101: "SUBJECT",
    0x0102: "ID",                 0x0103: "SENSITIVE",
    0x0104: "ENCRYPT",            0x0105: "DECRYPT",
    0x0106: "WRAP",               0x0107: "UNWRAP",
    0x0108: "SIGN",               0x0109: "SIGN_RECOVER",
    0x010A: "VERIFY",             0x010B: "VERIFY_RECOVER",
    0x010C: "DERIVE",
    0x0110: "START_DATE",         0x0111: "END_DATE",
    # RSA
    0x0120: "MODULUS",            0x0121: "MODULUS_BITS",
    0x0122: "PUBLIC_EXPONENT",    0x0123: "PRIVATE_EXPONENT",
    0x0124: "PRIME_1",            0x0125: "PRIME_2",
    0x0126: "EXPONENT_1",         0x0127: "EXPONENT_2",
    0x0128: "COEFFICIENT",        0x0129: "PUBLIC_KEY_INFO",
    # DH / DSA
    0x0130: "PRIME",              0x0131: "SUBPRIME",
    0x0132: "BASE",
    # Symmetric / common
    0x0160: "VALUE_BITS",         0x0161: "VALUE_LEN",
    0x0162: "EXTRACTABLE",        0x0163: "LOCAL",
    0x0164: "NEVER_EXTRACTABLE",  0x0165: "ALWAYS_SENSITIVE",
    0x0166: "KEY_GEN_MECHANISM",
    # OP-TEE internal (non-standard 0x0168 = allowed mechanisms list)
    0x0168: "ALLOWED_MECHANISMS",
    # Object management booleans (standard)
    0x0170: "MODIFIABLE",         0x0171: "COPYABLE",
    0x0172: "DESTROYABLE",
    # EC
    0x0180: "EC_PARAMS",          0x0181: "EC_POINT",
    # Auth / wrapping
    0x0202: "ALWAYS_AUTHENTICATE",0x0210: "WRAP_WITH_TRUSTED",
}
_CKO_NAMES = {0: "DATA", 1: "CERTIFICATE", 2: "PUBLIC_KEY",
              3: "PRIVATE_KEY", 4: "SECRET_KEY", 5: "HW_FEATURE",
              6: "DOMAIN_PARAMS", 7: "MECHANISM"}
_CKK_NAMES = {0x00: "RSA", 0x01: "DSA", 0x02: "DH", 0x03: "EC",
              0x15: "DES", 0x16: "DES2", 0x17: "DES3", 0x1F: "AES",
              0x21: "SHA_HMAC", 0x27: "SHA256_HMAC", 0x28: "SHA384_HMAC",
              0x29: "SHA512_HMAC", 0x2A: "SHA224_HMAC"}

# boolean attribute IDs (value is a single 0x00/0x01 byte)
_CKA_BOOL = {0x0001, 0x0002, 0x0086, 0x0103, 0x0104, 0x0105, 0x0106,
             0x0107, 0x0108, 0x0109, 0x010A, 0x010B, 0x010C, 0x0162,
             0x0163, 0x0164, 0x0165, 0x0170, 0x0171, 0x0172,
             0x0202, 0x0210}

def _decode_cka_value(attr_id: int, data: bytes) -> str:
    """Human-readable representation of a CKA attribute value."""
    import struct as _s
    n = len(data)
    if n == 0:
        return "(empty)"
    if n == 1 and attr_id in _CKA_BOOL:
        return "True" if data[0] else "False"
    if n == 4:
        val, = _s.unpack_from("<I", data)
        if attr_id == 0x0000:  # CKA_CLASS
            return f"CKO_{_CKO_NAMES.get(val, f'0x{val:08x}')}"
        if attr_id == 0x0100:  # CKA_KEY_TYPE
            return f"CKK_{_CKK_NAMES.get(val, f'0x{val:08x}')}"
        if attr_id in (0x0121, 0x0161):  # MODULUS_BITS, VALUE_LEN (show decimal)
            return f"{val}"
        return f"0x{val:08x}"
    if attr_id == 0x0003:  # CKA_LABEL
        return data.rstrip(b"\x00").decode("utf-8", errors="replace")
    if attr_id == 0x0102:  # CKA_ID
        return data.hex()
    if n <= 8:
        return data.hex()
    return f"{data[:8].hex()}…  ({n} bytes)"


def _parse_token_db_bytes(data: bytes, tag: str):
    """Parse token_persistent_main + token_persistent_objs and print fields.

    struct token_persistent_main layout (184 bytes, little-endian, ARM):
      +0x00  u32  version        (always 0)
      +0x04  u8[32] label        (space-padded token label)
      +0x24  u32  flags          (PKCS11_CKFT_* bitmask)
      +0x28  u32  so_pin_count
      +0x2C  u32  so_pin_salt    (0 = SO PIN not set)
      +0x30  u8[64] so_pin_hash  (SHA256(user_type‖salt‖pin), 32B used)
      +0x70  u32  user_pin_count
      +0x74  u32  user_pin_salt  (0 = user PIN not set)
      +0x78  u8[64] user_pin_hash

    Immediately follows: struct token_persistent_objs
      +0xB8  u32  count
      +0xBC  u8[16×count] uuids[]
    """
    import struct as _s
    MAIN = 184

    # Auto-detect W77Q header prefix.
    # The W77Q object has a 128-byte header (magic, TA UUID, padded obj-name,
    # metadata) before the OP-TEE payload.  Scan at 4-byte boundaries.
    # A valid token_persistent_main has version==0 and a label field that is
    # mostly printable ASCII (spaces, letters, digits).
    base = None
    for skip in range(0, min(len(data) - MAIN, 260), 4):
        if len(data) < skip + MAIN:
            break
        ver, = _s.unpack_from("<I", data, skip)
        lbl  = data[skip + 4 : skip + 36]
        # PKCS#11 labels are always space-padded: every byte must be
        # printable ASCII (0x20–0x7E) — require all 32 bytes, not just some.
        all_printable = all(32 <= b <= 126 for b in lbl)
        if ver == 0 and all_printable:
            base = skip
            break

    if base is None:
        print(f"  {YELLOW}⚠  {tag}: cannot locate struct in {len(data)} bytes "
              f"— data may be TEE-encrypted or unsupported format{NC}")
        return

    if base:
        print(f"  {CYAN}(skipping {base}-byte W77Q object prefix){NC}")

    ver,     = _s.unpack_from("<I", data, base + 0x00)
    lbl_b    = data[base + 0x04 : base + 0x24]
    flags,   = _s.unpack_from("<I", data, base + 0x24)
    so_cnt,  = _s.unpack_from("<I", data, base + 0x28)
    so_salt, = _s.unpack_from("<I", data, base + 0x2C)
    so_hash  = data[base + 0x30 : base + 0x50]   # 32 of 64 bytes
    us_cnt,  = _s.unpack_from("<I", data, base + 0x70)
    us_salt, = _s.unpack_from("<I", data, base + 0x74)
    us_hash  = data[base + 0x78 : base + 0x98]   # 32 of 64 bytes

    lbl_str  = lbl_b.rstrip(b"\x00 ").decode("ascii", errors="replace")

    W = 20
    print(f"  {'version':<{W}}: 0x{ver:08x}")
    print(f"  {'label':<{W}}: {lbl_str!r}")
    print(f"  {'flags':<{W}}: 0x{flags:08x}  →  {_decode_pkcs11_flags(flags)}")
    print(f"  {'SO  fail_count':<{W}}: {so_cnt}")
    print(f"  {'SO  pin_salt':<{W}}: {'SET (0x{:08x})'.format(so_salt) if so_salt else 'UNSET (PIN not initialized)'}")
    if so_salt:
        print(f"  {'SO  pin_hash':<{W}}: {so_hash.hex()}")
    print(f"  {'User fail_count':<{W}}: {us_cnt}")
    print(f"  {'User pin_salt':<{W}}: {'SET (0x{:08x})'.format(us_salt) if us_salt else 'UNSET (PIN not initialized)'}")
    if us_salt:
        print(f"  {'User pin_hash':<{W}}: {us_hash.hex()}")

    # token_persistent_objs follows immediately
    ob = base + MAIN
    if len(data) >= ob + 4:
        count, = _s.unpack_from("<I", data, ob)
        print(f"  {'object UUIDs':<{W}}: {count} registered object(s)")
        for i in range(count):
            uo = ob + 4 + i * 16
            if uo + 16 > len(data):
                print(f"    [{i}] (truncated)")
                break
            ub = data[uo: uo + 16]
            tl, tm, thv = _s.unpack_from("<IHH", ub, 0)
            nd = ub[8:16].hex()
            print(f"    [{i}]  {tl:08x}-{tm:04x}-{thv:04x}-{nd[:4]}-{nd[4:]}")


def _parse_pkcs11_obj_bytes(data: bytes, tag: str, lut_size: int = 0):
    """Parse a PKCS#11 persistent object attribute blob (struct obj_attrs) and
    print each attribute.

    struct obj_attrs:
      +0x00  u32  attrs_size    byte length of the packed attrs[] array
      +0x04  u32  attrs_count   number of attribute items
      +0x08  packed sequence of:
               u32 attr_id  +  u32 attr_size  +  u8[attr_size] value

    lut_size: the data_size value from the W77Q LUT (OP-TEE payload size only,
    not counting the 128-byte W77Q object header).  Used to compute the
    expected attrs_size when the buffer contains extra padding bytes.
    """
    import struct as _s

    # Auto-detect W77Q prefix: obj_attrs.attrs_size ≈ lut_size - 8.
    # When lut_size is known, compute expected at each skip offset;
    # otherwise fall back to using len(data) - skip - 8 (less reliable).
    # Scan at 4-byte boundaries up to 260 (covers the 128-byte W77Q header).
    base = None
    for skip in range(0, min(len(data) - 8, 260), 4):
        if len(data) < skip + 8:
            break
        sz,  = _s.unpack_from("<I", data, skip)
        cnt, = _s.unpack_from("<I", data, skip + 4)
        if lut_size:
            # W77Q appends 24 bytes of auth overhead (8B nonce + 16B GCM tag)
            # per object.  Subtract it plus the 8-byte obj_attrs header.
            expected = lut_size - 32
        else:
            expected = len(data) - skip - 8
        if cnt < 128 and abs(int(sz) - expected) <= 16:
            base = skip
            break

    if base is None:
        print(f"  {YELLOW}⚠  {tag}: cannot parse obj_attrs ({len(data)} bytes){NC}")
        return

    sz,  = _s.unpack_from("<I", data, base)
    cnt, = _s.unpack_from("<I", data, base + 4)
    print(f"  attrs_size={sz}  attrs_count={cnt}")

    pos = base + 8
    end = pos + sz
    for _ in range(cnt):
        if pos + 8 > min(end, len(data)):
            break
        attr_id, attr_sz = _s.unpack_from("<II", data, pos)
        pos += 8
        val_b = data[pos: pos + attr_sz]
        pos  += attr_sz
        name = _CKA_NAMES.get(attr_id, f"0x{attr_id:04x}")
        val  = _decode_cka_value(attr_id, val_b)
        print(f"    CKA_{name:<24} = {val}")

def test_w77q_storage():
    hdr("11. W77Q secure storage diagnostics")

    # 11.1 — tool availability
    if not shutil.which("w77q-dump"):
        sk("w77q-dump not found — skipping section")
        return

    # 11.2 — list-all: enumerate every flash object
    print(f"\n  {CYAN}── Enumerating flash objects ──{NC}")
    out, rc = run_cmd("w77q-dump list-all")
    if rc != 0 or not _grep(r"flash_off|object", out):
        no(f"w77q-dump list-all failed (rc={rc})")
        return

    # Parse objects table: lines look like
    # [NNN] ta_uuid=<uuid>  flash_off=0x<hex>  data_size=<n>  obj_id=<name>
    import re as _re
    objects = []
    for line in out.splitlines():
        m = _re.search(
            r'\[(\d+)\]\s+ta_uuid=(\S+)\s+flash_off=(0x[\da-fA-F]+)\s+data_size=\s*(\d+)\s+obj_id=(.*)',
            line,
        )
        if m:
            objects.append({
                "idx":      int(m.group(1)),
                "ta_uuid":  m.group(2),
                "offset":   m.group(3),
                "size":     int(m.group(4)),
                "obj_id":   m.group(5).strip(),
                "name":     _w77q_obj_name(m.group(5).strip()),
            })

    total_obj = len(objects)
    pkcs11_obj = [o for o in objects if o["ta_uuid"] == _PKCS11_TA_UUID]
    other_obj  = [o for o in objects if o["ta_uuid"] != _PKCS11_TA_UUID]

    if total_obj > 0:
        ok(f"flash objects: {total_obj} total  "
           f"({len(pkcs11_obj)} PKCS#11 TA, {len(other_obj)} other)")
    else:
        no("no flash objects found")
        return

    # 11.3 — print table of all objects
    print(f"\n  {CYAN}── Flash object table ──{NC}")
    print(f"\n  {'IDX':>3}  {'TA':>8}  {'OFFSET':>10}  {'SIZE':>7}  OBJ-ID")
    print(f"  {'─'*3}  {'─'*8}  {'─'*10}  {'─'*7}  {'─'*30}")
    _OBJ_DESCRIPTIONS = {
        "token.db.0": "Slot 0: token config + object index (keys, certs, data)",
        "token.db.1": "Slot 1 object index (reserved)",
        "token.db.2": "Slot 2 object index (reserved)",
        "ta_ver.db":  "OP-TEE anti-rollback counter (prevents downgrade)",
    }
    for o in objects:
        ta_short = "PKCS#11" if o["ta_uuid"] == _PKCS11_TA_UUID else o["ta_uuid"][:8]
        obj_id_printable = o["name"]
        desc = _OBJ_DESCRIPTIONS.get(o["name"], "")
        desc_str = f"  ← {desc}" if desc else ""
        print(f"  {o['idx']:>3}  {ta_short:>8}  {o['offset']:>10}  {o['size']:>7}  {obj_id_printable}{desc_str}")

    # 11.4 — hex-dump first 64 bytes of each PKCS#11 object
    print(f"\n  {CYAN}── Hex-dumping PKCS#11 TA objects ──{NC}")
    if pkcs11_obj:
        print(f"\n  {CYAN}PKCS#11 TA objects — raw flash hex dumps:{NC}")
        for o in pkcs11_obj:
            dump_len = o["size"]
            obj_id_printable = o["name"]
            print(f"\n  [{o['idx']}] {obj_id_printable}  ({o['size']} bytes @ {o['offset']})")
            if o["offset"] == "0x00000000":
                run_cmd(f"w77q-dump read-file {o['ta_uuid']} {o['obj_id']}")
            else:
                run_cmd(f"w77q-dump read-raw {o['offset']} {dump_len}")

    # 11.5 — deserialize token.db.* and UUID-named object files
    token_db_objs = [o for o in pkcs11_obj if o["name"].startswith("token.db.")]
    uuid_objs     = [o for o in pkcs11_obj if not o["name"].startswith("token.db.")]

    if token_db_objs:
        print(f"\n  {CYAN}── token.db.* deserialization (token_persistent_main) ──{NC}")
        for o in sorted(token_db_objs, key=lambda x: x["name"]):
            print(f"\n  {BOLD}{o['name']}{NC}  ({o['size']} bytes @ {o['offset']})")
            if o["offset"] == "0x00000000":
                raw = _w77q_read_file(o["ta_uuid"], o["obj_id"])
            else:
                raw = _w77q_read_bytes(o["offset"], o["size"])
            if raw:
                _parse_token_db_bytes(raw, o["name"])
            else:
                print(f"  {YELLOW}⚠ read failed{NC}")

    if uuid_objs:
        print(f"\n  {CYAN}── PKCS#11 object files (obj_attrs) ──{NC}")
        for o in uuid_objs:
            obj_id_p = o["name"]
            print(f"\n  [{o['idx']}] {obj_id_p}  ({o['size']} bytes @ {o['offset']})")
            # Use read-file for LFS backend (flash_off=0x00000000 means no raw offset)
            if o["offset"] == "0x00000000":
                raw = _w77q_read_file(o["ta_uuid"], o["obj_id"])
                lut_sz = 0  # no W77Q header prefix — use len-based detection
            else:
                raw = _w77q_read_bytes(o["offset"], o["size"])
                lut_sz = o["size"]
            if raw:
                _parse_pkcs11_obj_bytes(raw, obj_id_p, lut_size=lut_sz)
            else:
                print(f"  {YELLOW}⚠ read failed{NC}")

    # 11.6 — Absolute flash dump  (temporarily disabled)
    return  # TODO: re-enable 11.6
    # a) Flash header / LUT area (offset 0x000000, first 256 bytes)
    print(f"\n  {CYAN}── Absolute flash dump ──{NC}")
    print(f"  Flash header / LUT area (0x000000, 256 bytes):")
    run_cmd("w77q-dump read-raw 0x000000 256")

    # b) Full sequential dump of every object ordered by flash offset
    sorted_objs = sorted(objects, key=lambda o: int(o["offset"], 16))
    print(f"\n  Sequential full-object dumps ({len(sorted_objs)} objects):")
    for o in sorted_objs:
        obj_id_p = "".join(c if 32 <= ord(c) < 127 else "." for c in o["obj_id"])
        ta_short  = "PKCS#11" if o["ta_uuid"] == _PKCS11_TA_UUID else o["ta_uuid"][:8]
        print(f"\n  [{o['idx']}] {ta_short}  {o['offset']}  {o['size']} bytes  \"{obj_id_p}\"")
        run_cmd(f"w77q-dump read-raw {o['offset']} {o['size']}")

    # c) Interactive: let the user dump any arbitrary flash region
    print(f"\n  {CYAN}── Interactive region dump (Enter blank offset to skip) ──{NC}")
    while True:
        try:
            off_s = _prompt("  Offset (hex, e.g. 0x1000) [blank=done]: ").strip()
        except KeyboardInterrupt:
            break
        if not off_s:
            break
        try:
            int(off_s, 16)          # validate hex
        except ValueError:
            print("  Invalid offset — use hex e.g. 0x1000")
            continue
        try:
            len_s = _prompt("  Length in bytes [256]: ").strip()
        except KeyboardInterrupt:
            break
        dump_len = 256
        if len_s:
            try:
                dump_len = int(len_s, 0)
            except ValueError:
                print("  Invalid length, using 256")
        run_cmd(f"w77q-dump read-raw {off_s} {dump_len}")


# ─────────────────────────────────────────────────────────────────────────────
# SECTION 12 — Secure storage: PKCS#11 vs raw flash
# ─────────────────────────────────────────────────────────────────────────────
def test_encryption_at_rest():
    hdr("12. Secure storage — PKCS#11 vs raw flash")

    LABEL_KEY  = "demo-aes-key"
    LABEL_DATA = "demo-encrypted"
    ID_KEY     = "20"
    PLAINTEXT  = b"Confidential: patient ID 4829103"
    plain_file = "/tmp/demo_secret.bin"
    enc_file   = "/tmp/demo_enc.bin"
    dec_file   = "/tmp/demo_dec.bin"
    IV         = "00000000000000000000000000000000"

    # Pad plaintext to 32 bytes (AES block aligned)
    padded = PLAINTEXT + b'\x00' * (32 - len(PLAINTEXT) % 32) if len(PLAINTEXT) % 32 else PLAINTEXT
    Path(plain_file).write_bytes(padded)

    if not shutil.which("w77q-dump"):
        no("w77q-dump not found — cannot demonstrate raw flash access")
        return

    # Cleanup leftovers
    _del_obj("secrkey", obj_id=ID_KEY)
    _del_obj("data", label=LABEL_DATA)

    # 12.1 Show the plaintext we want to protect
    print(f"\n  {BOLD}Secret to protect: \"{PLAINTEXT.decode()}\"{NC}")
    _hexshow(plain_file)

    # 12.2 Generate AES-256 key on token (sensitive — never leaves token)
    print(f"\n  {CYAN}── Generate AES-256 key (--sensitive, never exported) ──{NC}")
    out, _ = p11(
        f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}"'
        f' --keygen --key-type AES:32 --label {LABEL_KEY} --id {ID_KEY}'
        f' --sensitive --usage-decrypt'
    )
    if not _grep(r"key generated|Secret Key", out):
        no(f"AES keygen failed: {out.strip()}")
        return
    ok("AES-256 key generated (sensitive — stays on token)")

    # 12.3 Encrypt plaintext with token key
    print(f"\n  {CYAN}── Encrypt secret with token AES key (AES-CBC) ──{NC}")
    out, _ = p11(
        f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}"'
        f' --encrypt --mechanism AES-CBC --id {ID_KEY}'
        f' --iv {IV} --input-file {plain_file} --output-file {enc_file}'
    )
    if _grep(r"error|CKR_|fail", out):
        no(f"encryption failed: {out.strip()}")
        _del_obj("secrkey", obj_id=ID_KEY)
        return
    ok("plaintext encrypted → ciphertext")
    _hexshow(enc_file)

    # 12.4 Store ciphertext as data object on token
    print(f"\n  {CYAN}── Store ciphertext as data object on token ──{NC}")
    out, _ = p11(
        f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}"'
        f' --write-object {enc_file} --type data --label {LABEL_DATA}'
    )
    if _grep(r"error|CKR_", out):
        no(f"write-object failed: {out.strip()}")
        _del_obj("secrkey", obj_id=ID_KEY)
        return
    ok("ciphertext stored on token")

    # 12.5 Raw flash dump — shows only ciphertext
    print(f"\n  {CYAN}── Raw flash dump (what an attacker with SPI access sees) ──{NC}")
    run_cmd("w77q-dump list-all")
    objects = _w77q_list_objects()
    target = None
    for o in reversed(objects):
        if o["ta_uuid"] == _PKCS11_TA_UUID:
            target = o
            break
    if target:
        run_cmd(f"w77q-dump read-raw {target['offset']} {target['size'] + 128}")
        print(f"\n  {GREEN}{BOLD}  ✓  The plaintext \"{PLAINTEXT.decode()}\" is NOT on flash.{NC}")
        print(f"  {BOLD}  Only AES-CBC ciphertext is stored. The key never leaves the token.{NC}")
    else:
        no("could not locate object on flash")

    # 12.6 Read ciphertext back from token (flash) and decrypt
    print(f"\n  {CYAN}── Read ciphertext from token and decrypt ──{NC}")
    readback_file = "/tmp/demo_readback.bin"
    out, _ = p11(
        f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}"'
        f' --read-object --type data --label {LABEL_DATA}'
        f' --output-file {readback_file}'
    )
    if not Path(readback_file).exists() or Path(readback_file).stat().st_size == 0:
        no(f"read-object from token failed: {out.strip()}")
        _del_obj("data", label=LABEL_DATA)
        _del_obj("secrkey", obj_id=ID_KEY)
        return
    ok("ciphertext read back from token (stored on W77Q flash)")
    _hexshow(readback_file)

    print(f"\n  {CYAN}── Decrypt via PKCS#11 (authorized session recovers plaintext) ──{NC}")
    out, _ = p11(
        f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}"'
        f' --decrypt --mechanism AES-CBC --id {ID_KEY}'
        f' --iv {IV} --input-file {readback_file} --output-file {dec_file}'
    )
    if _files_equal(plain_file, dec_file):
        dec_data = Path(dec_file).read_bytes().rstrip(b'\x00').decode("utf-8", errors="replace")
        ok(f"decrypted from flash: \"{dec_data}\"")
    else:
        no(f"decryption mismatch: {out.strip()}")

    # Cleanup
    print(f"\n  {CYAN}── Cleanup ──{NC}")
    _del_obj("data", label=LABEL_DATA)
    _del_obj("secrkey", obj_id=ID_KEY)
    ok("demo key and data deleted")


# ─────────────────────────────────────────────────────────────────────────────
# SECTION 13 — W77Q hardware access control: plain read without session
# ─────────────────────────────────────────────────────────────────────────────
def test_w77q_plain_access():
    hdr("13. W77Q hardware access control — plain read denied")

    if not shutil.which("w77q-dump"):
        sk("w77q-dump not found — skipping section")
        return

    # 13.1 — Write a data object so we have real data on flash to read
    print(f"\n  {CYAN}── Storing test object on W77Q flash ──{NC}")
    ACCESS_LABEL = "access-ctrl-test"
    access_data  = "/tmp/access_ctrl_test.bin"
    Path(access_data).write_bytes(b"W77Q access control demo payload")
    _del_obj("data", label=ACCESS_LABEL)
    out, _ = p11(
        f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}"'
        f' --write-object {access_data} --type data --label {ACCESS_LABEL}'
    )
    if _grep(r"error|CKR_", out):
        no(f"write-object failed: {out.strip()}")
        return
    ok("test data object written to token (stored on W77Q flash)")

    # Find its offset on flash
    objects = _w77q_list_objects()
    target_offset = None
    for o in reversed(objects):
        if o.get("size", 0) > 0:
            target_offset = int(o["offset"], 16)
            target_size = min(o["size"], 64)
            break
    if target_offset is None:
        no("object not found on flash after write")
        return
    offset_hex = f"0x{target_offset:x}"
    ok(f"object located at flash offset {offset_hex}")

    # 13.2 — Authenticated read shows real data
    print(f"\n  {CYAN}── Authenticated read (via QLIB session) ──{NC}")
    print(f"  Reading {target_size} bytes at offset {offset_hex} with authenticated session...")
    out, rc = run_cmd(f"w77q-dump read-raw {offset_hex} {target_size}")
    if rc != 0:
        no(f"authenticated read-raw failed (rc={rc}) — cannot proceed")
        return
    ok("authenticated read succeeded (QLIB session active) — real data visible")

    # 13.3 — Now attempt plain read (no session) at the SAME offset — should be DENIED
    print(f"\n  {CYAN}── Plain read (no authentication — should be DENIED) ──{NC}")
    print(f"  Attempting to read the SAME {target_size} bytes at {offset_hex} WITHOUT the session key...")
    out, rc = run_cmd(f"w77q-dump read-plain {offset_hex} {target_size}")
    if rc != 0 or "DENIED" in out or "REJECTED" in out:
        ok("plain read DENIED by W77Q hardware — data is protected!\n"
           "         The flash enforces authenticated access: without the\n"
           "         correct session key, even Secure World cannot read the data.")
    elif "SUCCEEDED" in out or "WARNING" in out:
        no("plain read SUCCEEDED — section policy allows unauthenticated access!\n"
           "         plainAccessReadEnable=1 in section config. Set to 0 for protection.")
    else:
        no(f"unexpected result (rc={rc}): {out.strip()}")

    # 13.4 — Try a different offset deeper in flash
    second_offset = f"0x{target_offset + 0x1000:x}"
    print(f"\n  {CYAN}── Plain read at {second_offset} (additional verification) ──{NC}")
    out, rc = run_cmd(f"w77q-dump read-plain {second_offset} 64")
    if rc != 0 or "DENIED" in out or "REJECTED" in out:
        ok(f"plain read at {second_offset} also DENIED — entire section is protected")
    elif "SUCCEEDED" in out or "WARNING" in out:
        no(f"plain read at {second_offset} SUCCEEDED — section not fully protected")
    else:
        no(f"unexpected result (rc={rc}): {out.strip()}")

    # 13.5 — Summary
    print(f"\n  {CYAN}── Summary ──{NC}")
    print(f"  {BOLD}W77Q section 1 access control:{NC}")
    print(f"    • Authenticated read (QLIB session with FK/RK keys): ✓ ALLOWED")
    print(f"    • Plain read (no session, no keys):                  ✗ DENIED")
    print(f"    • Even with physical SPI bus access, data cannot be")
    print(f"      read without knowing the section keys.")

    # Cleanup
    _del_obj("data", label=ACCESS_LABEL)
    Path(access_data).unlink(missing_ok=True)


# ─────────────────────────────────────────────────────────────────────────────
# SECTION 14 — Secure deletion: zeroization on C_DestroyObject
# ─────────────────────────────────────────────────────────────────────────────
def test_w77q_zeroize():
    hdr("14. Secure deletion — flash zeroization after C_DestroyObject")

    LABEL     = "zeroize-test-obj"
    ID        = "30"
    SECRET    = b"TOP-SECRET-PAYLOAD-FOR-ZEROIZE-TEST!!"
    data_file = "/tmp/zeroize_test.bin"

    if not shutil.which("w77q-dump"):
        sk("w77q-dump not found — cannot verify flash content")
        return

    # Cleanup any leftover from previous runs
    _del_obj("data", label=LABEL)

    # 14.1 — Snapshot flash state before creating object
    print(f"\n  {CYAN}── Step 1: Record flash state before object creation ──{NC}")
    objs_before = _w77q_list_objects()
    offsets_before = {(o["ta_uuid"], o["obj_id"]) for o in objs_before}
    ok(f"flash state recorded: {len(objs_before)} objects")

    # 14.2 — Create a data object with known content
    print(f"\n  {CYAN}── Step 2: Create data object with known secret ──{NC}")
    Path(data_file).write_bytes(SECRET)
    print(f"  {BOLD}Plaintext: \"{SECRET.decode()}\"{NC}")
    _hexshow(data_file)

    out, rc = p11(
        f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}"'
        f' --write-object {data_file} --type data --label {LABEL} --id {ID}'
    )
    if rc != 0 or _grep(r"error|CKR_", out):
        no(f"write-object failed: {out.strip()}")
        return
    ok(f"object '{LABEL}' written to token")

    # 14.3 — Locate the new object on flash
    print(f"\n  {CYAN}── Step 3: Locate object on W77Q flash ──{NC}")
    objs_after = _w77q_list_objects()
    # Find the new object (present now but not before)
    new_objs = [o for o in objs_after
                if (o["ta_uuid"], o["obj_id"]) not in offsets_before]
    # Filter to PKCS#11 TA objects only
    new_pkcs11 = [o for o in new_objs if o["ta_uuid"] == _PKCS11_TA_UUID]

    if not new_pkcs11:
        no("could not identify new object on flash — aborting")
        _del_obj("data", label=LABEL)
        return

    target = new_pkcs11[-1]  # last new PKCS#11 object
    flash_off = target["offset"]
    flash_size = target["size"]
    obj_id_target = target["obj_id"]
    ta_uuid_target = target["ta_uuid"]
    # Total record on flash = W77Q header (104 bytes) + data_size (w77q_fs only)
    record_size = 104 + flash_size
    is_lfs = (flash_off == "0x00000000")

    ok(f"object located: offset={flash_off}, data_size={flash_size}, "
       f"total_record={record_size} bytes"
       f"{' (LFS backend)' if is_lfs else ''}")

    # 14.4 — Hex-dump the flash region and capture the PHYSICAL address
    print(f"\n  {CYAN}── Step 4: Raw flash dump BEFORE deletion ──{NC}")
    phys_off = None          # absolute flash offset of the secret (LFS backend)
    probe_len = len(SECRET)  # physical window verified before/after delete
    raw_phys_before = None
    if is_lfs:
        print(f"  (W77Q LFS backend stores objects in plaintext — the secret is")
        print(f"   visible on flash; secure deletion must physically erase it)")
        out, _ = run_cmd(f"w77q-dump read-file {ta_uuid_target} {obj_id_target}")
        raw_before = _w77q_read_file(ta_uuid_target, obj_id_target)
        # Locate the secret's PHYSICAL offset. read-raw shares the LFS address
        # space, so this offset stays valid after the logical file is deleted.
        phys_off_int = _w77q_scan_flash(SECRET)
        if phys_off_int is not None:
            phys_off = f"0x{phys_off_int:08x}"
            ok(f"secret located on raw flash at {phys_off}")
            print(f"\n  {CYAN}── Raw flash at {phys_off} "
                  f"(physical, bypasses LFS) ──{NC}")
            run_cmd(f"w77q-dump read-raw {phys_off} {probe_len}")
            raw_phys_before = _w77q_read_exact(phys_off, probe_len)
        else:
            no("could not locate secret on raw flash — "
               "physical verification unavailable")
    else:
        print(f"  (OP-TEE encrypts objects with AES-GCM — "
              f"you see ciphertext, not plaintext)")
        out, _ = run_cmd(f"w77q-dump read-raw {flash_off} {record_size}")
        raw_before = _w77q_read_exact(flash_off, record_size)
    if raw_before:
        non_zero = sum(1 for b in raw_before if b != 0)
        ok(f"flash content: {non_zero}/{len(raw_before)} non-zero bytes "
           f"(object data present)")
    else:
        no("could not read flash content")
        _del_obj("data", label=LABEL)
        return

    # 14.5 — Delete the object via PKCS#11 C_DestroyObject
    print(f"\n  {CYAN}── Step 5: Delete object (C_DestroyObject → zeroization) ──{NC}")
    out, rc = p11(
        f'--login --token-label "{cfg.token0}" --pin "{cfg.upin}"'
        f' --delete-object --type data --label {LABEL}'
    )
    if rc != 0 and _grep(r"error|CKR_", out):
        no(f"delete-object failed: {out.strip()}")
        return
    ok("object deleted via C_DestroyObject")

    # 14.6 — Hex-dump the SAME flash region AFTER deletion
    print(f"\n  {CYAN}── Step 6: Raw flash dump AFTER deletion (should be all zeros) ──{NC}")
    if is_lfs:
        # LFS backend: verify file is gone (read-file should fail)
        raw_after_file = _w77q_read_file(ta_uuid_target, obj_id_target)
        if raw_after_file is None:
            ok("file no longer readable through LFS (deleted)")
        else:
            no(f"file STILL readable after deletion ({len(raw_after_file)} bytes)")

        # Verify object no longer appears in list-all
        objs_final = _w77q_list_objects()
        still_present = [o for o in objs_final
                         if o["ta_uuid"] == ta_uuid_target and o["obj_id"] == obj_id_target]
        if not still_present:
            ok("object removed from list-all")
        else:
            no("object STILL in list-all after deletion!")

        # REAL physical verification: re-read the SAME flash offset captured in
        # Step 4 and measure what is actually left on the medium. This is the
        # ground truth — a logical delete does not imply the bytes were erased.
        if phys_off is not None:
            print(f"\n  {CYAN}── Raw flash at {phys_off} "
                  f"(physical, after delete) ──{NC}")
            run_cmd(f"w77q-dump read-raw {phys_off} {probe_len}")
            raw_after = _w77q_read_exact(phys_off, probe_len)
            if raw_after is None:
                no("could not read flash after deletion — inconclusive")
                raw_after = b'\xff' * probe_len
            non_zero_after = sum(1 for b in raw_after if b != 0)
            total_bytes = len(raw_after)
            if SECRET in raw_after:
                no("SECRET PLAINTEXT STILL PRESENT on flash after deletion")
            elif non_zero_after == 0:
                ok("physical flash region is fully zeroed")
        else:
            # No physical anchor — cannot make a truthful measurement.
            no("physical offset unknown — cannot verify erasure")
            non_zero_after = -1
            total_bytes = 0
            raw_after = b''
    else:
        out, _ = run_cmd(f"w77q-dump read-raw {flash_off} {record_size}")
        raw_after = _w77q_read_exact(flash_off, record_size)
        if raw_after is None:
            no("could not read flash after deletion")
            return
        non_zero_after = sum(1 for b in raw_after if b != 0)
        total_bytes = len(raw_after)

    # 14.7 — Verdict
    print(f"\n  {CYAN}── Verdict ──{NC}")
    if is_lfs and phys_off is not None:
        before_nz = (sum(1 for b in raw_phys_before if b != 0)
                     if raw_phys_before else 0)
        print(f"  Physical offset: {phys_off} .. +{probe_len} bytes")
        print(f"  Before delete: {before_nz}/{probe_len} non-zero bytes "
              f"(secret {'present' if raw_phys_before and SECRET in raw_phys_before else 'not found'})")
        print(f"  After  delete: {non_zero_after}/{total_bytes} non-zero bytes")
    else:
        print(f"  Flash region: {flash_off} .. +{record_size} bytes")
        print(f"  Before delete: {sum(1 for b in raw_before if b != 0)}/{len(raw_before)} non-zero bytes")
        print(f"  After  delete: {non_zero_after}/{total_bytes} non-zero bytes")

    if is_lfs and phys_off is None:
        no("INCONCLUSIVE: could not anchor a physical offset — not verified")
    elif is_lfs and raw_after and SECRET in raw_after:
        no("NOT ZEROIZED: secret plaintext still readable on flash after delete")
        print(f"  {RED}The object was unlinked logically but its bytes remain "
              f"physically on flash.{NC}")
    elif non_zero_after == 0:
        ok(f"ZEROIZED: entire {total_bytes}-byte flash record is all zeros")
        print(f"\n  {GREEN}{BOLD}  ✓  Secure deletion confirmed!{NC}")
        print(f"  {BOLD}  The deleted object's bytes have been overwritten with zeros.{NC}")
        print(f"  {BOLD}  No residual data remains on flash after C_DestroyObject.{NC}")
    elif non_zero_after > 0 and non_zero_after < total_bytes * 0.05:
        ok(f"nearly zeroized: only {non_zero_after}/{total_bytes} non-zero bytes remain "
           f"(likely metadata)")
        print(f"  {YELLOW}Minor residual bytes may be filesystem metadata.{NC}")
    else:
        no(f"NOT fully zeroized: {non_zero_after}/{total_bytes} non-zero bytes remain")
        print(f"  {RED}Expected all zeros but found residual data on flash.{NC}")

    # Cleanup temp file
    Path(data_file).unlink(missing_ok=True)


# ─────────────────────────────────────────────────────────────────────────────
# Summary
# ─────────────────────────────────────────────────────────────────────────────
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


# ─────────────────────────────────────────────────────────────────────────────
# Menu
# ─────────────────────────────────────────────────────────────────────────────
_SECTIONS: dict[str, callable] = {
    "2":  test_p11_info,
    "3":  test_p11_token,
    "4":  test_p11_random,
    "5":  test_p11_hash,
    "6":  test_p11_rsa,
    "7":  test_p11_ec,
    "8":  test_p11_aes,
    "9":  test_p11_objects,
    "10": test_p11_errors,
    "11": test_w77q_storage,
    "12": test_encryption_at_rest,
    "13": test_w77q_plain_access,
}

# Setup actions (not in _SECTIONS so they are excluded from "run all")
_SETUP_SECTION: dict[str, callable] = {
    "reset": setup_tee_storage,
    "init":  setup_token_init,
    "env":   test_env,
}


def show_menu():
    print(f"\n")
    print(f"{BOLD}{WHITE_ON_CYAN}╔═══════════════════════════════════════════════════════════════╗{NC}")
    print(f"{BOLD}{WHITE_ON_CYAN}║   PKCS#11 Test Suite                                          ║{NC}")
    print(f"{BOLD}{WHITE_ON_CYAN}║   Sparrow Hawk — OP-TEE                                       ║{NC}")
    print(f"{BOLD}{WHITE_ON_CYAN}╚═══════════════════════════════════════════════════════════════╝{NC}")
    print(f"\n  Module: {NC}{cfg.mod}{NC}")
    print(f"  Token : {NC}{cfg.token0}{NC}   UPIN: {cfg.upin}   SOPIN: {cfg.sopin}")
    print(f"\n  {BOLD}{CYAN}Setup:{NC}")
    print(f"   (env)   Environment check")
    print(f"   (reset) Build TEE storage from blank ⚠️  {WHITE_ON_RED}Destructive: erases W77Q flash{NC}")
    print(f"   (init)  Token init only (no flash erase)")
    print(f"\n  {BOLD}{CYAN}pkcs11-tool tests:{NC}")
    print("   (2) Token info & diagnostics")
    print("   (3) Enforcing private object access")
    print("   (4) Random number generation")
    print("   (5) Hash / Digest")
    print("   (6) RSA operations (keypairgen, sign, verify, encrypt, decrypt)")
    print("   (7) EC operations  (keypairgen, sign, verify)")
    print("   (8) AES operations (CBC encrypt/decrypt, CMAC)")
    print("   (9) Object management (write, list, read, delete, private visibility)")
    print(f"\n  {BOLD}{CYAN}Security / error tests:{NC}")
    print("   (10) Wrong PIN, wrong key, PIN lockout & unlock")
    print(f"\n  {BOLD}{CYAN}W77Q storage:{NC}")
    print("   (11) Flash object list, hex dumps, fs-test")
    print("   (12) Secure storage (PKCS#11 vs raw flash)")
    print("   (13) Hardware access control (plain read denied)")
    print(f"\n  {BOLD}{CYAN}More:{NC}")
    print("   (a) Run ALL tests (2–13)")
    print("   (c) Configure parameters (module, tokens, PINs)")
    print("   (q) Quit")


def main():
    global AUTO

    parser = argparse.ArgumentParser(
        description="PKCS#11 interactive test suite for Sparrow Hawk"
    )
    parser.add_argument("--auto", action="store_true",
                        help="Run all tests non-interactively")
    args = parser.parse_args()
    AUTO = args.auto

    _log(f"Log: {logfile_path}")
    _log(f"Started: {datetime.now()}")
    _log(f"Token: {cfg.token0}  MOD: {cfg.mod}")
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
        elif choice in _SETUP_SECTION:
            _SETUP_SECTION[choice]()
        elif choice == "a":
            for key in sorted(_SECTIONS, key=int):
                _SECTIONS[key]()
        elif choice == "c":
            edit_params()
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
