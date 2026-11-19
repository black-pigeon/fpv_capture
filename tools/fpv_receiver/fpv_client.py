"""E200 FPV HTTP stream v1. Python standard library only; see README.md."""
from dataclasses import dataclass
import http.client
import json
from pathlib import Path
import re
import struct
import time
import urllib.parse

MAX_IMAGE_BYTES = 1078 + 720 * 576
BOUNDARY = b'--fpv-stream-v1'
SESSION = re.compile(r'[0-9a-f]{32}\Z')

class ProtocolError(ValueError):
    """An untrusted stream failed framing or schema validation."""

class StreamDisconnected(ProtocolError, EOFError):
    """The transport ended before a complete message."""

@dataclass(frozen=True)
class Frame:
    metadata: dict
    bmp: bytes

    @property
    def target_hz(self):
        """Nominal search/lock center, used to group one capture target."""
        return self.metadata['target_hz']

    @property
    def frequency_hz(self):
        """Actual DDC center after NCO quantization, in Hz."""
        return self.metadata['frequency_hz']


def _exact(reader, length):
    chunks = bytearray()
    while len(chunks) < length:
        data = reader.read(length - len(chunks))
        if not data:
            raise StreamDisconnected('Truncated stream part')
        chunks.extend(data)
    return bytes(chunks)


def _json(data):
    try:
        result = json.loads(data)
    except (ValueError, UnicodeError, RecursionError) as error:
        raise ProtocolError('Invalid JSON metadata') from error
    if not isinstance(result, dict) or type(result.get('api_version')) is not int or result['api_version'] != 1:
        raise ProtocolError('Unsupported stream schema')
    if not isinstance(result.get('session_id'), str) or not SESSION.fullmatch(result['session_id']):
        raise ProtocolError('Invalid device session')
    return result


def _uint(meta, name, maximum=(1 << 64) - 1, minimum=0):
    value = meta.get(name)
    if type(value) is not int or not minimum <= value <= maximum:
        raise ProtocolError('Invalid ' + name)
    return value


def _validate_frame(meta, data):
    if meta.get('type') != 'frame' or type(meta.get('simulate')) is not bool:
        raise ProtocolError('Invalid frame kind')
    for key in ('frame_id', 'frequency_hz', 'target_hz', 'lo_hz'):
        _uint(meta, key, minimum=1)
    for key in ('captured_mono_ms', 'dropped_before'):
        _uint(meta, key)
    _uint(meta, 'epoch', (1 << 32) - 1)
    _uint(meta, 'channel', 3)
    _uint(meta, 'sync_quality', 100)
    height = {'PAL': 576, 'NTSC': 480}.get(meta.get('standard'))
    if not height or type(meta.get('width')) is not int or meta['width'] != 720 or type(meta.get('height')) is not int or meta['height'] != height:
        raise ProtocolError('Invalid video geometry/standard')
    if len(data) != 1078 + 720 * height or data[:2] != b'BM':
        raise ProtocolError('Invalid BMP size/signature')
    dib, width, rows, planes, depth, compression = struct.unpack_from('<IiiHHI', data, 14)
    if (dib, width, abs(rows), planes, depth, compression) != (40, 720, height, 1, 8, 0):
        raise ProtocolError('Unsupported BMP geometry/encoding')
    if struct.unpack_from('<I', data, 2)[0] != len(data) or struct.unpack_from('<I', data, 10)[0] != 1078:
        raise ProtocolError('Invalid BMP offsets')


