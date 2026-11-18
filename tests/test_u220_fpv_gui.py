"""Host GUI model and subprocess lifecycle checks; no SDR is opened."""
from pathlib import Path
import queue
import sys
import tempfile
import time
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools/u220_fpv_tx'
sys.path.insert(0, str(TOOLS))
from gui_support import Settings, JobRunner


class SettingsTest(unittest.TestCase):
    def test_units_attenuation_and_transmit_mode(self):
        settings = Settings.parse('5740, 5800.125', 'NTSC', '89.75', '15', '1', 'type=b200,serial=U220200')
        self.assertEqual(settings.frequencies_hz, (5740000000, 5800125000))
        self.assertEqual(settings.standard, 'ntsc')
        args = settings.command(Path('/tmp/project'), transmit=False)
        self.assertEqual(args[args.index('--attenuation') + 1], '89.75')
        self.assertIn('--dry-run', args)
        self.assertNotIn('--transmit', args)
        self.assertIn('--transmit', settings.command(Path('/tmp/project'), transmit=True))
        self.assertTrue(any('ntsc_fpv_30m72.cs16' in item for item in args))

    def test_runtime_payload_and_preparation(self):
        settings = Settings.parse('5740,5800', 'NTSC', '89.75', '1000', '0', 'type=b200')
        payload = settings.control_payload(7)
        self.assertEqual(payload['id'], 7)
        self.assertEqual(payload['standard'], 'ntsc')
        self.assertEqual(payload['attenuation_db'], 89.75)
        self.assertEqual(payload['frequencies_hz'], [5740000000, 5800000000])
        args = settings.command(Path('/tmp/project'), transmit=True)
        self.assertIn('--live-control', args)
        self.assertIn('--alternate-metadata', args)
        with tempfile.TemporaryDirectory() as temporary:
            commands = settings.preparation(Path(temporary), all_standards=True)
            self.assertEqual(len(commands), 3) # build once, then both standards

    def test_invalid_inputs(self):
        good = ['5800', 'PAL', '89.75', '15', '0', 'type=b200']
        for index, bad in ((0,'nan'), (0,'5800,'), (0,'49'), (0,'6001'), (1,'SECAM'),
                           (2,'90'), (2,'-1'), (2,'0.1'), (2,'inf'), (3,'0'),
                           (3,'3601'), (4,'2'), (5,'')):
            args = good.copy()
            args[index] = bad
            with self.subTest(index=index, value=bad), self.assertRaises(ValueError):
                Settings.parse(*args)

    def test_missing_build_and_waveform_are_prepared(self):
        settings = Settings.parse('5800', 'PAL', '89.75', '15', '0', 'type=b200')
        with tempfile.TemporaryDirectory() as temporary:
            commands = settings.preparation(Path(temporary))
            self.assertEqual(len(commands), 2)
            self.assertIn('build.sh', ' '.join(commands[0][1]))
            self.assertIn('generate_fpv.py', ' '.join(commands[1][1]))
            self.assertTrue(all('--transmit' not in cmd for _, cmd in commands))


class RunnerTest(unittest.TestCase):
    def wait_done(self, runner):
        deadline = time.monotonic() + 8
        logs = []
        while time.monotonic() < deadline:
            kind, value = runner.events.get(timeout=8)
            if kind == 'log': logs.append(value)
            if kind == 'done':
                runner.thread.join(timeout=2)
                return value, ''.join(logs)
        self.fail('worker did not finish')

    def test_streams_output_and_preserves_exit_failure(self):
        runner = JobRunner()
        runner.start_commands([('Check', [sys.executable, '-u', '-c', "print('hello'); raise SystemExit(7)"]),
                               ('Must not run', [sys.executable, '-c', "print('unexpected')"])])
        code, logs = self.wait_done(runner)
        self.assertEqual(code, 7)
        self.assertIn('hello', logs)
        self.assertNotIn('unexpected', logs)

    def test_runtime_updates_use_owned_process_and_confirm_reply(self):
        runner = JobRunner()
        initial = Settings.parse('5800', 'PAL', '89.75', '30', '0', 'type=b200')
        runner.active_settings = initial
        script = """import json, sys, time
print('CONTROL ' + json.dumps(dict(id=0, status='ready')), flush=True)
request = json.loads(sys.stdin.readline())
time.sleep(.15)
request['status'] = 'applied'
print('CONTROL ' + json.dumps(request), flush=True)
sys.stdin.readline()
"""
        runner.start_commands([('Live dummy', [sys.executable, '-u', '-c', script])])
        self.addCleanup(lambda: (runner.stop(), runner.thread.join(timeout=8)))
        while True:
            kind, value = runner.events.get(timeout=5)
            if kind == 'control':
                self.assertEqual(value['status'], 'ready')
                break
        process = runner.process
        with self.assertRaises(ValueError):
            runner.apply(Settings.parse('5800', 'PAL', '89.75', '30', '1', 'type=b200'))
        updated = Settings.parse('5740,5860', 'NTSC', '89.5', '15', '0', 'type=b200')
        request_id = runner.apply(updated)
        self.assertEqual(request_id, 1)
        with self.assertRaises(RuntimeError): runner.apply(updated)
        while True:
            kind, value = runner.events.get(timeout=5)
            if kind == 'control':
                self.assertEqual(value['id'], request_id)
                self.assertEqual(value['status'], 'applied')
                self.assertEqual(value['frequencies_hz'], [5740000000, 5860000000])
                self.assertEqual(value['standard'], 'ntsc')
                break
        self.assertIs(runner.process, process)
        self.assertIsNone(runner.pending_id)
        runner.stop()
        self.wait_done(runner)

    def test_stop_interrupts_child_and_prevents_next_stage(self):
        runner = JobRunner()
        runner.start_commands([('Wait', [sys.executable, '-u', '-c',
            "import time; print('ready', flush=True); time.sleep(30)"]),
            ('Must not run', [sys.executable, '-c', "print('unexpected')"])])
        while True:
            kind, value = runner.events.get(timeout=5)
            if kind == 'log' and value.strip() == 'ready': break
        with self.assertRaises(RuntimeError): runner.start_commands([])
        runner.stop()
        code, logs = self.wait_done(runner)
        self.assertNotEqual(code, 0)
        self.assertNotIn('unexpected', logs)
        self.assertFalse(runner.busy)


if __name__ == '__main__':
    unittest.main()
