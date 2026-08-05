#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""
build_tee_storage.py — Build OP-TEE PKCS#11 token from blank on Sparrow Hawk.

Uses pkcs11-tool --init-token (PKCS#11 C_InitToken) to reset the token and
wipe all objects, then sets the User PIN — no low-level flash erase required.

Steps:
  1. Environment check (tee-supplicant, /dev/tee0, libckteec, pkcs11-tool)
  2. Show current W77Q flash objects (before, informational)
  3. Confirm reset (interactive mode only)
  4. Ensure tee-supplicant is running
  5. pkcs11-tool --init-token  (detects existing vs fresh token; clears all objects)
  6. pkcs11-tool --init-pin    (sets User PIN)
  7. Verify: list slots, SO-login, user-login
  8. Show W77Q flash objects (after, informational)

Deploy as /usr/bin/build_tee_storage.py on the Sparrow Hawk board.
Usage:
  python3 build_tee_storage.py           # interactive with confirmation
  python3 build_tee_storage.py --auto    # non-interactive (skip prompts)
  python3 build_tee_storage.py --dry-run # print steps without executing
"""

import argparse
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
BOLD   = '\033[1m'
NC     = '\033[0m'

# ── Runtime-editable configuration ────────────────────────────────────────────
@dataclass
class Config:
    mod:    str = "/usr/lib/libckteec.so"
    token0: str = "optee-test-token"
    upin:   str = "87654321"
    sopin:  str = "12345678"

cfg = Config()

# Resolve module: fall back to .so.0 if plain .so is absent
if not Path(cfg.mod).exists() and Path(cfg.mod + ".0").exists():
    cfg.mod = cfg.mod + ".0"

# Pull values from tee-tests-common.sh if deployed on the board
_common = Path("/usr/bin/tee-tests-common.sh")
if _common.exists():
    _map = {"MOD": "mod", "TOKEN0": "token0", "UPIN": "upin", "SOPIN": "sopin"}
    for _line in _common.read_text().splitlines():
        for _var, _attr in _map.items():
            if _line.startswith(f"{_var}="):
                _val = _line.split("=", 1)[1]
                _val = re.sub(r'\s+#.*$', '', _val)
                _val = _val.strip().strip('"\'')
                if _val:
                    setattr(cfg, _attr, _val)

# ── Globals ───────────────────────────────────────────────────────────────────
AUTO    = False
DRY_RUN = False
_ok_count   = 0
_fail_count = 0
logfile_path = f"/tmp/build-tee-storage-{datetime.now().strftime('%Y%m%d-%H%M%S')}.log"

# ── Logging helpers ───────────────────────────────────────────────────────────
def _log(msg: str):
    with open(logfile_path, "a") as f:
        f.write(msg + "\n")

def ok(msg: str):
    global _ok_count
    _ok_count += 1
    print(f"  {GREEN}[ OK ]{NC}  {msg}")
    _log(f"[ OK ]  {msg}")

def fail(msg: str):
    global _fail_count
    _fail_count += 1
    print(f"  {RED}[FAIL]{NC}  {msg}")
    _log(f"[FAIL]  {msg}")

def info(msg: str):
    print(f"  {CYAN}[INFO]{NC}  {msg}")
    _log(f"[INFO]  {msg}")

def warn(msg: str):
    print(f"  {YELLOW}[WARN]{NC}  {msg}")
    _log(f"[WARN]  {msg}")

def hdr(msg: str):
    print(f"\n{BOLD}{CYAN}── {msg} ──{NC}")
    _log(f"\n── {msg} ──")

# ── Shell helpers ─────────────────────────────────────────────────────────────
def run_cmd(cmd: str, timeout: int = 30) -> tuple[str, int]:
    """Run a shell command, tee output to log, return (stdout+stderr, rc)."""
    _log(f"$ {cmd}")
    if DRY_RUN:
        print(f"    {YELLOW}[dry-run]{NC}  {cmd}")
        return "", 0
    result = subprocess.run(cmd, shell=True, capture_output=True, timeout=timeout)
    combined = result.stdout.decode("latin-1") + result.stderr.decode("latin-1")
    if combined.strip():
        for line in combined.splitlines():
            print(f"    {line}")
        _log(combined)
    return combined, result.returncode

def p11(args_str: str) -> tuple[str, int]:
    return run_cmd(f'pkcs11-tool --module "{cfg.mod}" {args_str}')

def _systemctl(action: str, service: str) -> int:
    _log(f"$ systemctl {action} {service}")
    if DRY_RUN:
        print(f"    {YELLOW}[dry-run]{NC}  systemctl {action} {service}")
        return 0
    rc = subprocess.run(["systemctl", action, service], capture_output=True).returncode
    return rc

def _is_active(service: str) -> bool:
    return subprocess.run(
        ["systemctl", "is-active", service, "-q"], capture_output=True
    ).returncode == 0

# ── Confirmation helper ───────────────────────────────────────────────────────
def _confirm(msg: str) -> bool:
    if AUTO:
        print(f"  {msg} {YELLOW}[auto: yes]{NC}")
        return True
    try:
        ans = input(f"  {msg} [y/N]: ").strip().lower()
    except (KeyboardInterrupt, EOFError):
        print()
        sys.exit(0)
    return ans in ("y", "yes")

# ── W77Q object listing ───────────────────────────────────────────────────────
def _w77q_list(label: str) -> None:
    """Print current W77Q flash object list."""
    out, rc = run_cmd("w77q-dump list-all")
    if rc == 0:
        lines = [l for l in out.splitlines() if l.strip()]
        count = sum(1 for l in lines if re.search(r'ta_uuid=', l))
        info(f"W77Q flash objects ({label}): {count}")
    else:
        warn("w77q-dump list-all failed")

# ── Build steps ───────────────────────────────────────────────────────────────
def step_env() -> bool:
    """Step 1: environment check."""
    hdr("1. Environment check")
    all_ok = True

    # tee-supplicant
    if _is_active("tee-supplicant"):
        ok("tee-supplicant is running")
    else:
        warn("tee-supplicant not running (will be started in step 3)")

    # /dev/tee0
    if Path("/dev/tee0").exists():
        ok("/dev/tee0 present")
    else:
        fail("/dev/tee0 missing — OP-TEE kernel driver not loaded")
        all_ok = False

    # libckteec
    if Path(cfg.mod).exists():
        ok(f"libckteec: {cfg.mod}")
    else:
        fail(f"libckteec not found at {cfg.mod}")
        all_ok = False

    # pkcs11-tool
    if shutil.which("pkcs11-tool"):
        ok("pkcs11-tool available")
    else:
        fail("pkcs11-tool not found")
        all_ok = False

    # w77q-dump (optional — used for before/after state display only)
    if shutil.which("w77q-dump"):
        ok("w77q-dump available (flash state display)")
    else:
        warn("w77q-dump not found — flash state display will be skipped")

    return all_ok

def step_show_before() -> None:
    """Step 2: show current flash state (informational)."""
    hdr("2. Current W77Q flash state (before)")
    if shutil.which("w77q-dump"):
        _w77q_list("before")
    else:
        info("w77q-dump not available — skipping")

def step_confirm() -> bool:
    """Step 3: confirm token reset."""
    hdr("3. Confirm token reset")
    print(f"\n  {RED}{BOLD}WARNING:{NC}  This will re-initialize the PKCS#11 token.")
    print(f"  Token : {YELLOW}{cfg.token0}{NC}")
    print(f"  SO-PIN: {cfg.sopin}   User-PIN: {cfg.upin}")
    print(f"  All PKCS#11 objects on this token will be wiped.\n")
    if not _confirm("Proceed with token re-initialization?"):
        print(f"  {YELLOW}Aborted by user.{NC}")
        return False
    return True

def step_ensure_supplicant() -> bool:
    """Step 4: restart tee-supplicant for a guaranteed clean TEE connection.

    A previous failed run may have stopped the supplicant and left it unable
    to serve user TA sessions (PKCS#11 TA is a user TA; it needs the
    supplicant to be fully registered with the kernel driver before any
    session can be opened).  Always do a clean stop→start and then poll
    until pkcs11-tool --list-slots succeeds — that confirms the supplicant
    is actually connected, not just that systemd says "active".
    """
    hdr("4. tee-supplicant (clean restart)")
    if _is_active("tee-supplicant"):
        info("Stopping tee-supplicant for clean restart…")
        _systemctl("stop", "tee-supplicant")
        time.sleep(0.5)

    rc = _systemctl("start", "tee-supplicant")
    if rc != 0:
        fail("systemctl start tee-supplicant failed")
        return False

    # Wait for active
    for _ in range(20):
        if _is_active("tee-supplicant"):
            break
        time.sleep(0.5)
    else:
        fail("tee-supplicant did not start within 10 s")
        return False

    # Wait until the PKCS#11 TA session can actually be opened.
    # systemd "active" is not enough — the supplicant needs a moment to
    # register with the TEE kernel driver so user TAs can be loaded.
    for attempt in range(10):
        time.sleep(1)
        out, rc_ls = p11("--list-slots")
        if rc_ls == 0 and "Slot" in out:
            ok(f"tee-supplicant ready (pkcs11-tool responded after {attempt + 1} s)")
            return True
        if attempt < 9:
            info(f"  waiting for PKCS#11 TA… ({attempt + 1}/10)")

    fail("tee-supplicant started but pkcs11-tool --list-slots still fails after 10 s")
    return False

def _token_slot() -> str | None:
    """Return the slot index string for cfg.token0, or None if not found."""
    out, rc = p11("--list-slots")
    if rc != 0:
        return None
    slot = None
    cur_slot = None
    for line in out.splitlines():
        m = re.match(r'\s*Slot\s+(\d+)', line)
        if m:
            cur_slot = m.group(1)
        if cfg.token0 in line and cur_slot is not None:
            slot = cur_slot
    return slot

def _first_initialized_slot() -> str | None:
    """Return first initialized slot index, or None if no initialized token exists."""
    out, rc = p11("--list-slots")
    if rc != 0:
        return None
    cur_slot = None
    for line in out.splitlines():
        m = re.match(r'\s*Slot\s+(\d+)', line)
        if m:
            cur_slot = m.group(1)
        if "token initialized" in line and cur_slot is not None:
            return cur_slot
    return None

def _pkcs11_clear_all_objects(slot_id: int) -> tuple[int, int]:
    """Delete every PKCS#11 object from slot_id via ctypes (user session + SO session).

    Returns (user_objects_deleted, so_objects_deleted).
    C_InitToken on OP-TEE can return CKR_GENERAL_ERROR when internal storage
    deletion fails (W77Q write protection), so we fall back to explicit
    C_DestroyObject calls which go through the normal TA write path.
    """
    import ctypes as _ct

    CKF_SERIAL_SESSION = 0x4
    CKF_RW_SESSION     = 0x2
    CKU_SO             = 0
    CKU_USER           = 1
    CKR_OK             = 0
    CKR_ALREADY_INIT   = 0x191
    MAX_OBJ            = 256

    try:
        lib = _ct.CDLL(cfg.mod)
        rc = lib.C_Initialize(None)
        if rc not in (CKR_OK, CKR_ALREADY_INIT):
            warn(f"C_Initialize: 0x{rc:x}")

        total_user = total_so = 0

        for login_type, pin_str, role in (
            (CKU_USER, cfg.upin,  "user"),
            (CKU_SO,   cfg.sopin, "SO"),
        ):
            session = _ct.c_ulong(0)
            rc = lib.C_OpenSession(
                _ct.c_ulong(slot_id),
                _ct.c_ulong(CKF_SERIAL_SESSION | CKF_RW_SESSION),
                None, None, _ct.byref(session),
            )
            if rc != CKR_OK:
                warn(f"C_OpenSession ({role}): 0x{rc:x}")
                continue

            pin_b = pin_str.encode()
            rc = lib.C_Login(
                session, _ct.c_ulong(login_type),
                pin_b,   _ct.c_ulong(len(pin_b)),
            )
            if rc != CKR_OK:
                warn(f"C_Login ({role}): 0x{rc:x}")
                lib.C_CloseSession(session)
                continue

            lib.C_FindObjectsInit(session, None, _ct.c_ulong(0))
            handles = (_ct.c_ulong * MAX_OBJ)()
            count   = _ct.c_ulong(0)
            lib.C_FindObjects(session, handles, _ct.c_ulong(MAX_OBJ), _ct.byref(count))
            lib.C_FindObjectsFinal(session)

            deleted = 0
            for i in range(count.value):
                drc = lib.C_DestroyObject(session, handles[i])
                if drc == CKR_OK:
                    deleted += 1
                else:
                    warn(f"C_DestroyObject handle {handles[i]}: 0x{drc:x}")

            if login_type == CKU_USER:
                total_user = deleted
            else:
                total_so = deleted

            lib.C_Logout(session)
            lib.C_CloseSession(session)

        lib.C_Finalize(None)
        return total_user, total_so

    except Exception as exc:
        warn(f"ctypes PKCS#11 clear failed: {exc}")
        return 0, 0

def step_init_token() -> bool:
    """Step 5: (re-)initialize PKCS#11 token, clearing all objects.

    Strategy:
      a) Fresh token (slot not yet initialized): pkcs11-tool --init-token on
         slot-index 0 (no SO PIN required).
      b) Existing token: try C_InitToken via pkcs11-tool first; if that
         returns CKR_GENERAL_ERROR (OP-TEE bug when flash-level delete fails),
         fall back to deleting all objects individually via ctypes.
    """
    hdr("5. (Re-)initialize PKCS#11 token")
    info(f"Label: {cfg.token0}   SO-PIN: {cfg.sopin}")

    slot = _token_slot()

    if slot is None:
        info("Token not found — initializing fresh token on slot-index 0")
        out, rc = p11(
            f'--slot-index 0 --init-token --label "{cfg.token0}" --so-pin "{cfg.sopin}"'
        )
        if rc == 0 or "successfully" in out.lower():
            ok(f"Token '{cfg.token0}' initialized (fresh)")
            return True
        fail(f"--init-token (fresh) failed (rc={rc}): {out.strip()[:120]}")
        return False

    # Token exists — try pkcs11-tool --init-token (C_InitToken) first
    slot_id = int(slot)
    info(f"Token already exists on slot {slot_id} — reinitializing (clears all objects)")
    out, rc = p11(
        f'--slot-index {slot_id} --init-token --label "{cfg.token0}" --so-pin "{cfg.sopin}"'
    )
    if rc == 0 or "successfully" in out.lower():
        ok(f"Token '{cfg.token0}' reinitialized via C_InitToken")
        return True

    # C_InitToken failed (CKR_GENERAL_ERROR from OP-TEE when internal storage
    # delete fails).  Fall back: delete all objects one-by-one via ctypes.
    warn(f"C_InitToken failed (rc={rc}) — falling back to per-object delete")
    n_user, n_so = _pkcs11_clear_all_objects(slot_id)
    ok(f"Deleted {n_user} user + {n_so} SO objects from token '{cfg.token0}'")

    # C_InitToken may partially reset metadata and drop the token label.
    # Ensure a labeled/initialized token exists for the next step.
    if _token_slot() is None:
        info(f"Token label '{cfg.token0}' not visible — re-initializing slot {slot_id}")
        out, rc = p11(
            f'--slot-index {slot_id} --init-token --label "{cfg.token0}" --so-pin "{cfg.sopin}"'
        )
        if rc == 0 or "successfully" in out.lower():
            ok(f"Token '{cfg.token0}' initialized after fallback")
            return True
        fail(f"--init-token recovery failed (rc={rc}): {out.strip()[:120]}")
        return False

    return True

def step_init_pin() -> bool:
    """Step 6: set / reset User PIN via SO login.

    Always calls --init-pin regardless of whether a PIN was previously set.
    After a fresh C_InitToken the user PIN is unset; after the ctypes
    fallback the old PIN is still set but we reset it to the configured
    value anyway so the token is in a known state.
    SO login opens an RW session which is closed cleanly on process exit,
    leaving no dangling read-only sessions in the OP-TEE TA.
    """
    hdr("6. Set User PIN")
    info(f"User-PIN: {cfg.upin}")

    slot = _token_slot()
    if slot is None:
        slot = _first_initialized_slot()

    if slot is not None:
        out, rc = p11(
            f'--slot-index {slot} --login --login-type so --so-pin "{cfg.sopin}"'
            f' --init-pin --pin "{cfg.upin}"'
        )
    else:
        out, rc = p11(
            f'--token-label "{cfg.token0}" --login --login-type so --so-pin "{cfg.sopin}"'
            f' --init-pin --pin "{cfg.upin}"'
        )
    if rc == 0 or "successfully" in out.lower():
        ok(f"User PIN set on token '{cfg.token0}'")
        return True
    fail(f"--init-pin failed (rc={rc}): {out.strip()[:120]}")
    return False

def _verify_so_login(slot_id: int) -> bool:
    """Verify SO PIN login via ctypes (avoids pkcs11-tool session leaks).

    OpenSC's pkcs11-tool opens read-only sessions during slot enumeration
    (PKCS11_enumerate_slots) that the OP-TEE PKCS#11 TA doesn't release on
    C_Finalize.  A dangling RO session causes C_Login(CKU_SO) to return
    CKR_SESSION_READ_ONLY_EXISTS.  Using ctypes directly we can open an
    explicit RW session, login, verify, and close it cleanly.
    """
    import ctypes as _ct
    CKF_SERIAL_SESSION = 0x4
    CKF_RW_SESSION     = 0x2
    CKU_SO             = 0
    CKR_OK             = 0
    CKR_ALREADY_INIT   = 0x191
    try:
        lib = _ct.CDLL(cfg.mod)
        rc = lib.C_Initialize(None)
        if rc not in (CKR_OK, CKR_ALREADY_INIT):
            warn(f"C_Initialize: 0x{rc:x}")
            return False

        session = _ct.c_ulong(0)
        rc = lib.C_OpenSession(
            _ct.c_ulong(slot_id),
            _ct.c_ulong(CKF_SERIAL_SESSION | CKF_RW_SESSION),
            None, None, _ct.byref(session),
        )
        if rc != CKR_OK:
            warn(f"C_OpenSession (SO verify): 0x{rc:x}")
            lib.C_Finalize(None)
            return False

        sopin_b = cfg.sopin.encode()
        rc = lib.C_Login(session, _ct.c_ulong(CKU_SO), sopin_b, _ct.c_ulong(len(sopin_b)))
        logged_in = (rc == CKR_OK)

        if logged_in:
            lib.C_Logout(session)
        lib.C_CloseSession(session)
        lib.C_Finalize(None)
        return logged_in

    except Exception as exc:
        warn(f"ctypes SO verify failed: {exc}")
        return False


def step_verify() -> bool:
    """Step 7: verify the initialized token."""
    hdr("7. Verify token")
    all_ok = True

    # List slots
    out, rc = p11("--list-slots")
    if rc == 0 and cfg.token0 in out:
        ok(f"Token '{cfg.token0}' appears in --list-slots")
    else:
        fail(f"Token not visible in --list-slots (rc={rc})")
        all_ok = False

    # SO-login via ctypes (avoids pkcs11-tool session leak causing
    # CKR_SESSION_READ_ONLY_EXISTS when login is attempted via pkcs11-tool)
    slot = _token_slot()
    slot_id = int(slot) if slot is not None else 0
    if _verify_so_login(slot_id):
        ok("SO PIN login accepted")
    else:
        fail("SO PIN login rejected")
        all_ok = False

    # User login via pkcs11-tool
    out, rc = p11(
        f'--token-label "{cfg.token0}" --login --pin "{cfg.upin}" --list-objects'
    )
    if rc == 0 and not re.search(r'CKR_PIN|incorrect', out, re.I):
        ok("User PIN login accepted (0 objects expected on blank token)")
    else:
        fail(f"User PIN login rejected (rc={rc}): {out.strip()[:80]}")
        all_ok = False

    return all_ok
    # User login
    out, rc = p11(
        f'--token-label "{cfg.token0}" --login --pin "{cfg.upin}" --list-objects'
    )
    if rc == 0 and not re.search(r'CKR_PIN|incorrect', out, re.I):
        ok("User PIN login accepted (0 objects expected on blank token)")
    else:
        fail(f"User PIN login rejected (rc={rc}): {out.strip()[:80]}")
        all_ok = False

    return all_ok

def step_show_after() -> None:
    """Step 8: show flash state after initialization."""
    hdr("8. W77Q flash state (after)")
    if shutil.which("w77q-dump"):
        _w77q_list("after")
    else:
        info("w77q-dump not available — skipping")

# ── Summary ───────────────────────────────────────────────────────────────────
def _print_summary() -> int:
    print(f"\n{BOLD}{'═'*46}{NC}")
    print(f"{BOLD}  Build TEE Storage — Summary{NC}")
    print(f"{BOLD}{'═'*46}{NC}")
    print(f"  {GREEN}OK  {NC}: {_ok_count}")
    print(f"  {RED}FAIL{NC}: {_fail_count}")
    print(f"\n  Log: {logfile_path}\n")
    if _fail_count == 0:
        print(f"  {GREEN}{BOLD}Token '{cfg.token0}' ready for testing.{NC}\n")
    else:
        print(f"  {RED}{BOLD}{_fail_count} step(s) FAILED.{NC}\n")
    return _fail_count

# ── Entry point ───────────────────────────────────────────────────────────────
def run_build(confirmed: bool = False) -> int:
    """Execute the full build-from-blank sequence.  Returns exit code (0=success)."""
    global AUTO

    _log(f"Started: {datetime.now()}")
    _log(f"Token: {cfg.token0}  MOD: {cfg.mod}  DRY_RUN: {DRY_RUN}")

    if not step_env():
        print(f"\n  {RED}Environment check failed — aborting.{NC}\n")
        return 1

    step_show_before()

    if not confirmed:
        if not step_confirm():
            return 0  # user aborted — not a failure

    if not step_ensure_supplicant():
        return 1

    if not step_init_token():
        return 1

    if not step_init_pin():
        return 1

    step_verify()
    step_show_after()

    return _print_summary()


def main():
    global AUTO, DRY_RUN

    parser = argparse.ArgumentParser(
        description="Build OP-TEE PKCS#11 token from blank on Sparrow Hawk"
    )
    parser.add_argument("--auto", action="store_true",
                        help="Non-interactive: skip all confirmation prompts")
    parser.add_argument("--dry-run", action="store_true",
                        help="Print steps without executing destructive commands")
    args = parser.parse_args()
    AUTO    = args.auto
    DRY_RUN = args.dry_run

    print(f"\n{BOLD}{CYAN}╔══════════════════════════════════════════════╗{NC}")
    print(f"{BOLD}{CYAN}║  Build TEE Storage from Blank                ║{NC}")
    print(f"{BOLD}{CYAN}║  Sparrow Hawk — W77Q / OP-TEE libckteec       ║{NC}")
    print(f"{BOLD}{CYAN}╚══════════════════════════════════════════════╝{NC}")
    print(f"\n  Module : {YELLOW}{cfg.mod}{NC}")
    print(f"  Token  : {YELLOW}{cfg.token0}{NC}")
    print(f"  SO-PIN : {cfg.sopin}   User-PIN: {cfg.upin}")
    print(f"  Log    : {logfile_path}")

    sys.exit(run_build())


if __name__ == "__main__":
    main()
