# Sparrow Hawk Tests

All test material for the Renesas R-Car V4H Sparrow Hawk board lives here.

## Directory Layout

```
tests/
├── host/               Python functional tests — run on the host PC
├── target/             Shell test scripts — run on the board over SSH/serial
├── pkcs11/             PKCS#11 and TEE secure-storage scripts (deployed to rootfs)
├── w77q-fs/            W77Q FS test TA + Linux host app (C source, built by Yocto)
└── tee-demo/           TEE demo TA + Linux host app (C source, built by Yocto)
```

## host/ — Host-Side Functional Tests

Python scripts driven from the host PC over serial + SSH.  
Run the full suite:

```bash
python3 tests/host/run_integration_suite.py
# or individually:
python3 tests/host/ft13.py    # W77Q FS LUT rebuild after restart
python3 tests/host/ft22.py    # PKCS#11 token init + key generation
```

Each `ftNN.py` corresponds to a functional test case; the matching
`tests/target/ftNN-target.sh` is the on-board counterpart it invokes.

Power-loss test (requires manual intervention):

```bash
python3 tests/host/run-ft21-power-loss.py
```

## target/ — On-Board Test Scripts

Shell scripts executed directly on the board.  Run all:

```bash
# On the board:
/usr/bin/run-all-tests.sh
```

Individual scripts: `ftNN-target.sh`.  Common helpers shared across all tests:
`common.sh`.

## pkcs11/ — PKCS#11 and TEE Storage Scripts

These are installed to `/usr/bin/` on the target rootfs by the
`pkcs11-tests` Yocto recipe.

| Script | Purpose |
|--------|---------|
| `check-pkcs11.sh` | Full PKCS#11 smoke test (init token, keygen, sign, verify, …) |
| `check-tee-storage.sh` | TEE secure-storage read/write/delete smoke test |
| `pkcs-test-menu.sh` | Interactive menu to run individual PKCS#11 checks |
| `pkcs11-rsa-sign-recover.sh` | RSA sign + recover round-trip test |

Run on the board:

```bash
# Initialize token and run all PKCS#11 checks:
check-pkcs11.sh

# Interactive menu:
pkcs-test-menu.sh
```

> **Note:** Keys must be created with `--usage-sign` for `C_Sign` to work.  
> `check-pkcs11.sh` already does this correctly.

## w77q-fs/ — W77Q FS Test TA + Host App

Source for the W77Q filesystem test suite.  Built by:
`recipes-utils/w77q-fs-test/w77q-fs-test_1.0.bb`

```
w77q-fs/
├── ta/      Trusted Application (OP-TEE secure world)
└── host/    Linux host app (calls the TA via libteec)
```

Run on the board:

```bash
w77q-fs-test          # run all 7 test cases
w77q-fs-test basic    # run TC_BASIC only
```

Exit code = number of failed test cases (0 = all passed).

## tee-demo/ — TEE Demo TA + Host App

Demonstration of password-protected W77Q QLIB secure storage.  
Built by: `recipes-dev/tee-demo/tee-demo_1.0.bb`

```
tee-demo/
├── ta/      Trusted Application
└── host/    Linux host app
```

Run on the board:

```bash
tee-demo
```
