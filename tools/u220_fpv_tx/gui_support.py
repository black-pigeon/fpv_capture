"""GUI-independent validation and owned-subprocess lifecycle for the U220 tool."""
from dataclasses import dataclass
import json
import math
import os
from pathlib import Path
import queue
import shlex
import signal
import subprocess
import sys
import threading


def finite_number(value, label):
    try:
        number = float(value)
    except (TypeError, ValueError) as exc:
        raise ValueError(f'{label} must be a number.') from exc
    if not math.isfinite(number):
        raise ValueError(f'{label} must be finite.')
    return number


@dataclass(frozen=True)
class Settings:
    frequencies_hz: tuple
    standard: str
    attenuation: float
    dwell: float
    channel: int
    device_args: str

    @classmethod
    def parse(cls, frequency, standard, attenuation, dwell, channel, device_args):
        frequencies = []
        for item in frequency.split(','):
            value = finite_number(item.strip(), 'Frequency (MHz)')
            if not 50 <= value <= 6000:
                raise ValueError('Frequency must be between 50 and 6000 MHz.')
            frequencies.append(round(value * 1e6))
        if len(frequencies) > 128:
            raise ValueError('Use at most 128 frequency centers.')
        standard = standard.lower()
        if standard not in ('pal', 'ntsc'):
            raise ValueError('Select PAL or NTSC.')
        attenuation = finite_number(attenuation, 'TX attenuation')
        if not 0 <= attenuation <= 89.75 or not math.isclose(attenuation * 4, round(attenuation * 4), abs_tol=1e-6):
            raise ValueError('TX attenuation must be 0..89.75 dB in 0.25 dB steps.')
        dwell = finite_number(dwell, 'Duration')
        if not 0 < dwell <= 3600:
            raise ValueError('Duration per center must be greater than 0 and at most 3600 seconds.')
        if str(channel) not in ('0', '1'):
            raise ValueError('TX channel must be 0 or 1.')
        if not device_args.strip():
            raise ValueError('Device arguments must not be empty.')
        return cls(tuple(frequencies), standard, attenuation, dwell, int(channel), device_args.strip())

    def control_payload(self, request_id):
        return dict(id=request_id, frequencies_hz=list(self.frequencies_hz), standard=self.standard,
                    attenuation_db=self.attenuation, dwell_s=self.dwell)

    def command(self, root, transmit):
        base = Path(root) / 'build/u220-fpv-source'
        directory = base / self.standard
        command = [str(base / 'u220_fpv_tx'), '--input', str(directory / f'{self.standard}_fpv_30m72.cs16'),
                '--metadata', str(directory / 'signal.json'), '--args', self.device_args,
                '--channel', str(self.channel), '--freqs', ','.join(map(str, self.frequencies_hz)),
                '--attenuation', f'{self.attenuation:g}', '--dwell', f'{self.dwell:g}',
                '--transmit' if transmit else '--dry-run']
        if transmit:
            alternate = 'ntsc' if self.standard == 'pal' else 'pal'
            command += ['--live-control', '--alternate-input', str(base / alternate / f'{alternate}_fpv_30m72.cs16'),
                        '--alternate-metadata', str(base / alternate / 'signal.json')]
        return command

    def preparation(self, root, all_standards=False):
        root = Path(root)
        source = root / 'tools/u220_fpv_tx'
        base = root / 'build/u220-fpv-source'
        binary = base / 'u220_fpv_tx'
        commands = []
        inputs = [source / name for name in ('u220_fpv_tx.cpp', 'replay.hpp', 'tx_events.hpp', 'test_tx_events.cpp', 'live_control.hpp', 'test_live_control.cpp', 'build.sh')]
        if not binary.is_file() or any(p.is_file() and p.stat().st_mtime > binary.stat().st_mtime for p in inputs):
            commands.append(('Building sender', ['bash', str(source / 'build.sh'), str(base)]))
        for standard in (('pal', 'ntsc') if all_standards else (self.standard,)):
            directory = base / standard
            manifest = directory / 'signal.json'
            iq = directory / f'{standard}_fpv_30m72.cs16'
            try:
                meta = json.loads(manifest.read_text())
                reusable = (iq.is_file() and meta['standard'] == standard and
                            meta['bytes'] == iq.stat().st_size and meta['iq_file'] == iq.name and
                            manifest.stat().st_mtime >= (source / 'generate_fpv.py').stat().st_mtime)
            except (OSError, ValueError, KeyError, TypeError):
                reusable = False
            if not reusable:
                commands.append((f'Generating {standard.upper()} signal',
                                 [sys.executable, str(source / 'generate_fpv.py'), '--standard', standard,
                                  '--output-dir', str(directory)]))
        return commands


