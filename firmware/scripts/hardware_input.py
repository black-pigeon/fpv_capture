#!/usr/bin/env python3
"""Validate a released board-specific XSA and extract its matching bitstream."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import sys
import tempfile
import zipfile

FIRMWARE = Path(__file__).resolve().parents[1]
PUBLIC_HARDWARE = FIRMWARE / 'board/e200/hardware'
DEFAULT_XSA = PUBLIC_HARDWARE / 'e200_fpv.xsa'
DEFAULT_MANIFEST = PUBLIC_HARDWARE / 'manifest.json'
MAX_XSA_BYTES = 32 * 1024 * 1024
MAX_EXPANDED_BYTES = 64 * 1024 * 1024
# Hardware handoff metadata and generated PS initialization only. No RTL,
# Vivado project/reconstruction scripts, checkpoints or debug probes.
COMMON_MEMBERS = frozenset({
    'system.bda', 'system.hwh',
    'system_fpv_ddr_0.hwh', 'system_spectrum_ddr_0.hwh', 'sysdef.xml', 'xsa.xml', 'xsa.json',
    'aie_primitive.json', 'ps7_init.c', 'ps7_init.h', 'ps7_init_gpl.c',
    'ps7_init_gpl.h', 'ps7_init.html', 'ps7_init.tcl',
})
SPECTRUM = {'register_version': 1, 'bins': 2048,
            'sample_rate_hz': 61440000, 'averages': 16, 'dma_count': 1,
            'control_base': '0x43c40000', 'dma_base': '0x40440000'}
CAPTURE = {'register_version': 6, 'packet_version': 2, 'channels': 4, 'dma_count': 1}
ETHERNET = {'phy_mode': 'rgmii-id', 'phy_address': 1,
            'converter_address': 8, 'forced_phy_id': False}
BOARD_ETHERNET = {
    'e200': ETHERNET,
    'e310': {'interface': 'ps-gem-mio', 'phy_mode': 'rgmii-id', 'phy_address': 0,
             'converter_address': None, 'forced_phy_id': False},
}
BOARD_MEMBERS = {
    'e200': COMMON_MEMBERS | {'e200_fpv.bit', 'e200_fpv.mmi'},
    'e310': COMMON_MEMBERS | {'e310_fpv.bit', 'e310_fpv.mmi'},
}
# Preserve the E200 constant for existing callers.
ALLOWED_MEMBERS = BOARD_MEMBERS['e200']
SOURCE_PAYLOAD = re.compile(
    rb'^\s*\+?\s*(?:(?:module|macromodule)\s+[a-z_]\w*|entity\s+\w+\s+is\b|'
    rb'(?:create_project|open_project|create_bd_design|create_bd_cell|'
    rb'read_verilog|read_vhdl|add_files|read_xdc|synth_design|write_bitstream)\b)',
    re.M | re.I)


def hardware_paths(board='e200'):
    if board not in BOARD_MEMBERS:
        raise ValueError(f'unsupported hardware board: {board}')
    directory = FIRMWARE / 'board' / board / 'hardware'
    return directory / f'{board}_fpv.xsa', directory / 'manifest.json'


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def validate_xsa(data, manifest, board='e200'):
    """Validate bytes against the public contract, returning embedded bit bytes.

    Hashes pin a reviewed release; they are not proof of hardware functionality.
    Keeping the public manifest schema small excludes private source inventories.
    """
    hardware_paths(board)
    bit_member = f'{board}_fpv.bit'
    allowed_members = BOARD_MEMBERS[board]
    if not isinstance(manifest, dict) or set(manifest) - {'spectrum'} != {
            'format', 'board', 'xsa', 'bitstream', 'capture', 'ethernet'}:
        raise ValueError('invalid public hardware manifest fields')
    if 'spectrum' in manifest and manifest['spectrum'] != SPECTRUM:
        raise ValueError('unsupported spectrum hardware contract')
    if manifest['format'] != f'{board}-fpv-xsa-v1' or manifest['board'] != board:
        raise ValueError('unsupported hardware manifest format or board')
    if manifest['capture'] != CAPTURE or manifest['ethernet'] != BOARD_ETHERNET[board]:
        raise ValueError('hardware capture ABI or Ethernet configuration differs')
    for key, filename_key, filename in (
            ('xsa', 'file', f'{board}_fpv.xsa'), ('bitstream', 'member', bit_member)):
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
            if set(names) - allowed_members:
                raise ValueError('unapproved XSA archive member: ' + ', '.join(sorted(set(names) - allowed_members)))
            if not {bit_member, 'system.hwh', 'xsa.xml'}.issubset(names):
                raise ValueError('XSA is missing bitstream or hardware description')
            if sum(entry.file_size for entry in entries) > MAX_EXPANDED_BYTES:
                raise ValueError('expanded XSA is too large')
            for entry in entries:
                kind = (entry.external_attr >> 16) & 0o170000
                if entry.flag_bits & 1 or kind not in (0, 0o100000):
                    raise ValueError('encrypted or non-regular XSA member')
                # Read every member to check CRCs, not just the bitstream.
                content = archive.read(entry)
                if entry.filename != bit_member and SOURCE_PAYLOAD.search(content):
                    raise ValueError('FPGA source payload in XSA member: ' + entry.filename)
            bit = archive.read(bit_member)
    except (zipfile.BadZipFile, RuntimeError, NotImplementedError) as error:
        raise ValueError(f'invalid XSA archive: {error}') from error
    if len(bit) != manifest['bitstream']['bytes'] or sha256(bit) != manifest['bitstream']['sha256']:
        raise ValueError('embedded bitstream size/SHA256 mismatch')
    return bit


def load_hardware(xsa=None, manifest=None, board='e200'):
    default_xsa, default_manifest = hardware_paths(board)
    xsa = default_xsa if xsa is None else xsa
    manifest = default_manifest if manifest is None else manifest
    record = json.loads(Path(manifest).read_text())
    bit = validate_xsa(Path(xsa).read_bytes(), record, board=board)
    return record, bit


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--board', choices=tuple(BOARD_MEMBERS), default='e200')
    parser.add_argument('--xsa', type=Path)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--output-bit', type=Path)
    args = parser.parse_args()
    try:
        default_xsa, default_manifest = hardware_paths(args.board)
        args.xsa = default_xsa if args.xsa is None else args.xsa
        args.manifest = default_manifest if args.manifest is None else args.manifest
        record, bit = load_hardware(args.xsa, args.manifest, board=args.board)
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
        print(f"PASS: {args.board.upper()} XSA and embedded bitstream; register v{record['capture']['register_version']}, packet v{record['capture']['packet_version']}")
    except (OSError, ValueError) as error:
        parser.exit(1, f'hardware input rejected: {error}\n')


if __name__ == '__main__':
    main()
