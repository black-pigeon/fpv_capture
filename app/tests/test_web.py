#!/usr/bin/env python3
"""Host-only end-to-end HTTP/worker/image tests with explicit simulated RF."""
import http.client
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import time
import urllib.parse

binary, assets = sys.argv[1:]
with socket.socket() as s:
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
p = subprocess.Popen([binary, '--simulate', '--listen', '127.0.0.1', '--port', str(port), '--web-root', assets], stdout=subprocess.DEVNULL)
def request(method, path, body=None, extra=None):
    c = http.client.HTTPConnection('127.0.0.1', port, timeout=3)
    h = {'Content-Type': 'application/x-www-form-urlencoded'}
    h.update(extra or {})
    c.request(method, path, body, h)
    r = c.getresponse()
    result = (r.status, r.getheader('Content-Type'), r.read())
    c.close()
    return result
try:
    for _ in range(100):
        try:
            if request('GET', '/api/status')[0] == 200: break
        except OSError: pass
        time.sleep(.02)
    else: raise AssertionError('server did not start')
    assert json.loads(request('GET', '/api/status')[2])['config']['step_mhz'] == 6
    assert json.loads(request('GET', '/api/status')[2])['config']['gain_db'] == 50
    html = request('GET', '/')[2].decode()
    assert '<html lang="en">' in html and 'Search grid' in html and 'Fast · 8 MHz' in html and 'Lock frequency' in html
    for asset in ('/', '/app.js'):
        text = request('GET', asset)[2].decode()
        assert not any('\u4e00' <= ch <= '\u9fff' for ch in text)
    assert request('GET', '/app.js')[0] == 200
    # The offline brand image must be served as a real PNG, not a 404/HTML page.
    status, kind, logo = request('GET', '/microphase-logo.png')
    assert status == 200 and kind == 'image/png', (status, kind)
    assert logo.startswith(b'\x89PNG\r\n\x1a\n')
    assert request('GET', '/../../etc/passwd')[0] == 404
    assert request('POST', '/api/start', 'bands=1-2')[0] == 400
    assert request('POST', '/api/start', 'bands=5798-5804', {'Origin':'http://other.invalid'})[0] == 403
    form = urllib.parse.urlencode(dict(bands='5798-5804', step_mhz='2', dwell_ms='100', settle_ms='0', gain_db='20', repeat='1'))
    assert request('POST', '/api/start', form)[0] == 200
    assert request('POST', '/api/start', form)[0] == 409
    for _ in range(100):
        state = json.loads(request('GET', '/api/status')[2])
        if state['tracks']: break
        time.sleep(.02)
    else: raise AssertionError(state)
    assert state['simulate'] and state['running']
    track = state['tracks'][0]
    assert track['epoch'] > 0 and abs(track['frequency_hz'] - 5800000000) <= 469
    status, kind, image = request('GET', f"/image/{track['id']}.bmp")
    assert status == 200 and kind == 'image/bmp' and image[:2] == b'BM'
    assert len(image) == 1078 + 720 * 576
    # Gain updates do not restart the scan or discard its current records.
    for invalid in ('', 'gain_db=', 'gain_db=-1', 'gain_db=61', 'gain_db=20.5',
                    'gain_db=NaN', 'gain_db=20&gain_db=21', 'gain_db=20&bands=5700-5800',
                    'gain_db=4294967296', 'gain_db=184467440737095516160'):
        assert request('POST', '/api/gain', invalid)[0] == 400, invalid
    assert request('POST', '/api/gain', 'gain_db=26', {'Origin':'http://other.invalid'})[0] == 403
    prior_frames, prior_id = state['captured_frames'], track['id']
    assert request('POST', '/api/gain', 'gain_db=26')[0] == 200
    for _ in range(150):
        state = json.loads(request('GET', '/api/status')[2])
        if state['config']['gain_db'] == 26 and not state['gain_pending']: break
        time.sleep(.02)
    else: raise AssertionError(state)
    assert state['running'] and state['mode'] == 'scan' and state['captured_frames'] >= prior_frames
    assert any(t['id'] == prior_id for t in state['tracks'])
    assert state['gain_updates'] == 1 and state['gain_target_db'] == 26
    assert state['config']['bands'] == [[5798000000, 5804000000]]
    # Lock directly from a running scan; keep one epoch while frames advance.
    assert request('POST', '/api/start', 'mode=lock&frequency_mhz=NaN')[0] == 400
    locked_form = urllib.parse.urlencode(dict(mode='lock', frequency_mhz='5800', gain_db='20',
        dwell_ms='100', settle_ms='0', repeat='0'))
    assert request('POST', '/api/start', locked_form, {'Origin':'http://other.invalid'})[0] == 403
    assert request('POST', '/api/start', locked_form)[0] == 200
    for _ in range(100):
        state = json.loads(request('GET', '/api/status')[2])
        if state['mode'] == 'lock' and state['frequency_locked'] and state['captured_frames'] >= 3: break
        time.sleep(.02)
    else: raise AssertionError(state)
    assert state['board'] == os.environ.get('FPV_EXPECT_BOARD', 'e200')
    assert state['rx_port'] == 'A_BALANCED'
    assert state['running'] and state['signal_present'] and state['rounds'] == 0
    assert state['lo_hz'] == state['lock_hz'] == 5800000000 and len(state['channels']) == 1
    assert state['ddc_tunes'] == state['rf_tunes'] == 1
    first_track = state['tracks'][0]
    time.sleep(.3)
    state = json.loads(request('GET', '/api/status')[2])
    assert state['running'] and state['captured_frames'] >= 5
    assert state['tracks'][0]['epoch'] == first_track['epoch']
    assert state['tracks'][0]['revision'] > first_track['revision']
    assert state['ddc_tunes'] == 1
    # Locked acquisition accepts gain changes without moving the LO/DDCs.
    prior_frames, prior_id = state['captured_frames'], state['tracks'][0]['id']
    assert request('POST', '/api/gain', 'gain_db=42')[0] == 200
    for _ in range(150):
        state = json.loads(request('GET', '/api/status')[2])
        if state['config']['gain_db'] == 42 and not state['gain_pending'] and state['captured_frames'] > prior_frames: break
        time.sleep(.02)
    else: raise AssertionError(state)
    assert state['running'] and state['mode'] == 'lock' and state['frequency_locked']
    assert state['lo_hz'] == state['lock_hz'] == 5800000000
    assert state['rf_tunes'] == state['ddc_tunes'] == 1 and state['gain_updates'] == 1
    assert state['tracks'][0]['id'] == prior_id and not state['error']
    # An empty frequency stays held; it must never fall back to scanning.
    assert request('POST', '/api/start', locked_form.replace('5800', '5700'))[0] == 200
    for _ in range(100):
        state = json.loads(request('GET', '/api/status')[2])
        if state['lock_hz'] == 5700000000 and state['frequency_locked']: break
        time.sleep(.02)
    time.sleep(.25)
    state = json.loads(request('GET', '/api/status')[2])
    assert state['running'] and not state['signal_present'] and not state['tracks'] and state['ddc_tunes'] == 1
    # Resume the previous scan without a separate stop request.
    assert request('POST', '/api/start', form)[0] == 200
    for _ in range(100):
        state = json.loads(request('GET', '/api/status')[2])
        if state['mode'] == 'scan' and state['tracks']: break
        time.sleep(.02)
    else: raise AssertionError(state)
    assert request('POST', '/api/stop', '')[0] == 200
    for _ in range(100):
        state = json.loads(request('GET', '/api/status')[2])
        if not state['running']: break
        time.sleep(.02)
    else: raise AssertionError('stop did not complete')
    assert request('POST', '/api/clear', '')[0] == 200
    assert not json.loads(request('GET', '/api/status')[2])['tracks']
    assert request('POST', '/api/start', form.replace('repeat=1','repeat=0'))[0] == 200
    for _ in range(100):
        state = json.loads(request('GET', '/api/status')[2])
        if not state['running']: break
        time.sleep(.02)
    assert not state['running'] and state['completed_points'] == state['total_points']
    form = urllib.parse.urlencode(dict(bands='5650-5700', step_mhz='2', dwell_ms='100', settle_ms='0', gain_db='20', repeat='0'))
    assert request('POST', '/api/start', form)[0] == 200
    for _ in range(150):
        state = json.loads(request('GET', '/api/status')[2])
        if not state['running']: break
        time.sleep(.02)
    assert not state['running'] and state['completed_points'] == 26
    assert state['ddc_tunes'] == 8 and state['rf_tunes'] == 2, state
    assert len(state['channels']) == 1 and state['window_phase'] == state['window_phases'] == 2
    for ch in state['channels']:
        assert abs(ch['offset_hz']) <= 20000469
        assert ch['frequency_hz'] == state['lo_hz'] + ch['offset_hz']
    # The test transmitter at 5800 MHz is reached only in the second
    # digital phase of the first RF window. It must be detected before
    # the worker advances its RF LO to the final window.
    form = urllib.parse.urlencode(dict(bands='5770-5812', step_mhz='6', dwell_ms='100', settle_ms='0', gain_db='20', repeat='0'))
    assert request('POST', '/api/start', form)[0] == 200
    observations = []
    for _ in range(150):
        state = json.loads(request('GET', '/api/status')[2])
        observations.append(state)
        if not state['running']: break
        time.sleep(.02)
    assert not state['running'] and state['completed_points'] == state['total_points'] == 8
    assert state['ddc_tunes'] == 3 and state['rf_tunes'] == 2, state
    for digital_phase in (1, 2):
        assert any(o['lo_hz'] == 5788000000 and o['window_phase'] == digital_phase
                   and o['rf_tunes'] == 1 for o in observations), observations
    assert any(t['target_hz'] == 5800000000 and t['lo_hz'] == 5788000000 for t in state['tracks']), state
    print('PASS web start/stop/config, same-origin protection, bounded scan worker, BMP image, single pass')
finally:
    p.terminate()
    try: p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        p.kill();p.wait();raise
