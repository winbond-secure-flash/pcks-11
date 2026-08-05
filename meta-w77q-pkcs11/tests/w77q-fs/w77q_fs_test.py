#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""
w77q_fs_test.py — Python port of the w77q-fs-test host application.

Uses ctypes to call libteec.so directly.  No compiled binary required.

Usage:
  w77q_fs_test.py                  Run all test cases (TC_ALL)
  w77q_fs_test.py basic            Run TC_BASIC only
  w77q_fs_test.py update           Run TC_UPDATE only
  w77q_fs_test.py rename           Run TC_RENAME only
  w77q_fs_test.py enum             Run TC_ENUM only
  w77q_fs_test.py large            Run TC_LARGE only
  w77q_fs_test.py overwrite        Run TC_OVERWRITE only
  w77q_fs_test.py truncate         Run TC_TRUNCATE only

Exit code: number of failed test cases (0 = all passed).

Tests exercise the w77q_fs secure-storage backend (TEE_STORAGE_PRIVATE)
running inside OP-TEE on the W77Q SPI NOR flash section.
TA UUID: f0e1d2c3-b4a5-9687-8796-a5b4c3d2e1f0
"""

import ctypes
import sys

# ---------------------------------------------------------------------------
# TEEC constants
# ---------------------------------------------------------------------------

TEEC_SUCCESS      = 0x00000000
TEEC_LOGIN_PUBLIC = 0x00000000
TEEC_NONE         = 0x00000000
TEEC_VALUE_INPUT  = 0x00000001
TEEC_VALUE_OUTPUT = 0x00000004

TEEC_ERROR_NOT_SUPPORTED = 0xffff000a  # GP TEE spec value


def TEEC_PARAM_TYPES(p0, p1, p2, p3):
    return p0 | (p1 << 4) | (p2 << 8) | (p3 << 12)


# ---------------------------------------------------------------------------
# TA command IDs  (must match w77q_fs_test_ta.h)
# ---------------------------------------------------------------------------

W77Q_FS_TC_BASIC     = 0
W77Q_FS_TC_UPDATE    = 1
W77Q_FS_TC_RENAME    = 2
W77Q_FS_TC_ENUM      = 3
W77Q_FS_TC_LARGE     = 4
W77Q_FS_TC_OVERWRITE = 5
W77Q_FS_TC_TRUNCATE  = 6
W77Q_FS_TC_ALL          = 7
W77Q_FS_TC_DIAG_CREATE  = 8   # create wft.diag, leave it for flash inspection
W77Q_FS_TC_DIAG_DELETE  = 9   # delete wft.diag
W77Q_FS_TC_DIAG_SETUP    = 10  # pre-create TC[n]'s objects; params[0].value.a=n
W77Q_FS_TC_DIAG_TEARDOWN = 11  # best-effort delete TC[n]'s objects
W77Q_FS_TC_COUNT        = 7

TC_NAMES = ['basic', 'update', 'rename', 'enum', 'large', 'overwrite', 'truncate']

# ---------------------------------------------------------------------------
# ctypes structure definitions
# ---------------------------------------------------------------------------

class TEEC_UUID(ctypes.Structure):
    _fields_ = [
        ('timeLow',          ctypes.c_uint32),
        ('timeMid',          ctypes.c_uint16),
        ('timeHiAndVersion', ctypes.c_uint16),
        ('clockSeqAndNode',  ctypes.c_uint8 * 8),
    ]


# TEEC_Context and TEEC_Session are implementation-defined (opaque).
# 64-byte buffers are large enough for any known OP-TEE build.

class TEEC_Context(ctypes.Structure):
    _fields_ = [('_opaque', ctypes.c_uint8 * 64)]


class TEEC_Session(ctypes.Structure):
    _fields_ = [('_opaque', ctypes.c_uint8 * 64)]


class _ValueParam(ctypes.Structure):
    _fields_ = [('a', ctypes.c_uint32), ('b', ctypes.c_uint32)]


class _TmpRefParam(ctypes.Structure):
    _fields_ = [('buffer', ctypes.c_void_p), ('size', ctypes.c_size_t)]


class _MemRefParam(ctypes.Structure):
    _fields_ = [
        ('parent', ctypes.c_void_p),
        ('size',   ctypes.c_size_t),
        ('offset', ctypes.c_size_t),
    ]


class TEEC_Parameter(ctypes.Union):
    _fields_ = [
        ('value',  _ValueParam),
        ('tmpref', _TmpRefParam),
        ('memref', _MemRefParam),
    ]


class TEEC_Operation(ctypes.Structure):
    _fields_ = [
        ('started',    ctypes.c_uint32),
        ('paramTypes', ctypes.c_uint32),
        ('params',     TEEC_Parameter * 4),
        ('_imp',       ctypes.c_uint8 * 64),   # implementation tail (e.g. semaphore)
    ]


# ---------------------------------------------------------------------------
# Load and configure libteec
# ---------------------------------------------------------------------------

def _load_libteec():
    for name in ('libteec.so.2', 'libteec.so'):
        try:
            return ctypes.CDLL(name)
        except OSError:
            continue
    raise OSError('libteec not found — is optee-client installed?')


def _setup_libteec(lib):
    lib.TEEC_InitializeContext.argtypes = [ctypes.c_char_p,
                                           ctypes.POINTER(TEEC_Context)]
    lib.TEEC_InitializeContext.restype  = ctypes.c_uint32

    lib.TEEC_FinalizeContext.argtypes = [ctypes.POINTER(TEEC_Context)]
    lib.TEEC_FinalizeContext.restype  = None

    lib.TEEC_OpenSession.argtypes = [
        ctypes.POINTER(TEEC_Context),
        ctypes.POINTER(TEEC_Session),
        ctypes.POINTER(TEEC_UUID),
        ctypes.c_uint32,               # connectionMethod
        ctypes.c_void_p,               # connectionData
        ctypes.POINTER(TEEC_Operation),
        ctypes.POINTER(ctypes.c_uint32),  # returnOrigin
    ]
    lib.TEEC_OpenSession.restype = ctypes.c_uint32

    lib.TEEC_CloseSession.argtypes = [ctypes.POINTER(TEEC_Session)]
    lib.TEEC_CloseSession.restype  = None

    lib.TEEC_InvokeCommand.argtypes = [
        ctypes.POINTER(TEEC_Session),
        ctypes.c_uint32,               # commandID
        ctypes.POINTER(TEEC_Operation),
        ctypes.POINTER(ctypes.c_uint32),  # returnOrigin
    ]
    lib.TEEC_InvokeCommand.restype = ctypes.c_uint32


# ---------------------------------------------------------------------------
# ANSI colour helpers
# ---------------------------------------------------------------------------

_COLOR = sys.stdout.isatty()


def _green(s): return f'\033[32m{s}\033[0m' if _COLOR else s
def _red(s):   return f'\033[31m{s}\033[0m' if _COLOR else s
def _bold(s):  return f'\033[1m{s}\033[0m'  if _COLOR else s


# ---------------------------------------------------------------------------
# TEE session helpers
# ---------------------------------------------------------------------------

_TA_UUID = TEEC_UUID(
    timeLow          = 0xf0e1d2c3,
    timeMid          = 0xb4a5,
    timeHiAndVersion = 0x9687,
    clockSeqAndNode  = (ctypes.c_uint8 * 8)(
        0x87, 0x96, 0xa5, 0xb4, 0xc3, 0xd2, 0xe1, 0xf0),
)


def open_session(lib):
    ctx  = TEEC_Context()
    sess = TEEC_Session()
    orig = ctypes.c_uint32(0)

    res = lib.TEEC_InitializeContext(None, ctypes.byref(ctx))
    if res != TEEC_SUCCESS:
        raise RuntimeError(f'TEEC_InitializeContext: 0x{res:08x}')

    res = lib.TEEC_OpenSession(
        ctypes.byref(ctx),
        ctypes.byref(sess),
        ctypes.byref(_TA_UUID),
        TEEC_LOGIN_PUBLIC,
        None,
        None,
        ctypes.byref(orig),
    )
    if res != TEEC_SUCCESS:
        lib.TEEC_FinalizeContext(ctypes.byref(ctx))
        raise RuntimeError(
            f'TEEC_OpenSession: 0x{res:08x} (orig 0x{orig.value:08x})')

    return ctx, sess


def close_session(lib, ctx, sess):
    lib.TEEC_CloseSession(ctypes.byref(sess))
    lib.TEEC_FinalizeContext(ctypes.byref(ctx))


# ---------------------------------------------------------------------------
# Test runners
# ---------------------------------------------------------------------------

def run_tc(lib, sess, cmd, name):
    op   = TEEC_Operation()
    orig = ctypes.c_uint32(0)
    op.paramTypes = TEEC_PARAM_TYPES(TEEC_NONE, TEEC_NONE, TEEC_NONE, TEEC_NONE)

    res = lib.TEEC_InvokeCommand(
        ctypes.byref(sess), cmd, ctypes.byref(op), ctypes.byref(orig))

    if res == TEEC_SUCCESS:
        print(f'  w77q-fs: {name:<12} {_green("PASS")}')
        return 0

    print(f'  w77q-fs: {name:<12} {_red("FAIL")}  (0x{res:08x})')
    return 1


def _run_all_batch(lib, sess):
    """Try the TC_ALL batch command (TA cmd 7); returns (fail_count, supported)."""
    op   = TEEC_Operation()
    orig = ctypes.c_uint32(0)
    op.paramTypes = TEEC_PARAM_TYPES(
        TEEC_VALUE_OUTPUT, TEEC_NONE, TEEC_NONE, TEEC_NONE)

    res  = lib.TEEC_InvokeCommand(
        ctypes.byref(sess), W77Q_FS_TC_ALL, ctypes.byref(op), ctypes.byref(orig))

    if res == TEEC_ERROR_NOT_SUPPORTED:
        return 0, False   # TA doesn't have TC_ALL — caller will fall back

    mask = op.params[0].value.a
    fail = 0
    for i, name in enumerate(TC_NAMES):
        failed = (mask >> i) & 1
        label  = _red('FAIL') if failed else _green('PASS')
        print(f'  w77q-fs: {name:<12} {label}')
        if failed:
            fail += 1

    if res != TEEC_SUCCESS and fail == 0:
        print(f'TC_ALL TA error: 0x{res:08x}', file=sys.stderr)
        fail = 1

    return fail, True


def run_all(lib, sess):
    fail, supported = _run_all_batch(lib, sess)
    if supported:
        return fail

    # TC_ALL not in this TA build — run each test case individually
    fail = 0
    for i, name in enumerate(TC_NAMES):
        fail += run_tc(lib, sess, i, name)
    return fail


def diag_create(lib, sess):
    """Create wft.diag and leave it in flash for inspection."""
    op   = TEEC_Operation()
    orig = ctypes.c_uint32(0)
    op.paramTypes = TEEC_PARAM_TYPES(TEEC_NONE, TEEC_NONE, TEEC_NONE, TEEC_NONE)
    res = lib.TEEC_InvokeCommand(
        ctypes.byref(sess), W77Q_FS_TC_DIAG_CREATE, ctypes.byref(op), ctypes.byref(orig))
    if res == TEEC_SUCCESS:
        print(f'  w77q-fs: {_green("wft.diag created")} (object left in flash)')
        return 0
    print(f'  w77q-fs: {_red("diag-create FAIL")}  (0x{res:08x})')
    return 1


def diag_delete(lib, sess):
    """Delete wft.diag from flash."""
    op   = TEEC_Operation()
    orig = ctypes.c_uint32(0)
    op.paramTypes = TEEC_PARAM_TYPES(TEEC_NONE, TEEC_NONE, TEEC_NONE, TEEC_NONE)
    res = lib.TEEC_InvokeCommand(
        ctypes.byref(sess), W77Q_FS_TC_DIAG_DELETE, ctypes.byref(op), ctypes.byref(orig))
    if res == TEEC_SUCCESS:
        print(f'  w77q-fs: {_green("wft.diag deleted")}')
        return 0
    print(f'  w77q-fs: {_red("diag-delete FAIL")}  (0x{res:08x})')
    return 1


def diag_setup(lib, sess, tc_idx: int):
    """Pre-create TC[tc_idx]'s initial objects and leave them in flash."""
    op   = TEEC_Operation()
    orig = ctypes.c_uint32(0)
    op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INPUT, TEEC_NONE, TEEC_NONE, TEEC_NONE)
    op.params[0].value.a = tc_idx
    res = lib.TEEC_InvokeCommand(
        ctypes.byref(sess), W77Q_FS_TC_DIAG_SETUP, ctypes.byref(op), ctypes.byref(orig))
    if res == TEEC_SUCCESS:
        print(f'  w77q-fs: {_green(f"diag-setup TC[{tc_idx}] done")} (objects left in flash)')
        return 0
    if res == TEEC_ERROR_NOT_SUPPORTED:
        print(f'  w77q-fs: diag-setup TC[{tc_idx}] not applicable (pre-create skipped)')
        return 2  # non-error "not applicable" — caller skips pre-create
    print(f'  w77q-fs: {_red(f"diag-setup TC[{tc_idx}] FAIL")}  (0x{res:08x})')
    return 1


