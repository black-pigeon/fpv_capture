#!/usr/bin/env python3
"""Validate the released E200 XSA and extract its matching bitstream."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import sys
import tempfile
import zipfile

PUBLIC_HARDWARE = Path(__file__).resolve().parents[1] / 'board/e200/hardware'
DEFAULT_XSA = PUBLIC_HARDWARE / 'e200_fpv.xsa'
DEFAULT_MANIFEST = PUBLIC_HARDWARE / 'manifest.json'
MAX_XSA_BYTES = 32 * 1024 * 1024
MAX_EXPANDED_BYTES = 64 * 1024 * 1024
# Hardware handoff metadata and generated PS initialization only. No RTL,
# Vivado project/reconstruction scripts, checkpoints or debug probes.
ALLOWED_MEMBERS = frozenset({
    'e200_fpv.bit', 'e200_fpv.mmi', 'system.bda', 'system.hwh',
    'system_fpv_ddr_0.hwh', 'sysdef.xml', 'xsa.xml', 'xsa.json',
    'aie_primitive.json', 'ps7_init.c', 'ps7_init.h', 'ps7_init_gpl.c',
    'ps7_init_gpl.h', 'ps7_init.html', 'ps7_init.tcl',
})
CAPTURE = {'register_version': 6, 'packet_version': 2, 'channels': 4, 'dma_count': 1}
ETHERNET = {'phy_mode': 'rgmii-id', 'phy_address': 1,
            'converter_address': 8, 'forced_phy_id': False}


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def validate_xsa(data, manifest):
    """Validate bytes against the public contract, returning embedded bit bytes.

    Hashes pin a reviewed release; they are not proof of hardware functionality.
    Keeping the public manifest schema small excludes private source inventories.
    """
    if not isinstance(manifest, dict) or set(manifest) != {
            'format', 'board', 'xsa', 'bitstream', 'capture', 'ethernet'}:
        raise ValueError('invalid public hardware manifest fields')
    if manifest['format'] != 'e200-fpv-xsa-v1' or manifest['board'] != 'e200':
        raise ValueError('unsupported hardware manifest format or board')
    if manifest['capture'] != CAPTURE or manifest['ethernet'] != ETHERNET:
        raise ValueError('hardware capture ABI or Ethernet configuration differs')
    for key, filename_key, filename in (
            ('xsa', 'file', 'e200_fpv.xsa'), ('bitstream', 'member', 'e200_fpv.bit')):
        item = manifest[key]
        if not isinstance(item, dict) or set(item) != {filename_key, 'sha256', 'bytes'}:
            raise ValueError(f'invalid {key} manifest fields')
        if item[filename_key] != filename:
            raise ValueError(f'invalid {key} filename')
        if not isinstance(item['sha256'], str) or not re.fullmatch(r'[0-9a-f]{64}', item['sha256']):
            raise ValueError(f'invalid {key} SHA256')
        if type(item['bytes']) is not int or item['bytes'] <= 0:
            raise ValueError(f'invalid {key} byte count')
    if not data or len(data) > MAX_XSA_BYTES:
        raise ValueError('XSA size is outside the release limit')
    if len(data) != manifest['xsa']['bytes'] or sha256(data) != manifest['xsa']['sha256']:
        raise ValueError('XSA size/SHA256 mismatch')
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            entries = archive.infolist()
            names = [entry.filename for entry in entries]
            if len(names) != len(set(names)):
                raise ValueError('duplicate XSA archive member')
            if set(names) - ALLOWED_MEMBERS:
                raise ValueError('unapproved XSA archive member: ' + ', '.join(sorted(set(names) - ALLOWED_MEMBERS)))
            if not {'e200_fpv.bit', 'system.hwh', 'xsa.xml'}.issubset(names):
                raise ValueError('XSA is missing bitstream or hardware description')
            if sum(entry.file_size for entry in entries) > MAX_EXPANDED_BYTES:
                raise ValueError('expanded XSA is too large')
            for entry in entries:
                kind = (entry.external_attr >> 16) & 0o170000
                if entry.flag_bits & 1 or kind not in (0, 0o100000):
                    raise ValueError('encrypted or non-regular XSA member')
                # Read every member to check CRCs, not just the bitstream.
                archive.read(entry)
            bit = archive.read('e200_fpv.bit')
    except (zipfile.BadZipFile, RuntimeError, NotImplementedError) as error:
        raise ValueError(f'invalid XSA archive: {error}') from error
    if len(bit) != manifest['bitstream']['bytes'] or sha256(bit) != manifest['bitstream']['sha256']:
        raise ValueError('embedded bitstream size/SHA256 mismatch')
    return bit


def load_hardware(xsa=DEFAULT_XSA, manifest=DEFAULT_MANIFEST):
    record = json.loads(Path(manifest).read_text())
    bit = validate_xsa(Path(xsa).read_bytes(), record)
    return record, bit


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--xsa', type=Path, default=DEFAULT_XSA)
    parser.add_argument('--manifest', type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument('--output-bit', type=Path)
    args = parser.parse_args()
    try:
        record, bit = load_hardware(args.xsa, args.manifest)
        if args.output_bit:
            if args.output_bit.resolve() in (args.xsa.resolve(), args.manifest.resolve()):
                raise ValueError('bitstream output would overwrite an input')
            args.output_bit.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.NamedTemporaryFile(dir=args.output_bit.parent, delete=False) as output:
                temporary = Path(output.name)
                output.write(bit)
            try:
                temporary.chmod(0o644)
                temporary.replace(args.output_bit)
            finally:
                temporary.unlink(missing_ok=True)
        print(f"PASS: E200 XSA and embedded bitstream; register v{record['capture']['register_version']}, packet v{record['capture']['packet_version']}")
    except (OSError, ValueError) as error:
        parser.exit(1, f'hardware input rejected: {error}\n')


if __name__ == '__main__':
    main()
