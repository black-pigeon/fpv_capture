#!/usr/bin/env python3
"""Run the production spectrum read path against a fault-injecting DMA fake."""
from pathlib import Path
import os
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'firmware/board/e200/linux/fpv_capture/fpv_spectrum.c').read_text()
parts=[s[s.index('struct spectrum {'):s.index('static void spectrum_free')],
       s[s.index('static int spectrum_idle'):s.index('static int spectrum_open')],
       s[s.index('static ssize_t spectrum_read'):s.index('static const struct file_operations')]]
with tempfile.TemporaryDirectory() as tmp:
    p=Path(tmp);(p/'spectrum_functions.inc').write_text('\n'.join(parts))
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-I'+tmp,'-I'+str(root/'app/include'),str(root/'app/tests/test_spectrum_driver_host.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
