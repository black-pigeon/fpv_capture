#!/usr/bin/env python3
"""Generate loopable grayscale PAL/NTSC wideband FM in raw little-endian CS16.

Adapted from DDM-P1 plutosdr-fw/fpv_signal_test/generate_pal.py.
The original PAL timing, grayscale card, video LPF and phase-closure method
are retained; NTSC timing and portable output metadata are added here.
No color subcarrier, audio or transmitter-specific pre-emphasis is modeled.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

FS = 30_720_000
STANDARDS = {
    'pal': dict(name='PAL 625/50', lines=625, height=576, samples=1_228_800,
                pulses=5, first=22, second=335, active_start=10.4e-6,
                frame_rate_num=25, frame_rate_den=1, default_frames=25),
    'ntsc': dict(name='NTSC 525/59.94', lines=525, height=480, samples=1_025_024,
                 pulses=6, first=20, second=283, active_start=10.5e-6,
                 frame_rate_num=30000, frame_rate_den=1001, default_frames=30),
}


def test_card(frame, frames, standard):
    """Reference grayscale bars, checkerboard, ramp and moving block."""
    height = standard['height']
    image = Image.new('L', (720, height), 24)
    draw = ImageDraw.Draw(image)
    font_path = Path('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf')
    font = ImageFont.truetype(str(font_path), 24) if font_path.exists() else ImageFont.load_default()
    draw.text((30, 22), f"U220 -> E200   {standard['name']}", fill=235, font=font)
    draw.text((30, 60), f'FM TEST   FRAME {frame:02d}/{frames:02d}', fill=235, font=font)
    for i in range(8):
        draw.rectangle((40 + i * 80, 105, 119 + i * 80, 190), fill=round(i * 255 / 7))
    for y in range(215, height - 130, 20):
        for x in range(40, 680, 20):
            draw.rectangle((x, y, x + 19, y + 19), fill=200 if ((x // 20 + y // 20) % 2) else 45)
    for x in range(40, 680):
        draw.line((x, height - 110, x, height - 80), fill=round((x - 40) * 255 / 639))
    x = 40 + round(600 * frame / max(1, frames - 1))
    draw.rectangle((x, height - 60, x + 39, height - 30), fill=245)
    draw.rectangle((20, 10, 699, height - 17), outline=180, width=3)
    return image


def video_frame(image, standard, filtered=True):
    """Composite video volts with half-line interlace and serrated V-sync.

    Integer rational clocks avoid cumulative horizontal/field boundary drift.
    Only complete active lines are drawn: even rows in field one, odd in two.
    """
    count, lines = standard['samples'], standard['lines']
    n = np.arange(count, dtype=np.int64)
    line = n * lines // count
    lt = (n * lines % count) / (FS * lines)
    field_half = ((n * 2 % count) * lines) // count
    half_t = (n * 2 * lines % count) / (2 * FS * lines)
    pulses = standard['pulses']
    v = np.zeros(count, dtype=np.float64)
    v[lt < 4.7e-6] = -.3
    broad = (field_half >= pulses) & (field_half < 2 * pulses)
    equal = (field_half < pulses) | ((field_half >= 2 * pulses) & (field_half < 3 * pulses))
    v[broad | equal] = 0
    half_period = count / (2 * lines * FS)
    v[broad & (half_t < half_period - 4.7e-6)] = -.3
    v[equal & (half_t < 2.35e-6)] = -.3
    row = np.full(count, -1, dtype=np.int32)
    for first, parity in ((standard['first'], 0), (standard['second'], 1)):
        active_lines = (line >= first) & (line < first + standard['height'] // 2)
        row[active_lines] = 2 * (line[active_lines] - first) + parity
    start = standard['active_start']
    active = (row >= 0) & (lt >= start) & (lt < start + 52e-6)
    xpos = (lt[active] - start) / 52e-6 * 720
    x0 = np.minimum(xpos.astype(int), 719)
    x1 = np.minimum(x0 + 1, 719)
    pixels = np.asarray(image, dtype=np.float64)
    if pixels.shape != (standard['height'], 720):
        raise ValueError('image dimensions do not match the selected standard')
    fraction = xpos - x0
    v[active] = .7 / 255 * (pixels[row[active], x0] * (1 - fraction) + pixels[row[active], x1] * fraction)
    if not filtered:
        return v
    n = np.arange(129) - 64
    h = 2 * 3e6 / FS * np.sinc(2 * 3e6 / FS * n) * np.hamming(129)
    h /= h.sum()
    return np.convolve(np.pad(v, (64, 64), mode='wrap'), h, mode='valid')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--standard', choices=STANDARDS, default='pal')
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--frames', type=int, help='complete frames, 2..51; default PAL 25, NTSC 30')
    parser.add_argument('--sensitivity', type=float, default=8e6, help='FM Hz per video volt')
    parser.add_argument('--amplitude', type=float, default=.5, help='complex magnitude / 32767')
    parser.add_argument('--polarity', type=int, choices=(-1, 1), default=1)
    args = parser.parse_args()
    standard = STANDARDS[args.standard]
    frames = args.frames if args.frames is not None else standard['default_frames']
    if (not 2 <= frames <= 51 or not math.isfinite(args.amplitude) or
            not 0 < args.amplitude <= .95 or not math.isfinite(args.sensitivity) or
            not 0 < args.sensitivity <= 10e6):
        parser.error('require frames 2..51, finite amplitude (0,.95], sensitivity (0,10e6]')
    args.output_dir.mkdir(parents=True, exist_ok=True)
    # Never leave a stale manifest certifying a file while it is being regenerated.
    manifest = args.output_dir / 'signal.json'
    manifest.unlink(missing_ok=True)
    path = args.output_dir / f'{args.standard}_fpv_30m72.cs16'
    cards = [test_card(i, frames, standard) for i in range(frames)]
    cards[0].save(args.output_dir / 'test_card.png')
    cycles = sum(float(np.sum(args.polarity * args.sensitivity * (video_frame(c, standard) - .2)))
                 / FS for c in cards)
    duration = frames * standard['samples'] / FS
    correction = (round(cycles) - cycles) / duration
    phase, peak_hz, max_error = 0.0, 0.0, 0.0
    previous = args.amplitude * 32767 + 0j
    digest = hashlib.sha256()
    with path.open('wb') as output:
        for index, card in enumerate(cards):
            freq = args.polarity * args.sensitivity * (video_frame(card, standard) - .2) + correction
            peak_hz = max(peak_hz, float(np.max(np.abs(freq))))
            angles = phase + (2 * np.pi / FS) * np.cumsum(freq)
            phase = float(angles[-1] % (2 * np.pi))
            iq = np.empty((freq.size, 2), dtype='<i2')
            iq[:, 0] = np.rint(args.amplitude * 32767 * np.cos(angles))
            iq[:, 1] = np.rint(args.amplitude * 32767 * np.sin(angles))
            data = iq.tobytes()
            output.write(data)
            digest.update(data)
            z = iq[:, 0].astype(float) + 1j * iq[:, 1]
            prev = np.concatenate(([previous], z[:-1]))
            fm = np.angle(z * np.conj(prev)) * FS / (2 * np.pi)
            previous = z[-1]
            max_error = max(max_error, float(np.max(np.abs(fm - freq))))
            print(f'frame {index + 1}/{frames}', flush=True)
    line_rate = FS * standard['lines'] / standard['samples']
    meta = dict(format='cs16_le', iq_file=path.name, sha256=digest.hexdigest(),
                sample_rate_hz=FS, samples=frames * standard['samples'], bytes=path.stat().st_size,
                standard=args.standard, description=standard['name'], width=720, height=standard['height'],
                frames=frames, duration_s=duration, line_rate_hz=line_rate,
                field_rate_hz=2 * FS / standard['samples'], lines_per_frame=standard['lines'],
                frame_rate_num=standard['frame_rate_num'], frame_rate_den=standard['frame_rate_den'],
                fm_sensitivity_hz_per_v=args.sensitivity, nominal_peak_deviation_hz=args.sensitivity / 2,
                polarity=args.polarity, complex_amplitude=args.amplitude,
                video_lpf_hz=3000000, loop_frequency_correction_hz=correction,
                final_phase_error_rad=math.remainder(phase, 2 * math.pi),
                peak_instantaneous_offset_hz=peak_hz, max_quantized_fm_error_hz=max_error,
                note='Grayscale timing test source; no chroma, audio or pre-emphasis. RF has not been tested.')
    manifest.write_text(json.dumps(meta, indent=2) + '\n')
    print(json.dumps(meta, indent=2))


if __name__ == '__main__':
    main()
