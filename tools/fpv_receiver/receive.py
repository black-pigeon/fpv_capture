#!/usr/bin/env python3
"""Record E200 FPV captures by frequency, using the public HTTP API."""
import argparse
from decimal import Decimal, InvalidOperation
import http.client
import json
from pathlib import Path
import sys
from fpv_client import Client, save_frame


def mhz_hz(text):
    try:
        hz = Decimal(text) * 1000000
        if not hz.is_finite() or hz != hz.to_integral_value() or not 70000000 <= hz <= 6000000000:
            raise ValueError()
        return int(hz)
    except (ValueError, InvalidOperation):
        raise argparse.ArgumentTypeError('Use 70..6000 MHz with at most six decimal places') from None


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', default='http://192.168.1.10', help='SDR HTTP origin')
    parser.add_argument('--output', type=Path, default=Path('fpv-recordings'))
    parser.add_argument('--frequency-mhz', type=mhz_hz, action='append', default=[],
                        help='Subscribe to nominal MHz center; repeat for several targets; default all')
    control = parser.add_mutually_exclusive_group()
    control.add_argument('--lock-mhz', type=mhz_hz, help='Ask SDR to lock this center before receiving')
    control.add_argument('--scan-bands', help='Ask SDR to scan MHz ranges, e.g. 5650-5800,5800-5950')
    parser.add_argument('--gain-db', type=int, default=20)
    parser.add_argument('--step-mhz', default='6')
    parser.add_argument('--dwell-ms', type=int, default=150)
    parser.add_argument('--frames', type=int, default=0, help='Exit after N saved frames; 0 runs until Ctrl-C')
    args = parser.parse_args(argv)
    if args.frames < 0:
        parser.error('--frames must be nonnegative')
    targets = set(args.frequency_mhz)
    if args.lock_mhz and targets and args.lock_mhz not in targets:
        parser.error('The frequency filter excludes --lock-mhz')
    client = Client(args.url)
    if args.lock_mhz:
        client.lock(Decimal(args.lock_mhz) / 1000000, gain_db=args.gain_db, dwell_ms=args.dwell_ms)
    elif args.scan_bands:
        client.scan(args.scan_bands, step_mhz=args.step_mhz, gain_db=args.gain_db, dwell_ms=args.dwell_ms)

    def event(meta):
        if meta['type'] == 'reconnect':
            print('Reconnecting: ' + meta['error'], file=sys.stderr, flush=True)
        elif meta['type'] == 'hello':
            print(f"Connected: session={meta['session_id']} resume={meta['resume']}", file=sys.stderr, flush=True)
        if meta.get('dropped_before'):
            print(f"History overrun: {meta['dropped_before']} global frames skipped", file=sys.stderr, flush=True)

    receiver = client.frames(target_hz=next(iter(targets)) if len(targets) == 1 else None, on_event=event)
    count = 0
    try:
        for frame in receiver:
            if targets and frame.target_hz not in targets:
                continue
            if frame.metadata['dropped_before']:
                print(f"History overrun: {frame.metadata['dropped_before']} global frames skipped", file=sys.stderr, flush=True)
            image, record = save_frame(frame, args.output)
            print(json.dumps(dict(frame.metadata, image_path=str(image), metadata_path=str(record))), flush=True)
            count += 1
            if args.frames and count >= args.frames:
                break
    finally:
        receiver.close()
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(0)
    except (OSError, ValueError, RuntimeError, http.client.HTTPException) as error:
        print(f'Receiver error: {error}', file=sys.stderr)
        sys.exit(1)
