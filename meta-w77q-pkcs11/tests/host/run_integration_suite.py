#!/usr/bin/env python3
"""run_integration_suite.py — PKCS#11 on-board integration test suite runner.

Runs all FT tests against a physically connected Sparrow Hawk board over
serial, collects results, and writes a JUnit XML report for CI.

Usage:
  python3 scripts/run_integration_suite.py [options]

Options:
  --port PORT          Serial port  (default: /dev/ttyUSB0)
  --deploy             Deploy all target scripts before running
  --tests FT22,FT25    Comma-separated list of tests to run (default: all)
  --skip  FT17         Comma-separated list of tests to skip
  --no-rsa4096         Pass --no-rsa4096 to FT26 (skips ~5 min RSA-4096 keygen)
  --output FILE        JUnit XML output path (default: test-results.xml)
  --timeout-scale N    Multiply all per-test timeouts by N (default: 1.0)
  --lock FILE          Lock file path to prevent concurrent board access
                       (default: /tmp/sparrow-hawk-board.lock)
"""

import argparse
import fcntl
import os
import re
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from datetime import datetime, timezone

BASE  = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPTS = os.path.join(BASE, 'host')

# ── Test catalogue ─────────────────────────────────────────────────────────
# (id, script, timeout_s, description, extra_args, supports_deploy)
# supports_deploy=False: script has no --deploy flag (older scripts FT9-FT18)
SUITE = [
    ("FT9",  "ft9.py",   180, "PKCS#11 write → w77q-dump LUT delta",               [], False),
    ("FT10", "ft10.py",  180, "PKCS#11 write + read round-trip",                    [], False),
    ("FT11", "ft11.py",  180, "PKCS#11 + tee-storage cross-validation",             [], False),
    ("FT12", "ft12.py",  180, "PKCS#11 write + flash sector scan",                  [], False),
    ("FT13", "ft13.py",  240, "Persistence across tee-supplicant restart",          [], False),
    ("FT16", "ft16.py",  240, "PKCS#11 write → w77q-dump LUT delta (detailed)",     [], False),
    ("FT17", "ft17.py",  480, "25× tee-demo wear test",                             [], False),
    ("FT18", "ft18.py",  480, "pkcs11-demo RSA/EC/AES/RNG + flash delta",           [], False),
    ("FT19", "ft19.py",  360, "RSA TEE keygen + encrypt + TEE decrypt",             [], True),
    ("FT20", "ft20.py",  480, "Full flash compare around pkcs11-demo",              [], False),
    ("FT22", "ft22.py",  720, "PKCS#11 algorithm coverage (RSA/EC/AES/HMAC/RNG)",  [], True),
    ("FT23", "ft23.py",  300, "W77Q LUT integrity validation",                      [], True),
    ("FT24", "ft24.py",  360, "TEE isolation and data protection",                  [], True),
    ("FT25", "ft25.py",  600, "Hybrid AES+RSA encrypt/decrypt (1 KB–10 MB)",       [], True),
    ("FT26", "ft26.py",  900, "PKCS#11 full integration: search/attrs/mechanisms", [], True),
]

DIAGNOSTIC = {"FT14"}   # no PASS/FAIL, always marked as skipped in XML


def log(msg=""):
    print(msg, flush=True)