class JobRunner:
    """Run one pipeline; cancellation applies to only its own process groups."""
    def __init__(self):
        self.events = queue.Queue()
        self.cancelled = threading.Event()
        self.lock = threading.Lock()
        self.process = None
        self.thread = None
        self.live_ready = False
        self.active_settings = None
        self.request_id = 0
        self.pending_id = None

    @property
    def busy(self):
        return self.thread is not None and self.thread.is_alive()

    def start(self, settings, root, transmit=False):
        commands = settings.preparation(root, all_standards=transmit)
        self.active_settings = settings
        commands.append(('Starting TX' if transmit else 'Checking offline', settings.command(root, transmit)))
        self.start_commands(commands)

    def start_commands(self, commands):
        if self.busy:
            raise RuntimeError('A job is already running.')
        self.cancelled.clear()
        self.live_ready = False
        self.pending_id = None
        self.request_id = 0
        self.thread = threading.Thread(target=self._run, args=(commands,), daemon=True)
        self.thread.start()

    def _run(self, commands):
        code = 0
        try:
            for label, command in commands:
                with self.lock:
                    if self.cancelled.is_set():
                        code = 130
                        break
                    self.events.put(('phase', label))
                    self.events.put(('log', '$ ' + shlex.join(command) + '\n'))
                    self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        text=True, encoding='utf-8', errors='replace', start_new_session=True,
                        env={**os.environ, 'PYTHONUNBUFFERED': '1'})
                    process = self.process
                with process.stdout:
                    for line in process.stdout:
                        self.events.put(('log', line))
                        if line.startswith('CONTROL '):
                            try:
                                reply = json.loads(line[8:])
                                with self.lock:
                                    if reply['status'] == 'ready':
                                        self.live_ready = True
                                    elif int(reply['id']) == self.pending_id:
                                        self.pending_id = None
                                self.events.put(('control', reply))
                            except (ValueError, KeyError, TypeError):
                                self.events.put(('log', 'Invalid control reply from sender.\n'))
                code = process.wait()
                process.stdin.close()
                with self.lock:
                    self.process = None
                if code or self.cancelled.is_set():
                    break
        except Exception as exc:
            self.events.put(('log', f'Error: {exc}\n'))
            code = 1
        finally:
            with self.lock:
                self.process = None
            self.live_ready = False
            self.pending_id = None
            self.events.put(('done', 130 if self.cancelled.is_set() else code))

    def apply(self, settings):
        with self.lock:
            if (not self.live_ready or self.pending_id is not None or self.cancelled.is_set()
                    or self.process is None or self.process.poll() is not None):
                raise RuntimeError('Wait for the transmitter to be ready before applying changes.')
            if self.active_settings and (settings.channel != self.active_settings.channel or
                                        settings.device_args != self.active_settings.device_args):
                raise ValueError('Stop TX before changing device arguments or TX channel.')
            self.request_id += 1
            request_id = self.request_id
            self.pending_id = request_id
            try:
                self.process.stdin.write(json.dumps(settings.control_payload(request_id)) + '\n')
                self.process.stdin.flush()
            except (OSError, ValueError):
                self.pending_id = None
                raise RuntimeError('Transmitter control pipe has closed.') from None
            return request_id

    def stop(self):
        with self.lock:
            if self.cancelled.is_set():
                return
            self.cancelled.set()
            process = self.process
        if process is not None:
            threading.Thread(target=self._stop_process, args=(process,), daemon=True).start()

    def _stop_process(self, process):
        # Native sender first gets SIGINT to emit EOB and drain queued samples.
        # Escalation also handles a hung build or signal generator on window close.
        for sig, timeout in ((signal.SIGINT, 5), (signal.SIGTERM, 2), (signal.SIGKILL, 2)):
            if process.poll() is not None:
                return
            try:
                os.killpg(process.pid, sig)
            except ProcessLookupError:
                return
            try:
                process.wait(timeout=timeout)
                return
            except subprocess.TimeoutExpired:
                self.events.put(('log', 'Waiting for process to stop; escalating termination.\n'))