def diag_teardown(lib, sess, tc_idx: int):
    """Best-effort delete any objects owned by TC[tc_idx]."""
    op   = TEEC_Operation()
    orig = ctypes.c_uint32(0)
    op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INPUT, TEEC_NONE, TEEC_NONE, TEEC_NONE)
    op.params[0].value.a = tc_idx
    res = lib.TEEC_InvokeCommand(
        ctypes.byref(sess), W77Q_FS_TC_DIAG_TEARDOWN, ctypes.byref(op), ctypes.byref(orig))
    if res == TEEC_SUCCESS:
        print(f'  w77q-fs: {_green(f"diag-teardown TC[{tc_idx}] done")}')
        return 0
    print(f'  w77q-fs: {_red(f"diag-teardown TC[{tc_idx}] FAIL")}  (0x{res:08x})')
    return 1


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main():
    lib = _load_libteec()
    _setup_libteec(lib)

    try:
        ctx, sess = open_session(lib)
    except RuntimeError as e:
        print(f'Error: {e}', file=sys.stderr)
        sys.exit(1)

    print(f'\n{_bold("w77q-fs storage backend test")}')
    print('─────────────────────────────')

    try:
        if len(sys.argv) < 2:
            fail = run_all(lib, sess)
        else:
            name = sys.argv[1]
            extra_cmds = ('diag-create', 'diag-delete', 'diag-setup', 'diag-teardown')
            if name not in TC_NAMES and name not in extra_cmds:
                print(
                    f"Unknown test '{name}'.\n"
                    f"Available: {' '.join(TC_NAMES)} "
                    f"diag-create diag-delete diag-setup <n> diag-teardown <n>",
                    file=sys.stderr)
                close_session(lib, ctx, sess)
                sys.exit(1)
            if name == 'diag-create':
                fail = diag_create(lib, sess)
            elif name == 'diag-delete':
                fail = diag_delete(lib, sess)
            elif name in ('diag-setup', 'diag-teardown'):
                if len(sys.argv) < 3:
                    print(f"Usage: {name} <tc_index 0-6>", file=sys.stderr)
                    close_session(lib, ctx, sess)
                    sys.exit(1)
                try:
                    tc_idx = int(sys.argv[2])
                except ValueError:
                    print(f"tc_index must be an integer 0-6", file=sys.stderr)
                    close_session(lib, ctx, sess)
                    sys.exit(1)
                if name == 'diag-setup':
                    fail = diag_setup(lib, sess, tc_idx)
                else:
                    fail = diag_teardown(lib, sess, tc_idx)
            else:
                fail = run_tc(lib, sess, TC_NAMES.index(name), name)
    finally:
        close_session(lib, ctx, sess)

    print('─────────────────────────────')
    if fail == 0:
        print(f'{_green("w77q-fs: all tests PASSED")}\n')
    elif fail == 2:
        pass  # "not applicable" from diag-setup — message already printed
    else:
        print(f'{_red(f"w77q-fs: {fail} test(s) FAILED")}\n')

    sys.exit(fail)


if __name__ == '__main__':
    main()
