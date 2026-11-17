"""Run the U-Boot shell command flow with hardware commands replaced by probes."""
import json
import os
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
mmc() { test "$FAIL_STEP" != mmc; }
fatload() {
    case "${@: -1}" in
        uEnv.txt) test "$FAIL_STEP" != uenv || return 1;;
        *.bit) test "$FAIL_STEP" != bitfile || return 1;;
    esac
    filesize=128
    return 0
}
fpga() { test "$FAIL_STEP" != fpga; }
bootz() { echo BOOT_LINUX; }
setenv() { printf -v "$1" '%s' "$2"; }
run() { eval "${!1}"; }
env() {
    if test "$1" = import; then
        test "$FAIL_STEP" != import || return 1
        while IFS='=' read -r key value; do
            export "$key=$value"
        done < "$UENV_FILE"
    elif test "$1" = exists; then
        test -n "${!2}"
    else
        return 1
    fi
}
eval "$BOOTCOMMAND"
'''


class BootFlowTest(unittest.TestCase):
    def test_linux_boot_requires_successful_fpga_load(self):
        config = ROOT / 'firmware/board/e200/u-boot/fragment.config'
        line = next(x for x in config.read_text().splitlines()
                    if x.startswith('CONFIG_BOOTCOMMAND='))
        command = json.loads(line.split('=', 1)[1])
        for step in ['mmc', 'uenv', 'import', 'bitfile', 'fpga', 'success']:
            with self.subTest(step=step):
                environment = dict(os.environ, FAIL_STEP=step, BOOTCOMMAND=command,
                                   UENV_FILE=str(ROOT / 'firmware/board/e200/boot/uEnv.txt'))
                result = subprocess.run(['bash', '-c', HARNESS], env=environment,
                                        capture_output=True, text=True)
                boots = result.stdout.splitlines().count('BOOT_LINUX')
                self.assertEqual(boots, 1 if step == 'success' else 0,
                                 result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
