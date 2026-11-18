#!/usr/bin/env python3
"""Compile and exercise the real driver's control/read functions with fake I/O.

Run from any directory: python3 app/tests/test_capture_driver.py
This supplements the ARM object build; it does not model CDC or DMA hardware.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "firmware/board/e200/linux/fpv_capture/fpv_capture.c"
text = SOURCE.read_text()
parts = [
    text[text.index("#define REG_ID"):text.index("static void fpv_free")],
    text[text.index("static int fpv_wait_idle"):text.index("static int fpv_open")],
    text[text.index("static long fpv_ioctl"):text.index("static const struct file_operations")],
]
with tempfile.TemporaryDirectory(prefix="fpv-driver-test-") as temporary:
    work = Path(temporary)
    (work / "driver_functions.inc").write_text("\n".join(parts))
    subprocess.run([
        os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-parameter", "-I" + str(work), "-I" + str(ROOT / "app/include"),
        str(ROOT / "app/tests/test_capture_driver_host.c"), "-o", str(work / "test"),
    ], check=True)
    subprocess.run([str(work / "test")], check=True)