def parse_args():
    ap = argparse.ArgumentParser(
        description="PKCS#11 integration suite runner",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    ap.add_argument('--port',          default='/dev/ttyUSB0')
    ap.add_argument('--deploy',        action='store_true')
    ap.add_argument('--tests',         default='',
                    help='comma-separated FT IDs to run (default: all)')
    ap.add_argument('--skip',          default='',
                    help='comma-separated FT IDs to skip')
    ap.add_argument('--no-rsa4096',    action='store_true')
    ap.add_argument('--output',        default='test-results.xml')
    ap.add_argument('--timeout-scale', type=float, default=1.0)
    ap.add_argument('--lock',          default='/tmp/sparrow-hawk-board.lock')
    return ap.parse_args()


def acquire_lock(lock_path):
    """Exclusive flock on lock_path to prevent concurrent board access."""
    lf = open(lock_path, 'w')
    try:
        fcntl.flock(lf, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        log(f"[ERROR] Another process holds board lock {lock_path}")
        log("        Wait for it to finish or remove the lock file.")
        sys.exit(2)
    lf.write(f"pid={os.getpid()} started={datetime.now().isoformat()}\n")
    lf.flush()
    return lf


def run_test(tid, script, timeout_s, description, extra_args,
             port, deploy, scale, supports_deploy=True):
    """Run a single test script as a subprocess, return (status, elapsed, output).

    status: 'pass' | 'fail' | 'error' | 'skipped'
    """
    script_path = os.path.join(SCRIPTS, script)
    if not os.path.exists(script_path):
        return 'error', 0.0, f"script not found: {script_path}"

    cmd = [sys.executable, script_path, '--port', port]
    if deploy and supports_deploy:
        cmd.append('--deploy')
    cmd.extend(extra_args)

    effective_timeout = int(timeout_s * scale) + 30  # +30s subprocess overhead

    log(f"\n{'─'*68}")
    log(f"  {tid}: {description}")
    log(f"  cmd: {' '.join(cmd)}")
    log(f"  timeout: {effective_timeout}s")
    log(f"{'─'*68}")

    t0 = time.time()
    try:
        result = subprocess.run(
            cmd,
            timeout=effective_timeout,
            capture_output=False,   # let output stream live to terminal
            text=True,
        )
        elapsed = time.time() - t0
        status = 'pass' if result.returncode == 0 else 'fail'
        output = ''  # output already streamed live; nothing extra to capture
    except subprocess.TimeoutExpired as exc:
        elapsed = time.time() - t0
        log(f"\n[TIMEOUT] {tid} exceeded {effective_timeout}s")
        status = 'error'
        output = f"timeout after {effective_timeout}s"
    except Exception as exc:
        elapsed = time.time() - t0
        log(f"\n[ERROR] {tid}: {exc}")
        status = 'error'
        output = str(exc)

    return status, elapsed, output


def build_xml(results, suite_elapsed, output_path):
    """Write JUnit XML compatible with GitHub Actions test reporting."""
    total   = len(results)
    passed  = sum(1 for r in results if r['status'] == 'pass')
    failed  = sum(1 for r in results if r['status'] == 'fail')
    errors  = sum(1 for r in results if r['status'] == 'error')
    skipped = sum(1 for r in results if r['status'] == 'skipped')

    suite = ET.Element('testsuite',
        name="pkcs11-integration",
        tests=str(total),
        failures=str(failed),
        errors=str(errors),
        skipped=str(skipped),
        time=f"{suite_elapsed:.2f}",
        timestamp=datetime.now(timezone.utc).isoformat())

    for r in results:
        tc = ET.SubElement(suite, 'testcase',
            classname="pkcs11",
            name=r['tid'],
            time=f"{r['elapsed']:.2f}")

        if r['status'] == 'fail':
            fl = ET.SubElement(tc, 'failure',
                message=f"{r['tid']} failed",
                type="AssertionError")
            fl.text = r['output'] or f"{r['tid']} FAILED"
        elif r['status'] == 'error':
            er = ET.SubElement(tc, 'error',
                message=f"{r['tid']} error",
                type="RuntimeError")
            er.text = r['output'] or f"{r['tid']} ERROR"
        elif r['status'] == 'skipped':
            sk = ET.SubElement(tc, 'skipped', message=r['output'] or 'skipped')

        # Always attach description as system-out property
        so = ET.SubElement(tc, 'system-out')
        so.text = f"{r['description']}\nelapsed: {r['elapsed']:.1f}s\n{r['output'] or ''}"

    tree = ET.ElementTree(suite)
    ET.indent(tree, space='  ')
    with open(output_path, 'wb') as f:
        tree.write(f, xml_declaration=True, encoding='utf-8')

    return passed, failed, errors, skipped


def print_summary(results, suite_elapsed):
    W = 72
    log(f"\n{'═'*W}")
    log(f"  PKCS#11 Integration Suite — Results")
    log(f"{'═'*W}")
    log(f"  {'Test':<8} {'Status':<9} {'Time':>6}s  Description")
    log(f"  {'─'*62}")
    for r in results:
        icon = {'pass': '✓', 'fail': '✗', 'error': '!', 'skipped': '-'}[r['status']]
        status_str = r['status'].upper()
        log(f"  {r['tid']:<8} {icon} {status_str:<7} {r['elapsed']:>6.0f}s  {r['description']}")
    log(f"{'─'*W}")

    passed  = sum(1 for r in results if r['status'] == 'pass')
    failed  = sum(1 for r in results if r['status'] == 'fail')
    errors  = sum(1 for r in results if r['status'] == 'error')
    skipped = sum(1 for r in results if r['status'] == 'skipped')

    log(f"  Total: {len(results)}  "
        f"PASS: {passed}  FAIL: {failed}  ERROR: {errors}  SKIP: {skipped}")
    log(f"  Suite elapsed: {suite_elapsed:.0f}s  "
        f"({suite_elapsed/60:.1f} min)")
    log(f"{'═'*W}")


def main():
    args = parse_args()

    # Build run list
    run_set = {t.strip().upper() for t in args.tests.split(',') if t.strip()}
    skip_set = {t.strip().upper() for t in args.skip.split(',') if t.strip()}

    suite = [(tid, script, tmo, desc, extra, sdeploy)
             for tid, script, tmo, desc, extra, sdeploy in SUITE
             if (not run_set or tid in run_set) and tid not in skip_set]

    # Inject --no-rsa4096 into FT26 if requested
    if args.no_rsa4096:
        suite = [(tid, script, tmo, desc, (extra + ['--no-rsa4096'] if tid == 'FT26' else extra), sdeploy)
                 for tid, script, tmo, desc, extra, sdeploy in suite]

    log(f"\n{'═'*72}")
    log(f"  PKCS#11 Integration Suite")
    log(f"  port={args.port}  deploy={args.deploy}  "
        f"timeout_scale={args.timeout_scale}")
    log(f"  tests={[t[0] for t in suite]}")
    log(f"{'═'*72}\n")

    # Acquire board lock
    lock_fh = acquire_lock(args.lock)

    results = []
    suite_t0 = time.time()

    try:
        for tid, script, tmo, desc, extra, sdeploy in suite:
            if tid in DIAGNOSTIC:
                results.append(dict(
                    tid=tid, status='skipped', elapsed=0.0,
                    description=desc, output='diagnostic only — no PASS/FAIL'))
                continue

            status, elapsed, output = run_test(
                tid, script, tmo, desc, extra,
                args.port, args.deploy, args.timeout_scale,
                supports_deploy=sdeploy)

            results.append(dict(
                tid=tid, status=status, elapsed=elapsed,
                description=desc, output=output))

            # Inter-test pause — lets board serial buffer drain and tee-supplicant settle
            time.sleep(5)

    finally:
        suite_elapsed = time.time() - suite_t0

        # Always write XML even if we crash mid-suite
        passed, failed, errors, skipped = build_xml(
            results, suite_elapsed, args.output)
        log(f"\n[XML] written to {args.output}")

        print_summary(results, suite_elapsed)

        # Release board lock
        fcntl.flock(lock_fh, fcntl.LOCK_UN)
        lock_fh.close()

    rc = 0 if (failed == 0 and errors == 0) else 1
    sys.exit(rc)


if __name__ == '__main__':
    main()
