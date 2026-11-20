#!/usr/bin/env python3
"""Exercise real worker, RF simulation, HTTP spectrum and gain invalidation."""
import json
import socket
import subprocess
import sys
import time
import urllib.request
from pathlib import Path
with socket.socket() as s:
    s.bind(('127.0.0.1',0));port=s.getsockname()[1]
base=f'http://127.0.0.1:{port}'
def get():
    with urllib.request.urlopen(base+'/api/spectrum',timeout=3) as r:return json.load(r)
def post(path,body):
    with urllib.request.urlopen(urllib.request.Request(base+path,data=body.encode()),timeout=3) as r:assert r.status==200
def wait(predicate):
    until=time.monotonic()+5
    while time.monotonic()<until:
        try:
            j=get()
            if predicate(j):return j
        except OSError:pass
        time.sleep(.025)
    raise AssertionError('spectrum timeout')
p=subprocess.Popen([sys.argv[1],'--simulate','--listen','127.0.0.1','--port',str(port),'--web-root',sys.argv[2]],stdout=subprocess.DEVNULL)
try:
    wait(lambda j:not j['available'])
    post('/api/start','mode=lock&frequency_mhz=5800&gain_db=20')
    j=wait(lambda j:j['available'])
    assert j['bin_hz']==30000 and j['averages']==16 and j['unit']=='dBFS'
    b=j['bands'][0];assert len(b['epoch'])==len(b['values'])==len(b['lo_hz']);peak=max(range(len(b['values'])),key=lambda k:b['values'][k] if b['values'][k] is not None else -999)
    assert abs(b['start_hz']+peak*b['step_hz']-5800e6)<=300000
    rev=j['revision'];wait(lambda j:j['revision']>rev)
    post('/api/gain','gain_db=30');wait(lambda j:j['gain_db']==30 and j['available'])
    post('/api/stop','')
    with urllib.request.urlopen(base+'/spectrum.js',timeout=2) as r:assert b'canvas' in r.read()
    print('PASS spectrum HTTP: lock frequency, continuous updates, gain and offline asset')
finally:
    p.terminate();p.wait(timeout=3)
