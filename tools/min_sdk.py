#!/usr/bin/env python3
"""Lower the SDK revision an app declares in PebbleProcessInfo.sdk_version.

Pebble firmware refuses to launch an app whose declared SDK revision is newer
than its own ("This app requires a newer version of the Pebble firmware." —
pebble/process_management/process_manager.c:process_manager_check_SDK_compatible).
The value is baked in from PROCESS_INFO_CURRENT_SDK_VERSION_MAJOR/MINOR in each
platform's pebble_process_info.h, which is #defined unconditionally, so the only
way to change it is to patch the SDK headers, build, and restore them.

API revisions worth knowing (see the changelog in pebble_process_info.h):
    0x20 (5.32)  SDK 3.0 baseline: layers, tick, battery, bluetooth, graphics
    0x4a (5.74)  health_service_* (HealthAPI, rev 77)   <-- this watchface
    0x50 (5.80)  4.0: UnobstructedArea, AppGlance, health expansion
    0x56 (5.86)  what the current SDK stamps by default
    0x6e (5.110) what the current SDK stamps for emery/flint/gabbro

Usage:
    python3 tools/min_sdk.py 0x4a            # patch SDK headers only
    python3 tools/min_sdk.py 0x4a --build    # patch, build, restore
    python3 tools/min_sdk.py --restore       # put the headers back
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys

SDK_ROOT = os.path.expanduser("~/.local/share/pebble-sdk/SDKs")
MAJOR_RE = re.compile(r"#define PROCESS_INFO_CURRENT_SDK_VERSION_MAJOR 0x([0-9a-fA-F]+)")
MINOR_RE = re.compile(r"#define PROCESS_INFO_CURRENT_SDK_VERSION_MINOR 0x([0-9a-fA-F]+)")


def active_sdk():
    """Directory of the SDK pebble-tool currently has active."""
    try:
        out = subprocess.check_output(["pebble", "sdk", "list"],
                                      stderr=subprocess.DEVNULL).decode()
        for line in out.splitlines():
            m = re.match(r"^\s*(\S+)\s*\(active\)", line)
            if m:
                return os.path.join(SDK_ROOT, m.group(1))
    except Exception:
        pass
    dirs = glob.glob(os.path.join(SDK_ROOT, "*"))
    if not dirs:
        sys.exit("no SDKs installed")
    return sorted(dirs)[-1]


def headers(sdk):
    return sorted(glob.glob(os.path.join(
        sdk, "sdk-core", "pebble", "*", "include", "pebble_process_info.h")))


def read_version(path):
    txt = open(path).read()
    return int(MAJOR_RE.search(txt).group(1), 16), int(MINOR_RE.search(txt).group(1), 16)


def patch(major, minor):
    sdk = active_sdk()
    touched = []
    for h in headers(sdk):
        shutil.copy2(h, h + ".orig")
        txt = open(h).read()
        txt = MAJOR_RE.sub("#define PROCESS_INFO_CURRENT_SDK_VERSION_MAJOR 0x%x" % major, txt)
        txt = MINOR_RE.sub("#define PROCESS_INFO_CURRENT_SDK_VERSION_MINOR 0x%x" % minor, txt)
        open(h, "w").write(txt)
        touched.append(h)
    print("patched %d headers in %s -> sdk_version %d.%d (0x%x.0x%x)"
          % (len(touched), os.path.basename(sdk), major, minor, major, minor))
    return touched


def restore():
    sdk = active_sdk()
    n = 0
    for orig in glob.glob(os.path.join(sdk, "sdk-core", "pebble", "*", "include",
                                       "pebble_process_info.h.orig")):
        shutil.move(orig, orig[:-5])
        n += 1
    print("restored %d headers" % n)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("revision", nargs="?", help="minor revision, e.g. 0x4a")
    ap.add_argument("--major", default="5")
    ap.add_argument("--build", action="store_true", help="run `pebble build` then restore")
    ap.add_argument("--restore", action="store_true")
    a = ap.parse_args()

    if a.restore:
        restore()
        return
    if not a.revision:
        sys.exit("give a revision (e.g. 0x4a) or --restore")

    patch(int(a.major, 0), int(a.revision, 0))
    if a.build:
        try:
            rc = subprocess.call(["pebble", "build"])
        finally:
            restore()
        sys.exit(rc)


if __name__ == "__main__":
    main()