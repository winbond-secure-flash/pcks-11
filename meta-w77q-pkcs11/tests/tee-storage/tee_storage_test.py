#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""
tee_storage_test.py — Generic TEE secure storage test suite.

Uses ctypes + libteec.so to exercise every GP TEE storage command exposed
by tee_storage_test_ta (UUID c3d4e5f6-a7b8-90ab-cdef-012345678901).

Usage:
    python3 tee_storage_test.py [--auto] [--verbose] [<test_name> ...]

    --auto      run all tests and exit with 0/1
    --verbose   print TEEC param details
    <test_name> run only the named test(s) (partial match allowed)

Examples:
    python3 tee_storage_test.py --auto
    python3 tee_storage_test.py test_write_read test_delete
    python3 tee_storage_test.py large
"""

import ctypes
import os
import struct
import subprocess
import sys
import time

# ---------------------------------------------------------------------------
# GP TEE constants
# ---------------------------------------------------------------------------
TEEC_SUCCESS              = 0x00000000
# Note: TEE_ERROR_SHORT_BUFFER in the GP Internal Core API (TA side) is
# 0xFFFF0010, which differs from TEEC_ERROR_SHORT_BUFFER in the GP Client
# API (0xFFFF0005). TA return values use the internal API constants.
TEEC_ERROR_SHORT_BUFFER   = 0xFFFF0010
TEEC_ERROR_ITEM_NOT_FOUND = 0xFFFF0008
TEEC_ERROR_NOT_SUPPORTED  = 0xFFFF000A
TEEC_ERROR_BAD_PARAMETERS = 0xFFFF0006

TEEC_NONE         = 0
TEEC_VALUE_INPUT  = 1
TEEC_VALUE_OUTPUT = 2
TEEC_VALUE_INOUT  = 3
TEEC_MEMREF_INPUT  = 5
TEEC_MEMREF_OUTPUT = 6
TEEC_MEMREF_INOUT  = 7

def TEEC_PARAM_TYPES(t0, t1, t2, t3):
    return (t0 & 0xF) | ((t1 & 0xF) << 4) | \
           ((t2 & 0xF) << 8) | ((t3 & 0xF) << 12)

# ---------------------------------------------------------------------------
# TA commands (must match tee_storage_test_ta.h)
# ---------------------------------------------------------------------------
CMD_WRITE     = 0
CMD_READ      = 1
CMD_DELETE    = 2
CMD_EXISTS    = 3
CMD_LIST      = 4
CMD_GET_SIZE  = 5
CMD_CLEAR_ALL = 6

TA_UUID_STR = "c3d4e5f6-a7b8-90ab-cdef-012345678901"

# ---------------------------------------------------------------------------
# ctypes structures  (must match GP TEE Client API)
# ---------------------------------------------------------------------------
class TEEC_UUID(ctypes.Structure):
    _fields_ = [
        ("timeLow",    ctypes.c_uint32),
        ("timeMid",    ctypes.c_uint16),
        ("timeHiAndVersion", ctypes.c_uint16),
        ("clockSeqAndNode",  ctypes.c_uint8 * 8),
    ]

class TEEC_Context(ctypes.Structure):
    _fields_ = [("_opaque", ctypes.c_uint8 * 256)]

class TEEC_Session(ctypes.Structure):
    _fields_ = [("_opaque", ctypes.c_uint8 * 256)]

class _TmpRef(ctypes.Structure):
    _fields_ = [("buffer", ctypes.c_void_p),
                ("size",   ctypes.c_size_t)]

# GP TEE Client API: RegisteredMemoryReference is the largest union member (24 bytes on 64-bit)
class _RegRef(ctypes.Structure):
    _fields_ = [("parent", ctypes.c_void_p),
                ("size",   ctypes.c_size_t),
                ("offset", ctypes.c_size_t)]

class _Value(ctypes.Structure):
    _fields_ = [("a", ctypes.c_uint32),
                ("b", ctypes.c_uint32)]

class _ParamUnion(ctypes.Union):
    _fields_ = [("tmpref", _TmpRef),
                ("memref", _RegRef),   # RegisteredMemoryReference — sets union size to 24 bytes
                ("value",  _Value)]

class TEEC_Parameter(ctypes.Structure):
    _fields_ = [("u", _ParamUnion)]

class TEEC_Operation(ctypes.Structure):
    _fields_ = [
        ("started",    ctypes.c_uint32),
        ("paramTypes", ctypes.c_uint32),
        ("params",     TEEC_Parameter * 4),
        ("_imp",       ctypes.c_uint8 * 64),  # libteec implementation tail (semaphore etc.)
    ]

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
LIBTEEC_PATH = "/usr/lib/libteec.so.2"
VERBOSE = False

def _load_lib():
    lib = ctypes.CDLL(LIBTEEC_PATH)
    lib.TEEC_InitializeContext.argtypes = [ctypes.c_char_p, ctypes.POINTER(TEEC_Context)]
    lib.TEEC_InitializeContext.restype  = ctypes.c_uint32
    lib.TEEC_FinalizeContext.argtypes   = [ctypes.POINTER(TEEC_Context)]
    lib.TEEC_FinalizeContext.restype    = None
    lib.TEEC_OpenSession.argtypes = [
        ctypes.POINTER(TEEC_Context),
        ctypes.POINTER(TEEC_Session),
        ctypes.POINTER(TEEC_UUID),
        ctypes.c_uint32,                      # connectionMethod
        ctypes.c_void_p,                      # connectionData
        ctypes.POINTER(TEEC_Operation),
        ctypes.POINTER(ctypes.c_uint32),      # returnOrigin
    ]
    lib.TEEC_OpenSession.restype = ctypes.c_uint32
    lib.TEEC_CloseSession.argtypes = [ctypes.POINTER(TEEC_Session)]
    lib.TEEC_CloseSession.restype  = None
    lib.TEEC_InvokeCommand.argtypes = [
        ctypes.POINTER(TEEC_Session),
        ctypes.c_uint32,                      # commandID
        ctypes.POINTER(TEEC_Operation),
        ctypes.POINTER(ctypes.c_uint32),      # returnOrigin
    ]
    lib.TEEC_InvokeCommand.restype = ctypes.c_uint32
    return lib

def _parse_uuid(s):
    parts = s.split("-")
    time_low     = int(parts[0], 16)
    time_mid     = int(parts[1], 16)
    time_hi      = int(parts[2], 16)
    clock_bytes  = bytes.fromhex(parts[3] + parts[4])
    uuid = TEEC_UUID()
    uuid.timeLow = time_low
    uuid.timeMid = time_mid
    uuid.timeHiAndVersion = time_hi
    for i, b in enumerate(clock_bytes):
        uuid.clockSeqAndNode[i] = b
    return uuid

def _open(lib):
    ctx  = TEEC_Context()
    sess = TEEC_Session()
    rc = lib.TEEC_InitializeContext(None, ctypes.byref(ctx))
    if rc != TEEC_SUCCESS:
        raise RuntimeError(f"TEEC_InitializeContext failed: 0x{rc:08x}")
    uuid     = _parse_uuid(TA_UUID_STR)
    err_orig = ctypes.c_uint32(0)
    rc = lib.TEEC_OpenSession(ctypes.byref(ctx), ctypes.byref(sess),
                              ctypes.byref(uuid),
                              ctypes.c_uint32(0), None, None,
                              ctypes.byref(err_orig))
    if rc != TEEC_SUCCESS:
        lib.TEEC_FinalizeContext(ctypes.byref(ctx))
        raise RuntimeError(f"TEEC_OpenSession failed: 0x{rc:08x} (orig=0x{err_orig.value:08x})")
    return ctx, sess

def _close(lib, ctx, sess):
    lib.TEEC_CloseSession(ctypes.byref(sess))
    lib.TEEC_FinalizeContext(ctypes.byref(ctx))

def _invoke(lib, sess, cmd, op):
    err_orig = ctypes.c_uint32(0)
    rc = lib.TEEC_InvokeCommand(ctypes.byref(sess), ctypes.c_uint32(cmd),
                                ctypes.byref(op), ctypes.byref(err_orig))
    if VERBOSE:
        print(f"    cmd={cmd} rc=0x{rc:08x} orig=0x{err_orig.value:08x}")
    return rc

def _memref_in(op, idx, data: bytes):
    """Set op.params[idx] as a MEMREF_INPUT for the given bytes."""
    buf = ctypes.create_string_buffer(data) if data else ctypes.create_string_buffer(1)
    op.params[idx].u.tmpref.buffer = ctypes.cast(buf, ctypes.c_void_p)
    op.params[idx].u.tmpref.size   = len(data)
    return buf   # caller must keep alive

def _memref_out(op, idx, size: int):
    """Set op.params[idx] as a MEMREF_OUTPUT with a fresh buffer of `size` bytes."""
    buf = ctypes.create_string_buffer(size)
    op.params[idx].u.tmpref.buffer = ctypes.cast(buf, ctypes.c_void_p)
    op.params[idx].u.tmpref.size   = size
    return buf

# ---------------------------------------------------------------------------
# TEE storage operations
# ---------------------------------------------------------------------------

def tst_write(lib, sess, key: bytes, value: bytes) -> int:
    op = TEEC_Operation()
    op.paramTypes = TEEC_PARAM_TYPES(
        TEEC_NONE, TEEC_MEMREF_INPUT, TEEC_MEMREF_INPUT, TEEC_NONE)
    k = _memref_in(op, 1, key)
    v = _memref_in(op, 2, value) if value else ctypes.create_string_buffer(1)
    if not value:
        op.params[2].u.tmpref.buffer = ctypes.cast(v, ctypes.c_void_p)
        op.params[2].u.tmpref.size   = 0
    return _invoke(lib, sess, CMD_WRITE, op)

def tst_read(lib, sess, key: bytes, max_size: int = 65536):
    """Returns (rc, data_bytes).  On SHORT_BUFFER data_bytes is None."""
    op = TEEC_Operation()
    op.paramTypes = TEEC_PARAM_TYPES(
        TEEC_VALUE_OUTPUT, TEEC_MEMREF_INPUT, TEEC_MEMREF_OUTPUT, TEEC_NONE)
    k    = _memref_in(op, 1, key)
    outb = _memref_out(op, 2, max_size)
    rc   = _invoke(lib, sess, CMD_READ, op)
    actual = op.params[0].u.value.a
    if rc == TEEC_SUCCESS:
        return rc, bytes(outb.raw[:actual])
    if rc == TEEC_ERROR_SHORT_BUFFER:
        return rc, None
    return rc, None

def tst_delete(lib, sess, key: bytes) -> int:
    op = TEEC_Operation()
    op.paramTypes = TEEC_PARAM_TYPES(
        TEEC_NONE, TEEC_MEMREF_INPUT, TEEC_NONE, TEEC_NONE)
    k = _memref_in(op, 1, key)
    return _invoke(lib, sess, CMD_DELETE, op)

def tst_exists(lib, sess, key: bytes):
    """Returns (rc, exists_bool, data_size)."""
    op = TEEC_Operation()
    op.paramTypes = TEEC_PARAM_TYPES(
        TEEC_VALUE_OUTPUT, TEEC_MEMREF_INPUT, TEEC_NONE, TEEC_NONE)
    k  = _memref_in(op, 1, key)
    rc = _invoke(lib, sess, CMD_EXISTS, op)
    exists = bool(op.params[0].u.value.a)
    sz     = op.params[0].u.value.b
    return rc, exists, sz

def tst_list(lib, sess, buf_size: int = 8192):
    """Returns (rc, count, [id_bytes, ...])."""
    op = TEEC_Operation()
    op.paramTypes = TEEC_PARAM_TYPES(
        TEEC_VALUE_OUTPUT, TEEC_MEMREF_OUTPUT, TEEC_NONE, TEEC_NONE)
    outb = _memref_out(op, 1, buf_size)
    rc   = _invoke(lib, sess, CMD_LIST, op)
    cnt  = op.params[0].u.value.a
    ids  = []
    if rc == TEEC_SUCCESS and cnt > 0:
        raw = bytes(outb.raw)
        for part in raw.split(b"\x00"):
            if part:
                ids.append(part)
    return rc, cnt, ids

def tst_get_size(lib, sess, key: bytes):
    """Returns (rc, size)."""
    op = TEEC_Operation()
    op.paramTypes = TEEC_PARAM_TYPES(
        TEEC_VALUE_OUTPUT, TEEC_MEMREF_INPUT, TEEC_NONE, TEEC_NONE)
    k  = _memref_in(op, 1, key)
    rc = _invoke(lib, sess, CMD_GET_SIZE, op)
    sz = op.params[0].u.value.a
    return rc, sz

def tst_clear_all(lib, sess):
    """Returns (rc, count_deleted)."""
    op = TEEC_Operation()
    op.paramTypes = TEEC_PARAM_TYPES(
        TEEC_VALUE_OUTPUT, TEEC_NONE, TEEC_NONE, TEEC_NONE)
    rc  = _invoke(lib, sess, CMD_CLEAR_ALL, op)
    cnt = op.params[0].u.value.a
    return rc, cnt

# ---------------------------------------------------------------------------
# Test framework
# ---------------------------------------------------------------------------
_all_tests    = []
_pass_count   = 0
_fail_count   = 0
_current_lib  = None
_current_sess = None
_current_ctx  = None

def _test(fn):
    _all_tests.append(fn)
    return fn

def _ok(msg):
    global _pass_count
    _pass_count += 1
    print(f"  \033[32m✔\033[0m  {msg}")

def _fail(msg):
    global _fail_count
    _fail_count += 1
    print(f"  \033[31m✘\033[0m  {msg}")

def _assert(cond, msg):
    if cond:
        _ok(msg)
    else:
        _fail(msg)

def _assert_eq(got, want, label):
    if got == want:
        _ok(f"{label}: {got!r}")
    else:
        _fail(f"{label}: got {got!r}, want {want!r}")

def _assert_rc(rc, want, label):
    if rc == want:
        _ok(f"{label}: rc=0x{rc:08x}")
    else:
        _fail(f"{label}: rc=0x{rc:08x}, want=0x{want:08x}")

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

@_test
def test_write_read():
    """Write a value, read it back."""
    lib, sess = _current_lib, _current_sess
    key   = b"test.wr.key"
    value = b"Hello, TEE storage!"
    rc = tst_write(lib, sess, key, value)
    _assert_rc(rc, TEEC_SUCCESS, "write")
    rc, data = tst_read(lib, sess, key)
    _assert_rc(rc, TEEC_SUCCESS, "read-rc")
    _assert_eq(data, value, "read-data")
    tst_delete(lib, sess, key)

@_test
def test_overwrite():
    """Overwrite an existing key with a different value."""
    lib, sess = _current_lib, _current_sess
    key = b"test.overwrite"
    tst_write(lib, sess, key, b"original")
    rc = tst_write(lib, sess, key, b"replaced!")
    _assert_rc(rc, TEEC_SUCCESS, "overwrite-rc")
    rc, data = tst_read(lib, sess, key)
    _assert_eq(data, b"replaced!", "overwrite-data")
    tst_delete(lib, sess, key)

@_test
def test_empty_value():
    """Write and read back a zero-length value."""
    lib, sess = _current_lib, _current_sess
    key = b"test.empty"
    rc = tst_write(lib, sess, key, b"")
    _assert_rc(rc, TEEC_SUCCESS, "write-empty")
    rc, data = tst_read(lib, sess, key, max_size=64)
    _assert_rc(rc, TEEC_SUCCESS, "read-empty-rc")
    _assert_eq(data, b"", "read-empty-data")
    tst_delete(lib, sess, key)

@_test
def test_delete():
    """Delete an object and confirm it is gone."""
    lib, sess = _current_lib, _current_sess
    key = b"test.delete"
    tst_write(lib, sess, key, b"delete me")
    rc = tst_delete(lib, sess, key)
    _assert_rc(rc, TEEC_SUCCESS, "delete-rc")
    rc, data = tst_read(lib, sess, key)
    _assert_rc(rc, TEEC_ERROR_ITEM_NOT_FOUND, "post-delete-read")

@_test
def test_delete_missing():
    """Deleting a non-existent key returns ITEM_NOT_FOUND."""
    lib, sess = _current_lib, _current_sess
    key = b"test.no.such.key.xyz"
    rc = tst_delete(lib, sess, key)
    _assert_rc(rc, TEEC_ERROR_ITEM_NOT_FOUND, "delete-missing")

@_test
def test_exists():
    """EXISTS returns 1 for present and 0 for absent."""
    lib, sess = _current_lib, _current_sess
    key = b"test.exists"
    tst_write(lib, sess, key, b"i am here")
    rc, ex, sz = tst_exists(lib, sess, key)
    _assert_rc(rc, TEEC_SUCCESS, "exists-rc")
    _assert(ex, "exists=True")
    _assert_eq(sz, len(b"i am here"), "exists-size")
    tst_delete(lib, sess, key)
    rc, ex, sz = tst_exists(lib, sess, key)
    _assert_rc(rc, TEEC_SUCCESS, "not-exists-rc")
    _assert(not ex, "exists=False after delete")

@_test
def test_get_size():
    """GET_SIZE returns the correct data size."""
    lib, sess = _current_lib, _current_sess
    key   = b"test.getsize"
    value = b"0123456789"
    tst_write(lib, sess, key, value)
    rc, sz = tst_get_size(lib, sess, key)
    _assert_rc(rc, TEEC_SUCCESS, "getsize-rc")
    _assert_eq(sz, len(value), "getsize-value")
    tst_delete(lib, sess, key)

@_test
def test_short_buffer():
    """READ with undersized buffer returns SHORT_BUFFER and correct size."""
    lib, sess = _current_lib, _current_sess
    key   = b"test.shortbuf"
    value = b"0123456789ABCDEF"   # 16 bytes
    tst_write(lib, sess, key, value)
    rc, data = tst_read(lib, sess, key, max_size=4)
    _assert_rc(rc, TEEC_ERROR_SHORT_BUFFER, "short-buffer-rc")
    _assert(data is None, "data=None on SHORT_BUFFER")
    # now read with correct size
    rc, data = tst_read(lib, sess, key, max_size=len(value))
    _assert_rc(rc, TEEC_SUCCESS, "full-read-after-short")
    _assert_eq(data, value, "full-read-data")
    tst_delete(lib, sess, key)

@_test
def test_large_value():
    """Write and read back an 8 KB repeating pattern."""
    lib, sess = _current_lib, _current_sess
    key      = b"test.large"
    pattern  = bytes(range(256)) * 32   # 8192 bytes
    rc = tst_write(lib, sess, key, pattern)
    _assert_rc(rc, TEEC_SUCCESS, "large-write")
    rc, data = tst_read(lib, sess, key, max_size=len(pattern) + 16)
    _assert_rc(rc, TEEC_SUCCESS, "large-read-rc")
    _assert_eq(len(data) if data else -1, len(pattern), "large-read-len")
    _assert(data == pattern, "large-read-content")
    tst_delete(lib, sess, key)

@_test
def test_binary_data():
    """Value containing all 256 byte values round-trips correctly."""
    lib, sess = _current_lib, _current_sess
    key   = b"test.binary"
    value = bytes(range(256))
    tst_write(lib, sess, key, value)
    rc, data = tst_read(lib, sess, key, max_size=512)
    _assert_rc(rc, TEEC_SUCCESS, "binary-rc")
    _assert_eq(data, value, "binary-data")
    tst_delete(lib, sess, key)

@_test
def test_multiple_keys():
    """Write 10 distinct keys, verify each reads back correctly."""
    lib, sess = _current_lib, _current_sess
    keys = [f"multi.key.{i:02d}".encode() for i in range(10)]
    vals = [f"value-for-key-{i:02d}-{'x'*20}".encode() for i in range(10)]
    for k, v in zip(keys, vals):
        tst_write(lib, sess, k, v)
    all_ok = True
    for k, v in zip(keys, vals):
        rc, data = tst_read(lib, sess, k)
        if rc != TEEC_SUCCESS or data != v:
            all_ok = False
    _assert(all_ok, "all 10 keys read back correctly")
    for k in keys:
        tst_delete(lib, sess, k)

@_test
def test_list():
    """LIST returns the correct count and IDs after writing known keys."""
    lib, sess = _current_lib, _current_sess
    tst_clear_all(lib, sess)
    keys = [b"list.alpha", b"list.beta", b"list.gamma"]
    for k in keys:
        tst_write(lib, sess, k, b"data")
    rc, cnt, ids = tst_list(lib, sess)
    _assert_rc(rc, TEEC_SUCCESS, "list-rc")
    _assert_eq(cnt, len(keys), "list-count")
    for k in keys:
        _assert(k in ids, f"list contains {k!r}")
    tst_clear_all(lib, sess)

@_test
def test_list_empty():
    """LIST on empty storage returns count=0."""
    lib, sess = _current_lib, _current_sess
    tst_clear_all(lib, sess)
    rc, cnt, ids = tst_list(lib, sess)
    _assert_rc(rc, TEEC_SUCCESS, "list-empty-rc")
    _assert_eq(cnt, 0, "list-empty-count")

@_test
def test_clear_all():
    """CLEAR_ALL deletes all objects and LIST confirms empty storage."""
    lib, sess = _current_lib, _current_sess
    for i in range(5):
        tst_write(lib, sess, f"clear.{i}".encode(), b"v")
    rc, cnt = tst_clear_all(lib, sess)
    _assert_rc(rc, TEEC_SUCCESS, "clear-all-rc")
    _assert(cnt >= 5, f"clear-all-count >= 5 (got {cnt})")
    rc, cnt2, _ = tst_list(lib, sess)
    _assert_eq(cnt2, 0, "post-clear list=0")

@_test
def test_persistence():
    """Object written in one session is readable in a new session."""
    lib = _current_lib
    key   = b"test.persist"
    value = b"persists across sessions"

    ctx1, sess1 = _open(lib)
    tst_write(lib, sess1, key, value)
    _close(lib, ctx1, sess1)

    ctx2, sess2 = _open(lib)
    rc, data = tst_read(lib, sess2, key)
    _assert_rc(rc, TEEC_SUCCESS, "persist-read-rc")
    _assert_eq(data, value, "persist-read-data")
    tst_delete(lib, sess2, key)
    _close(lib, ctx2, sess2)

@_test
def test_unicode_key():
    """Key with non-ASCII bytes round-trips correctly."""
    lib, sess = _current_lib, _current_sess
    key   = "tée.ñoño.🔑".encode("utf-8")
    value = b"unicode key works"
    rc = tst_write(lib, sess, key, value)
    _assert_rc(rc, TEEC_SUCCESS, "unicode-key-write")
    rc, data = tst_read(lib, sess, key)
    _assert_rc(rc, TEEC_SUCCESS, "unicode-key-read-rc")
    _assert_eq(data, value, "unicode-key-data")
    tst_delete(lib, sess, key)

# ---------------------------------------------------------------------------
# Pre-flight checks
# ---------------------------------------------------------------------------

def _check_prerequisites():
    issues = []
    if not os.path.exists(LIBTEEC_PATH):
        issues.append(f"libteec.so not found at {LIBTEEC_PATH}")
    ta_path = f"/lib/optee_armtz/c3d4e5f6-a7b8-90ab-cdef-012345678901.ta"
    if not os.path.exists(ta_path):
        issues.append(f"TA not found at {ta_path}")
    try:
        out = subprocess.run(
            "pgrep -x tee-supplicant || pgrep -x optee_supplicant || pgrep -f tee-supplicant",
            shell=True, capture_output=True)
        if out.returncode != 0:
            issues.append("tee-supplicant is not running")
    except Exception:
        pass
    return issues

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def _run_tests(selected):
    global _current_lib, _current_ctx, _current_sess
    global _pass_count, _fail_count

    lib = _load_lib()
    ctx, sess = _open(lib)
    _current_lib  = lib
    _current_ctx  = ctx
    _current_sess = sess

    # clean slate
    tst_clear_all(lib, sess)

    tests = [t for t in _all_tests
             if not selected or any(s in t.__name__ for s in selected)]

    for t in tests:
        doc  = (t.__doc__ or t.__name__).strip().split("\n")[0]
        name = t.__name__
        print(f"\n[{name}]  {doc}")
        try:
            t()
        except Exception as e:
            _fail_count += 1
            print(f"  \033[31m✘\033[0m  EXCEPTION: {e}")

    # clean up after ourselves
    tst_clear_all(lib, sess)
    _close(lib, ctx, sess)

def main():
    global VERBOSE
    args = sys.argv[1:]

    auto    = "--auto"    in args; args = [a for a in args if a != "--auto"]
    VERBOSE = "--verbose" in args; args = [a for a in args if a != "--verbose"]

    if not auto and not args:
        print(__doc__)
        print("Available tests:")
        for t in _all_tests:
            doc = (t.__doc__ or "").strip().split("\n")[0]
            print(f"  {t.__name__:<30} {doc}")
        sys.exit(0)

    issues = _check_prerequisites()
    if issues:
        print("Pre-flight check FAILED:")
        for i in issues:
            print(f"  • {i}")
        sys.exit(2)

    _run_tests(args)

    total = _pass_count + _fail_count
    colour = "\033[32m" if _fail_count == 0 else "\033[31m"
    reset  = "\033[0m"
    print(f"\n{colour}{'='*50}{reset}")
    print(f"{colour}Results: {_pass_count}/{total} passed"
          f"{' — ALL PASS' if _fail_count == 0 else f', {_fail_count} FAILED'}{reset}")
    print(f"{colour}{'='*50}{reset}")
    sys.exit(0 if _fail_count == 0 else 1)

if __name__ == "__main__":
    main()
