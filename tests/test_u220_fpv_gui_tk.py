"""Run under an explicit test X display: FPV_GUI_TEST_DISPLAY=:N."""
import os
from pathlib import Path
import sys
import time
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools/u220_fpv_tx'
sys.path.insert(0, str(TOOLS))


@unittest.skipUnless(os.environ.get('FPV_GUI_TEST_DISPLAY'), 'needs a dedicated test X display')
class TkSmokeTest(unittest.TestCase):
    def setUp(self):
        import tkinter as tk
        from gui import TransmitterGUI
        self.window = tk.Tk(screenName=os.environ['FPV_GUI_TEST_DISPLAY'])
        self.app = TransmitterGUI(self.window)
        self.window.update()

    def tearDown(self):
        if self.app.runner.busy:
            self.app.runner.stop()
            self.app.runner.thread.join(timeout=10)
        try:
            self.window.destroy()
        except Exception:
            pass

    def spin_until(self, predicate, timeout=20):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.window.update()
            if predicate(): return
            time.sleep(.02)
        self.fail('GUI operation timed out: ' + self.app.status.get())

    def test_idle_and_both_standards_offline(self):
        self.assertFalse(self.app.runner.busy)
        self.assertEqual(self.app.attenuation.get(), '89.75')
        self.app.device_args.set('type=device_that_does_not_exist')
        for standard in ('PAL', 'NTSC'):
            self.app.standard.set(standard)
            self.app._preview()
            self.app.start(False)
            self.assertEqual(str(self.app.start_button['state']), 'disabled')
            self.spin_until(lambda: self.app.status.get() == 'Offline check passed')
            self.assertIsNotNone(self.app.preview_image)
            self.assertIn(standard, self.app.card_detail.get())
        log = self.app.log.get('1.0', 'end')
        self.assertIn('mode=offline', log)
        self.assertIn('attenuation_db=89.75', log)
        self.assertNotIn('--transmit', log)
        self.assertEqual(str(self.app.start_button['state']), 'normal')
        self.app.standard.set('PAL')
        self.app.device_args.set('type=b200,serial=U220200')
        self.app._preview()
        self.window.update()
        try:
            from PIL import ImageGrab
            image = ImageGrab.grab(xdisplay=os.environ['FPV_GUI_TEST_DISPLAY'])
            image.crop((self.window.winfo_rootx(), self.window.winfo_rooty(),
                self.window.winfo_rootx() + self.window.winfo_width(),
                self.window.winfo_rooty() + self.window.winfo_height())).save(
                    TOOLS.parents[1] / 'build/u220-fpv-source/gui-preview.png')
        except ImportError:
            pass

    def test_live_edit_apply_and_confirmed_settings(self):
        from gui_support import Settings
        self.app.running_mode = 'transmit'
        self.app.runner.active_settings = Settings.parse('5800', 'PAL', '89.75', '30', '0', 'type=b200,serial=U220200')
        script = """import json, sys
state = dict(id=0, status='ready', standard='pal', frequency_hz=5800000000, attenuation_db=89.75, dwell_s=30)
print('CONTROL ' + json.dumps(state), flush=True)
for line in sys.stdin:
    request = json.loads(line)
    request['status'] = 'applied'
    request['frequency_hz'] = request['frequencies_hz'][0]
    print('CONTROL ' + json.dumps(request), flush=True)
"""
        self.app.runner.start_commands([('Live dummy', [sys.executable, '-u', '-c', script])])
        self.app._busy(True)
        self.spin_until(lambda: str(self.app.apply_button['state']) == 'normal')
        self.assertTrue(all(str(control['state']) != 'disabled' for control in self.app.controls[:4]))
        self.assertTrue(all(str(control['state']) == 'disabled' for control in self.app.controls[4:]))
        child = self.app.runner.process
        old_detail = self.app.detail.get()
        self.app.frequency.set('5860')
        self.app.standard.set('NTSC')
        self.app.duration.set('100')
        self.assertEqual(self.app.detail.get(), old_detail)  # edits are not reported as applied
        self.app.apply()
        self.assertEqual(str(self.app.apply_button['state']), 'disabled')
        self.spin_until(lambda: self.app.status.get() == 'Transmitting — changes applied')
        self.assertIs(self.app.runner.process, child)
        self.assertIn('NTSC · 5860 MHz', self.app.detail.get())
        self.app.frequency.set('nan')
        self.app.apply()
        self.assertEqual(self.app.status.get(), 'Changes not applied — check settings')
        self.assertIn('NTSC · 5860 MHz', self.app.detail.get())
        self.app.stop()
        self.spin_until(lambda: self.app.status.get() == 'Stopped')
        self.assertEqual(str(self.app.apply_button['state']), 'disabled')

    def test_invalid_parameters_do_not_start_job(self):
        self.app.frequency.set('nan')
        self.app.start(True)
        self.assertFalse(self.app.runner.busy)
        self.assertEqual(self.app.status.get(), 'Check settings')

    def test_window_close_stops_owned_worker(self):
        self.app.runner.start_commands([('Dummy worker', [sys.executable, '-u', '-c',
            "import time; print('dummy-ready', flush=True); time.sleep(30)"])])
        self.spin_until(lambda: 'dummy-ready' in self.app.log.get('1.0', 'end'))
        child = self.app.runner.process
        self.app.close()
        self.app.runner.thread.join(timeout=10)
        self.assertIsNotNone(child.poll())
        self.assertTrue(self.app.closing)


if __name__ == '__main__':
    unittest.main()