def iter_messages(reader):
    """Yield (metadata, BMP-or-None) from a response/body binary reader.

    An EOF without a closing boundary is a truncated connection. Callers can
    reconnect with the last successfully processed session/frame cursor.
    """
    while True:
        line = reader.readline(256)
        if not line:
            raise StreamDisconnected('Stream connection closed')
        if line == BOUNDARY + b'--\r\n':
            return
        if not line.endswith(b'\n') and (BOUNDARY + b'\r\n').startswith(line):
            raise StreamDisconnected('Truncated multipart boundary')
        if line != BOUNDARY + b'\r\n':
            raise ProtocolError('Invalid or truncated multipart boundary')
        headers = {}
        total = 0
        while True:
            line = reader.readline(8193)
            total += len(line)
            if not line or (not line.endswith(b'\n') and len(line) < 8193):
                raise StreamDisconnected('Truncated part headers')
            if total > 8192 or not line.endswith(b'\r\n'):
                raise ProtocolError('Invalid or oversized part headers')
            if line == b'\r\n':
                break
            try:
                key, value = line[:-2].decode('ascii').split(':', 1)
            except (ValueError, UnicodeError, RecursionError) as error:
                raise ProtocolError('Invalid part header') from error
            key = key.lower()
            if key in headers:
                raise ProtocolError('Duplicate part header')
            headers[key] = value.strip()
        raw_size = headers.get('content-length', '')
        if not raw_size.isascii() or not raw_size.isdecimal() or len(raw_size) > 9:
            raise ProtocolError('Invalid content length')
        size = int(raw_size)
        kind = headers.get('content-type')
        if kind not in ('image/bmp', 'application/json') or not 0 < size <= (MAX_IMAGE_BYTES if kind == 'image/bmp' else 8192):
            raise ProtocolError('Invalid part type/size')
        body = _exact(reader, size)
        if _exact(reader, 2) != b'\r\n':
            raise ProtocolError('Invalid part terminator')
        if kind == 'image/bmp':
            meta = _json(headers.get('x-fpv-metadata', ''))
            _validate_frame(meta, body)
            yield meta, body
        else:
            meta = _json(body)
            if meta.get('type') not in ('hello', 'heartbeat'):
                raise ProtocolError('Unknown control message')
            yield meta, None


def save_frame(frame, root):
    """Save one BMP + JSON sidecar grouped by nominal Hz, standard and session."""
    meta = frame.metadata
    _validate_frame(meta, frame.bmp)
    if not isinstance(meta.get('session_id'), str) or not SESSION.fullmatch(meta['session_id']):
        raise ProtocolError('Invalid device session')
    folder = Path(root) / f"{meta['target_hz']}Hz" / meta['standard'] / meta['session_id']
    folder.mkdir(parents=True, exist_ok=True)
    name = f"{meta['frame_id']:020d}"
    image, record = folder / (name + '.bmp'), folder / (name + '.json')
    image.write_bytes(frame.bmp)
    record.write_text(json.dumps(dict(meta, received_unix_ns=time.time_ns()), indent=2) + '\n')
    return image, record


