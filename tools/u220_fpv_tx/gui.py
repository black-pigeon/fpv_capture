#!/usr/bin/env python3
"""Local desktop controls for bounded U220 PAL/NTSC transmission."""
import argparse
from pathlib import Path
import queue
import tkinter as tk
from tkinter import ttk

from gui_support import JobRunner, Settings

ROOT = Path(__file__).resolve().parents[2]


class TransmitterGUI:
    def __init__(self, window, root=ROOT):
        self.window, self.root = window, Path(root)
        self.runner = JobRunner()
        self.closing = False
        self.running_mode = None
        self.pending_update = None
        self.confirmed = None
        self.controls = []
        self.preview_image = None
        self.frequency = tk.StringVar(value='5800')
        self.standard = tk.StringVar(value='PAL')
        self.attenuation = tk.StringVar(value='89.75')
        self.duration = tk.StringVar(value='15')
        self.channel = tk.StringVar(value='0')
        self.device_args = tk.StringVar(value='type=b200,serial=U220200')
        self.status = tk.StringVar(value='Idle — transmitter is stopped')
        self.detail = tk.StringVar(value='Set parameters, then check offline or start a timed transmission.')
        self.card_detail = tk.StringVar()
        window.title('U220 FPV Signal Source')
        window.geometry('1040x880')
        window.minsize(960, 820)
        window.configure(bg='#eef2f6')
        self._build()
        self._preview()
        window.protocol('WM_DELETE_WINDOW', self.close)
        self.poll_id = window.after(80, self._poll)

    def _build(self):
        style = ttk.Style(self.window)
        style.theme_use('clam')
        style.configure('.', font=('DejaVu Sans', 10), background='#eef2f6', foreground='#1c3042')
        style.configure('Title.TLabel', font=('DejaVu Sans', 22, 'bold'))
        style.configure('Subtitle.TLabel', foreground='#566a7e')
        style.configure('TLabelframe', padding=16)
        style.configure('TLabelframe.Label', font=('DejaVu Sans', 11, 'bold'))
        style.configure('TEntry', padding=6, fieldbackground='white')
        style.configure('TSpinbox', padding=6, fieldbackground='white')
        style.configure('TCombobox', padding=6, fieldbackground='white')
        style.configure('TButton', padding=(12, 9))
        style.configure('Start.TButton', background='#165dce', foreground='white', font=('DejaVu Sans', 10, 'bold'))
        style.map('Start.TButton', background=[('disabled', '#bbc8db'), ('active', '#104da9')])
        outer = ttk.Frame(self.window, padding=22)
        outer.pack(fill='both', expand=True)
        ttk.Label(outer, text='U220 FPV Signal Source', style='Title.TLabel').pack(anchor='w')
        ttk.Label(outer, text='PAL / NTSC grayscale FM  ·  USB UHD  ·  30.72 MSPS',
                  style='Subtitle.TLabel').pack(anchor='w', pady=(5, 16))
        body = ttk.Frame(outer)
        body.pack(fill='x')
        body.columnconfigure(0, weight=1, uniform='body')
        body.columnconfigure(1, weight=1, uniform='body')
        config = ttk.LabelFrame(body, text='Transmission settings')
        config.grid(row=0, column=0, sticky='nsew', padx=(0, 10))
        config.columnconfigure(1, weight=1)
        self._field(config, 0, 'Frequency / list (MHz)', ttk.Entry(config, textvariable=self.frequency))
        ttk.Label(config, text='Example: 5800 or 5740, 5800, 5860', style='Subtitle.TLabel').grid(
            row=1, column=0, columnspan=2, sticky='w', pady=(0, 12))
        standard = ttk.Combobox(config, textvariable=self.standard, values=('PAL', 'NTSC'), state='readonly', width=15)
        standard.bind('<<ComboboxSelected>>', lambda _: self._preview())
        self._field(config, 2, 'Video standard', standard)
        self._field(config, 3, 'TX attenuation (dB)', ttk.Spinbox(config, from_=0, to=89.75,
            increment=.25, textvariable=self.attenuation, width=15))
        ttk.Label(config, text='0 dB = strongest; 89.75 dB = weakest.\nRelative level control; RF power is not calibrated.',
            style='Subtitle.TLabel').grid(row=4, column=0, columnspan=2, sticky='w', pady=(0, 12))
        self._field(config, 5, 'Seconds per frequency', ttk.Spinbox(config, from_=1, to=3600,
            textvariable=self.duration, width=15))
        self._field(config, 6, 'UHD TX channel', ttk.Combobox(config, textvariable=self.channel,
            values=('0', '1'), state='readonly', width=15))
        ttk.Label(config, text='Last U220 probe: 0 = FE-TX2, 1 = FE-TX1.\nVerify the antenna connector on your unit.',
            style='Subtitle.TLabel').grid(row=7, column=0, columnspan=2, sticky='w', pady=(0, 12))
        ttk.Label(config, text='Device arguments').grid(row=8, column=0, columnspan=2, sticky='w', pady=(0, 5))
        args = ttk.Entry(config, textvariable=self.device_args)
        args.grid(row=9, column=0, columnspan=2, sticky='ew')
        self.controls.append(args)
        preview = ttk.LabelFrame(body, text='Source test card · preview')
        preview.grid(row=0, column=1, sticky='nsew', padx=(10, 0))
        self.image_label = ttk.Label(preview, anchor='center', background='#172027', foreground='white')
        self.image_label.pack(fill='both', expand=True)
        ttk.Label(preview, textvariable=self.card_detail, style='Subtitle.TLabel').pack(anchor='w', pady=(12, 4))
        ttk.Label(preview, text='Preview shows the source card.\nReceiver images are available in the E200 web UI.',
            style='Subtitle.TLabel').pack(anchor='w')
        actions = ttk.Frame(outer)
        actions.pack(fill='x', pady=(18, 12))
        self.check_button = ttk.Button(actions, text='Check offline', command=lambda: self.start(False))
        self.check_button.pack(side='left')
        self.start_button = ttk.Button(actions, text='Start TX', style='Start.TButton', command=lambda: self.start(True))
        self.start_button.pack(side='left', padx=10)
        self.stop_button = ttk.Button(actions, text='Stop TX', state='disabled', command=self.stop)
        self.stop_button.pack(side='left')
        self.apply_button = ttk.Button(actions, text='Apply changes', state='disabled', command=self.apply)
        self.apply_button.pack(side='left', padx=10)
        ttk.Label(actions, text='Edit settings, then Apply changes.', style='Subtitle.TLabel').pack(side='right')
        ttk.Label(outer, textvariable=self.status, font=('DejaVu Sans', 11, 'bold')).pack(anchor='w')
        ttk.Label(outer, textvariable=self.detail, style='Subtitle.TLabel', wraplength=970).pack(anchor='w', pady=(5, 12))
        log_frame = ttk.Frame(outer)
        log_frame.pack(fill='both', expand=True)
        self.log = tk.Text(log_frame, height=8, bg='#172027', fg='#c9e4d4', insertbackground='white',
            font=('DejaVu Sans Mono', 9), relief='flat', padx=12, pady=10, wrap='word', state='disabled')
        scrollbar = ttk.Scrollbar(log_frame, command=self.log.yview)
        self.log.configure(yscrollcommand=scrollbar.set)
        scrollbar.pack(side='right', fill='y')
        self.log.pack(side='left', fill='both', expand=True)
        self._append('Ready. Opening this window does not initialize the SDR.\n')

    def _field(self, frame, row, text, widget):
        ttk.Label(frame, text=text).grid(row=row, column=0, sticky='w', padx=(0, 12), pady=(0, 12))
        widget.grid(row=row, column=1, sticky='ew', pady=(0, 12))
        self.controls.append(widget)

    def _preview(self):
        standard = self.standard.get().lower()
        self.card_detail.set('PAL · 720 × 576 · 50 fields/s' if standard == 'pal' else
                             'NTSC · 720 × 480 · 59.94 fields/s')
        path = self.root / 'build/u220-fpv-source' / standard / 'test_card.png'
        try:
            self.preview_image = tk.PhotoImage(file=str(path)).subsample(2)
            self.image_label.configure(image=self.preview_image, text='')
        except tk.TclError:
            self.preview_image = None
            self.image_label.configure(image='', text='Test card appears after signal preparation.')

    def _append(self, text):
        self.log.configure(state='normal')
        self.log.insert('end', text)
        lines = int(self.log.index('end-1c').split('.')[0])
        if lines > 2000:
            self.log.delete('1.0', f'{lines - 1500}.0')
        self.log.see('end')
        self.log.configure(state='disabled')

    def _busy(self, active):
        for index, control in enumerate(self.controls):
            locked = active and not (self.running_mode == 'transmit' and index < 4 and not self.closing)
            control.configure(state='disabled' if locked else ('readonly' if isinstance(control, ttk.Combobox) else 'normal'))
        self.apply_button.configure(state='normal' if active and self.runner.live_ready and
            self.pending_update is None and not self.closing and not self.runner.cancelled.is_set() else 'disabled')
        self.check_button.configure(state='disabled' if active else 'normal')
        self.start_button.configure(state='disabled' if active else 'normal')
        self.stop_button.configure(state='normal' if active else 'disabled')

    def start(self, transmit):
        if self.closing or self.runner.busy:
            return
        try:
            settings = self._settings()
            self.runner.start(settings, self.root, transmit=transmit)
        except (ValueError, OSError, RuntimeError) as exc:
            self.status.set('Check settings')
            self.detail.set(str(exc))
            self._append(f'Error: {exc}\n')
            return
        self.pending_update = None
        self.confirmed = None
        self.running_mode = 'transmit' if transmit else 'offline'
        self._busy(True)
        self.status.set('Preparing ' + ('transmission' if transmit else 'offline check'))
        self.detail.set(f'Requested: {settings.standard.upper()} · {len(settings.frequencies_hz)} frequency center(s) · '
                        f'{settings.attenuation:g} dB attenuation · {settings.dwell:g} s per center')

    def _settings(self):
        return Settings.parse(self.frequency.get(), self.standard.get(), self.attenuation.get(),
                              self.duration.get(), self.channel.get(), self.device_args.get())

    def apply(self):
        try:
            settings = self._settings()
            request_id = self.runner.apply(settings)
        except (ValueError, RuntimeError) as exc:
            self.status.set('Changes not applied — check settings')
            self._append(f'Error: {exc}\n')
            return
        self.pending_update = request_id
        self.apply_button.configure(state='disabled')
        self.status.set(f'Applying changes (request {request_id})')

    def _control(self, reply):
        status, request_id = reply['status'], int(reply['id'])
        if status == 'ready' or (status == 'applied' and request_id == self.pending_update) or status == 'center':
            if status != 'center':
                self.pending_update = None
            self.confirmed = reply
            self.detail.set(f"Active: {reply['standard'].upper()} · {float(reply['frequency_hz']) / 1e6:g} MHz · "
                            f"{float(reply['attenuation_db']):g} dB attenuation · "
                            f"{float(reply['dwell_s']):g} s per center")
            self.status.set('Transmitting — changes applied' if status == 'applied' else 'Transmitting')
            self._busy(True)
        elif status == 'rejected' and request_id == self.pending_update:
            self.pending_update = None
            self.status.set('Changes rejected — previous settings remain active')
            self._append('Update rejected: ' + reply.get('error', 'Unknown error') + '\n')
            self._busy(True)

    def stop(self):
        self.runner.stop()
        self.apply_button.configure(state='disabled')
        self.stop_button.configure(state='disabled')
        self.status.set('Stopping — waiting for the current process to exit')

    def _poll(self):
        for _ in range(150):
            try:
                kind, value = self.runner.events.get_nowait()
            except queue.Empty:
                break
            if kind == 'log':
                self._append(value)
            elif kind == 'control' and not self.closing and not self.runner.cancelled.is_set():
                self._control(value)
            elif kind == 'phase' and not self.closing and not self.runner.cancelled.is_set():
                self.status.set(value)
            elif kind == 'done':
                self.pending_update = None
                self._busy(False)
                self._preview()
                if value == 0:
                    self.status.set('Offline check passed' if self.running_mode == 'offline' else 'Transmission finished')
                elif self.runner.cancelled.is_set():
                    self.status.set('Stopped')
                else:
                    self.status.set(f'Run failed (exit {value}) — see log')
                self.detail.set('Transmitter process has exited. Click Start TX to begin another run.')
        if self.closing and not self.runner.busy:
            self.window.destroy()
            return
        self.poll_id = self.window.after(80, self._poll)

    def close(self):
        self.closing = True
        if self.runner.busy:
            self.stop()
            self._busy(True)
            self.stop_button.configure(state='disabled')
        else:
            self.window.after_cancel(self.poll_id)
            self.window.destroy()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()
    try:
        window = tk.Tk()
    except tk.TclError as exc:
        raise SystemExit(f'Cannot open GUI: {exc}. Run in a desktop session with Tkinter installed.') from exc
    app = TransmitterGUI(window)
    try:
        window.mainloop()
    except KeyboardInterrupt:
        pass
    finally:
        if app.runner.busy:
            app.runner.stop()
            app.runner.thread.join(timeout=10)


if __name__ == '__main__':
    main()
