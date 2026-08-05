#!/usr/bin/env python3
"""
Write a Sparrow Hawk SD card image to a removable block device.

Usage:
    sudo python3 write-sd.py /dev/sdX
    sudo python3 write-sd.py /dev/sdX --image /path/to/image.wic.gz

Requires root.  Tries bmaptool first (fast, sparse-aware), falls back to
gzip + dd if bmaptool is not installed.
"""

import argparse
import os
import shutil
import subprocess
import sys

DEPLOY_DIR = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "build", "build-sparrow-hawk", "tmp", "deploy", "images", "sparrow-hawk",
)
DEFAULT_IMAGE = "core-image-minimal-sparrow-hawk.rootfs.wic.gz"
DEFAULT_BMAP = "core-image-minimal-sparrow-hawk.rootfs.wic.bmap"


def fatal(msg: str) -> None:
    print(f"\033[91mERROR:\033[0m {msg}", file=sys.stderr)
    sys.exit(1)


def confirm(device: str, image: str) -> None:
    """Show device info and ask for confirmation."""
    print(f"\n  Image  : {image}")
    print(f"  Device : {device}")

    # Show device details via lsblk
    try:
        info = subprocess.check_output(
            ["lsblk", "-d", "-o", "NAME,SIZE,MODEL,TRAN", device],
            text=True, stderr=subprocess.DEVNULL,
        )
        for line in info.strip().splitlines():
            print(f"           {line}")
    except (subprocess.CalledProcessError, FileNotFoundError):
        pass

    print(f"\n\033[93mWARNING: ALL DATA on {device} will be destroyed.\033[0m")
    ans = input("Type 'yes' to continue: ")
    if ans.strip().lower() != "yes":
        print("Aborted.")
        sys.exit(0)


def unmount(device: str) -> None:
    """Unmount any mounted partitions on the device."""
    try:
        mounts = subprocess.check_output(
            ["findmnt", "-rno", "SOURCE,TARGET"], text=True,
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        return

    for line in mounts.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0].startswith(device):
            print(f"  Unmounting {parts[0]} from {parts[1]} ...")
            subprocess.run(["umount", parts[0]], check=False)

    # Detach partition mappings so the kernel releases the device
    subprocess.run(["partx", "-d", device], check=False,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def write_bmaptool(image: str, bmap: str, device: str) -> bool:
    """Write using bmaptool.  Returns True on success."""
    if not shutil.which("bmaptool"):
        return False

    cmd = ["bmaptool", "copy"]
    if os.path.isfile(bmap):
        cmd += ["--bmap", bmap]
    else:
        cmd.append("--nobmap")
    cmd += [image, device]

    print(f"\n  Running: {' '.join(cmd)}\n")
    ret = subprocess.run(cmd)
    return ret.returncode == 0


def write_dd(image: str, device: str) -> bool:
    """Fallback: decompress with gzip and write with dd."""
    cmd = f'gzip -dc "{image}" | dd of="{device}" bs=4M conv=fsync status=progress'
    print(f"\n  Running: {cmd}\n")
    ret = subprocess.run(cmd, shell=True)
    if ret.returncode != 0:
        return False
    subprocess.run(["sync"])
    return True


def main() -> None:
    parser = argparse.ArgumentParser(description="Write Sparrow Hawk image to SD card")
    parser.add_argument("device", help="Block device, e.g. /dev/sdb")
    parser.add_argument("--image", help="Path to .wic.gz image (auto-detected if omitted)")
    args = parser.parse_args()

    if os.geteuid() != 0:
        fatal("This script must be run as root (sudo).")

    device = args.device

    # Validate device
    if not os.path.exists(device):
        fatal(f"Device {device} does not exist.")
    if not os.path.basename(device).startswith("sd") and "mmcblk" not in device:
        fatal(f"{device} doesn't look like a removable device (expected /dev/sdX or /dev/mmcblkN).")

    # Resolve image path
    if args.image:
        image = os.path.abspath(args.image)
    else:
        image = os.path.join(os.path.abspath(DEPLOY_DIR), DEFAULT_IMAGE)

    if not os.path.isfile(image):
        fatal(f"Image not found: {image}\n"
              f"       Run ./build.sh first, or pass --image /path/to/image.wic.gz")

    bmap = image.replace(".wic.gz", ".wic.bmap")

    # Confirm with user
    confirm(device, image)

    # Unmount
    print("\n[1/3] Unmounting partitions ...")
    unmount(device)

    # Write
    print("[2/3] Writing image ...")
    if write_bmaptool(image, bmap, device):
        method = "bmaptool"
    elif write_dd(image, device):
        method = "dd"
    else:
        fatal("Flash failed.")

    # Eject
    print("[3/3] Ejecting ...")
    subprocess.run(["eject", device], check=False)

    print(f"\n\033[92mDone!\033[0m  Image written to {device} via {method}.")
    print("Remove the SD card and insert it into the Sparrow Hawk board.")


if __name__ == "__main__":
    main()
