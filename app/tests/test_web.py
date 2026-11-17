#!/usr/bin/env python3
"""Host-only end-to-end HTTP/worker/image tests with explicit simulated RF."""
import http.client
import json
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
    html = request('GET', '/')[2].decode()
    assert '<html lang="en">' in html and 'Search grid' in html and 'Fast · 8 MHz' in html
    for asset in ('/', '/app.js'):
        text = request('GET', asset)[2].decode()
        assert not any('\u4e00' <= ch <= '\u9fff' for ch in text)
    assert request('GET', '/app.js')[0] == 200
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
    assert state['ddc_tunes'] == 7 and state['rf_tunes'] == 2, state
    assert len(state['channels']) == 2 and state['window_phase'] == state['window_phases'] == 2
    for ch in state['channels']:
        assert abs(ch['offset_hz']) <= 20000469
        assert ch['frequency_hz'] == state['lo_hz'] + ch['offset_hz']
    print('PASS web start/stop/config, same-origin protection, bounded scan worker, BMP image, single pass')
finally:
    p.terminate()
    try: p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        p.kill();p.wait();raise