class Client:
    """Read/control one E200 and subscribe without accessing radio hardware.

    Each frames() iterator owns one HTTP connection. Process Frames promptly;
    a slow consumer can overrun the board's bounded history. No background
    unbounded queue is created on either side.
    """
    def __init__(self, base_url='http://192.168.1.10', timeout=5.0):
        url = urllib.parse.urlsplit(base_url)
        if url.scheme != 'http' or not url.hostname or url.username or url.password or url.query or url.fragment or url.path not in ('', '/'):
            raise ValueError('Use http://host[:port] without credentials or path')
        self.host, self.port = url.hostname, url.port if url.port is not None else 80
        if not 1 <= self.port <= 65535 or timeout <= 0:
            raise ValueError('Invalid port or timeout')
        self.base_url, self.timeout = base_url.rstrip('/'), timeout

    def _connection(self):
        return http.client.HTTPConnection(self.host, self.port, timeout=self.timeout)

    def _request(self, method, path, parameters=None):
        connection = self._connection()
        response = None
        try:
            body = urllib.parse.urlencode(parameters) if parameters is not None else None
            connection.request(method, path, body, {'Content-Type': 'application/x-www-form-urlencoded'})
            response = connection.getresponse()
            result = response.read(65537)
            if len(result) > 65536:
                raise ProtocolError('Oversized control response')
            if response.status != 200:
                raise RuntimeError(f'HTTP {response.status}: {result.decode(errors="replace")}')
            return result
        finally:
            if response is not None:
                response.close()
            connection.close()

    def status(self):
        return json.loads(self._request('GET', '/api/status'))

    def lock(self, frequency_mhz, gain_db=20, dwell_ms=150, settle_ms=20):
        self._request('POST', '/api/start', dict(mode='lock', frequency_mhz=frequency_mhz,
            gain_db=gain_db, dwell_ms=dwell_ms, settle_ms=settle_ms))

    def scan(self, bands='5650-5950', step_mhz=6, gain_db=20, dwell_ms=150, settle_ms=20, repeat=True):
        """bands uses the web API format, e.g. '5650-5800,5800-5950'."""
        self._request('POST', '/api/start', dict(mode='scan', bands=bands, step_mhz=step_mhz,
            gain_db=gain_db, dwell_ms=dwell_ms, settle_ms=settle_ms, repeat=int(repeat)))

    def stop(self):
        self._request('POST', '/api/stop', {})

    def frames(self, target_hz=None, reconnect=True, retry_seconds=1.0, on_event=None):
        """Yield Frame objects, optionally filtering an exact nominal center.

        Reconnects retain the last processed cursor within this iterator. A
        device restart starts a new session, never confuses old/new frame IDs.
        Protocol/schema errors are raised; only transport failures are retried.
        on_event receives hello/heartbeat and reconnect diagnostic dictionaries.
        """
        if target_hz is not None and (type(target_hz) is not int or not 0 < target_hz < 1 << 64):
            raise ValueError('target_hz must be a positive integer')
        if retry_seconds < 0:
            raise ValueError('retry_seconds must be nonnegative')
        session, cursor = None, None
        while True:
            parameters = {}
            if target_hz is not None:
                parameters['target_hz'] = target_hz
            if session is not None:
                parameters.update(session_id=session, after=cursor)
            path = '/api/v1/stream' + ('?' + urllib.parse.urlencode(parameters) if parameters else '')
            connection, response = self._connection(), None
            try:
                connection.request('GET', path)
                response = connection.getresponse()
                if response.status != 200:
                    message = response.read(4096).decode(errors='replace')
                    if response.status in (408, 429, 500, 502, 503, 504):
                        raise OSError(f'HTTP {response.status}: {message}')
                    raise ProtocolError(f'HTTP {response.status}: {message}')
                if response.getheader('Content-Type') != 'multipart/mixed; boundary=fpv-stream-v1':
                    raise ProtocolError('Server does not provide FPV stream v1')
                messages = iter_messages(response)
                try:
                    hello, image = next(messages)
                except StopIteration:
                    raise ProtocolError('Missing stream hello') from None
                if hello['type'] != 'hello' or image is not None:
                    raise ProtocolError('Missing stream hello')
                session = hello['session_id']
                cursor = _uint(hello, 'cursor')
                if on_event:
                    on_event(hello)
                for meta, image in messages:
                    if meta['session_id'] != session:
                        raise ProtocolError('Session changed within a connection')
                    if image is None:
                        if meta['type'] != 'heartbeat':
                            raise ProtocolError('Unexpected control message')
                        next_cursor = _uint(meta, 'cursor')
                        if next_cursor < cursor:
                            raise ProtocolError('Stream cursor moved backwards')
                        cursor = next_cursor
                        if on_event:
                            on_event(meta)
                        continue
                    if meta['frame_id'] <= cursor or (target_hz is not None and meta['target_hz'] != target_hz):
                        raise ProtocolError('Out-of-order or incorrectly filtered frame')
                    yield Frame(dict(meta), image)
                    cursor = meta['frame_id']
                if not reconnect:
                    return
            except (OSError, http.client.HTTPException, StreamDisconnected) as error:
                if not reconnect:
                    raise
                if on_event:
                    on_event(dict(type='reconnect', error=str(error)))
            finally:
                if response is not None:
                    response.close()
                connection.close()
            if not reconnect:
                return
            time.sleep(retry_seconds)
