#!/usr/bin/env python3
"""
run-target-tests.py — run the full on-target test suite via serial
===================================================================
Connects to the board over /dev/ttyUSB0, deploys any updated target
scripts if --deploy is given, then executes run-all-tests.sh and
streams all output live to the terminal.

Usage:
  python3 scripts/run-target-tests.py             # run only
  python3 scripts/run-target-tests.py --deploy    # deploy scripts then run
  python3 scripts/run-target-tests.py --test ft9  # run single test
"""

import serial, time, sys, os, argparse, subprocess

PORT  = '/dev/ttyUSB0'
BAUD  = 921600
BASE  = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

TARGET_SCRIPTS = [
    ('scripts/target/common.sh',        '/usr/bin/tee-tests-common.sh'),
    ('scripts/target/ft9-target.sh',    '/usr/bin/ft9-target.sh'),
    ('scripts/target/ft10-target.sh',   '/usr/bin/ft10-target.sh'),
    ('scripts/target/ft11-target.sh',   '/usr/bin/ft11-target.sh'),
    ('scripts/target/ft12-target.sh',   '/usr/bin/ft12-target.sh'),
    ('scripts/target/ft13-target.sh',   '/usr/bin/ft13-target.sh'),
    ('scripts/target/ft14-target.sh',   '/usr/bin/ft14-target.sh'),
    ('scripts/target/ft16-target.sh',   '/usr/bin/ft16-target.sh'),
    ('scripts/target/ft17-target.sh',   '/usr/bin/ft17-target.sh'),
    ('scripts/target/ft18-target.sh',   '/usr/bin/ft18-target.sh'),
    ('scripts/target/ft19-target.sh',   '/usr/bin/ft19-target.sh'),
    ('scripts/target/ft20-target.sh',   '/usr/bin/ft20-target.sh'),
    ('scripts/target/ft25-target.sh',   '/usr/bin/ft25-target.sh'),
    ('scripts/target/run-all-tests.sh', '/usr/bin/run-all-tests.sh'),
]


def kill_port_holders():
    r = subprocess.run(['lsof', PORT], capture_output=True, text=True)
    for line in r.stdout.splitlines()[1:]:
        parts = line.split()
        if len(parts) > 1 and parts[1].isdigit():
            subprocess.run(['kill', parts[1]])
    if r.stdout.strip():
        time.sleep(1)


def open_port():
    for attempt in range(1, 7):
        try:
            s = serial.Serial(PORT, BAUD, timeout=2.0, exclusive=False)
            print(f'[port] opened (attempt {attempt})')
            return s
        except serial.SerialException:
            time.sleep(2)
    sys.exit('[port] ERROR: cannot open serial port')


def deploy_script(s, src_path, dest_path):
    lines = open(src_path).readlines()
    s.write(f"cat > {dest_path} << 'ENDSCRIPT'\n".encode())
    time.sleep(0.4); s.read(65536)
    for line in lines:
        safe = line.rstrip('\n').encode('ascii', errors='replace').decode('ascii')
        s.write((safe + '\n').encode('ascii'))
        time.sleep(0.03)
    time.sleep(0.5)
    s.write(b'ENDSCRIPT\r'); time.sleep(1); s.read(65536)
    s.write(f'chmod +x {dest_path}\r'.encode()); time.sleep(0.4); s.read(65536)
    # verify line count
    s.reset_input_buffer()
    s.write(f'wc -l {dest_path}\r'.encode()); time.sleep(1)
    out = s.read(65536).decode(errors='replace').strip()
    lines_on_board = out.split('\n')[0].strip()
    print(f'  {dest_path}: {lines_on_board}')


def stream_command(s, cmd, timeout=600):
    """Send command and stream output line-by-line until shell prompt returns."""
    s.reset_input_buffer()
    s.write((cmd + '\r').encode())
    time.sleep(0.5)

    buf = b''
    t0 = time.time()
    last_activity = time.time()

    while True:
        try:
            chunk = s.read(4096)
        except Exception:
            time.sleep(0.1)
            continue
        if chunk:
            buf += chunk
            last_activity = time.time()
            # print complete lines as they arrive
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                text = line.decode(errors='replace').rstrip('\r')
                print(text)

        # detect shell prompt (command finished)
        decoded = buf.decode(errors='replace')
        if decoded.endswith('# ') or decoded.endswith('$ '):
            if buf:
                print(buf.decode(errors='replace').rstrip())
            break

        if time.time() - t0 > timeout:
            print('[timeout] command exceeded limit')
            break

        # idle watchdog — print partial buffer every 5s so user sees progress
        if not chunk and time.time() - last_activity > 5:
            if buf:
                partial = buf.decode(errors='replace')
                print(partial, end='', flush=True)
            last_activity = time.time()

    return buf.decode(errors='replace')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--deploy', action='store_true',
                        help='deploy all target scripts before running')
    parser.add_argument('--test', metavar='NAME', default=None,
                        help='run a single test script, e.g. ft9 or ft17')
    args = parser.parse_args()

    kill_port_holders()
    s = open_port()
    s.reset_input_buffer()
    s.write(b'\r'); time.sleep(1); s.read(65536)

    # ensure clean shell prompt
    s.write(b'stty echo\r'); time.sleep(0.5); s.read(65536)

    if args.deploy:
        print('\n[deploy] deploying target scripts...')
        for src, dst in TARGET_SCRIPTS:
            full_src = os.path.join(BASE, src)
            print(f'  -> {dst}')
            deploy_script(s, full_src, dst)
        print('[deploy] done\n')

    if args.test:
        script = f'/usr/bin/{args.test}-target.sh'
        print(f'\n[run] {script}\n{"="*50}')
        stream_command(s, f'{script} 2>&1', timeout=300)
    else:
        print('\n[run] run-all-tests.sh\n' + '='*50)
        stream_command(s, 'run-all-tests.sh 2>&1', timeout=600)

    s.write(b'stty echo\r')
    s.close()
    print('\n[done]')


if __name__ == '__main__':
    main()
