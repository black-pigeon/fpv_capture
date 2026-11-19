#!/usr/bin/env python3
"""Real HTTP streaming with simulated captures; never operates RF hardware."""
import http.client
import json
from pathlib import Path
import socket
import subprocess
import sys
import time
import urllib.parse
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tools/fpv_receiver'))
from fpv_client import Client, iter_messages

binary,assets=sys.argv[1:]
with socket.socket() as sock:
    sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
p=subprocess.Popen([binary,'--simulate','--listen','127.0.0.1','--port',str(port),'--web-root',assets],stdout=subprocess.DEVNULL)
connections=[]
class Subscription:
    def __init__(self,conn,response):self.conn,self.response=conn,response
    def close(self):self.response.close();self.conn.close()

def request(method,path,body=''):
    conn=http.client.HTTPConnection('127.0.0.1',port,timeout=3)
    conn.request(method,path,body,{'Content-Type':'application/x-www-form-urlencoded'})
    response=conn.getresponse();status=response.status;data=response.read();conn.close();return status,data

def stream(query=''):
    conn=http.client.HTTPConnection('127.0.0.1',port,timeout=4)
    conn.request('GET','/api/v1/stream'+query);response=conn.getresponse()
    assert response.status==200,(response.status,response.read())
    assert response.getheader('Content-Type')=='multipart/mixed; boundary=fpv-stream-v1'
    subscription=Subscription(conn,response);connections.append(subscription)
    messages=iter_messages(response);hello,_=next(messages);assert hello['type']=='hello'
    return subscription,messages,hello

def next_frame(messages):
    for _ in range(8):
        meta,data=next(messages)
        if data is not None:return meta,data
    raise AssertionError('No frame received')

def lock(mhz):
    body=urllib.parse.urlencode(dict(mode='lock',frequency_mhz=str(mhz),gain_db=20,dwell_ms=100,settle_ms=0))
    for _ in range(100):
        status,data=request('POST','/api/start',body)
        if status==200:return
        assert status==409,(status,data)
        time.sleep(.02)
    raise AssertionError('Lock timed out')
try:
    for _ in range(100):
        try:
            if request('GET','/api/status')[0]==200:break
        except OSError:pass
        time.sleep(.02)
    else:raise AssertionError('Server did not start')
    all_conn,all_messages,hello=stream()
    meta,data=next(all_messages);assert meta['type']=='heartbeat' and data is None
    assert request('GET','/api/status')[0]==200
    selected,selected_messages,_=stream('?target_hz=5800000000')
    lock(5800)
    frames=[next_frame(all_messages)[0] for _ in range(5)]
    assert [f['frame_id'] for f in frames]==list(range(frames[0]['frame_id'],frames[0]['frame_id']+5))
    assert all(f['target_hz']==5800000000 and f['standard']=='PAL' and f['simulate'] for f in frames)
    assert len({f['epoch'] for f in frames})==1
    assert len({f['session_id'] for f in frames})==1
    assert next_frame(selected_messages)[0]['target_hz']==5800000000
    assert json.loads(request('GET','/api/status')[1])['ddc_tunes']==1
    last=frames[-1]['frame_id'];session=frames[-1]['session_id'];all_conn.close()
    resumed,resumed_messages,resumed_hello=stream(f'?session_id={session}&after={last}')
    assert resumed_hello['resume']=='resumed'
    assert next_frame(resumed_messages)[0]['frame_id']==last+1
    resumed.close();selected.close()
    time.sleep(.5)
    late,late_messages,_=stream(f'?session_id={session}&after=0')
    oldest,_=next_frame(late_messages)
    assert oldest['frame_id']>1 and oldest['dropped_before']==oldest['frame_id']-1
    late.close()
    # A surviving all-frequency stream follows tune changes without reconnect.
    live,live_messages,_=stream()
    lock(5860)
    for _ in range(20):
        meta,_=next_frame(live_messages)
        if meta['target_hz']==5860000000:
            assert meta['standard']=='NTSC' and meta['height']==480
            break
    else:raise AssertionError('Changed target did not arrive')
    only,only_messages,_=stream('?target_hz=5800000000')
    meta,data=next(only_messages);assert meta['type']=='heartbeat' and data is None
    assert request('POST','/api/stop')[0]==200
    live.close();only.close();time.sleep(1.2)
    # Invalid subscription queries must not consume any worker slot.
    for query in ('?target_hz=-1','?target_hz=1&target_hz=2','?after=1','?unknown=x',
                  '?target_hz=18446744073709551616','?session_id='+session+'&after=-1'):
        assert request('GET','/api/v1/stream'+query)[0]==400,query
    reset,_,reset_hello=stream('?session_id='+'0'*32+'&after=0')
    assert reset_hello['resume']=='session_reset';reset.close();time.sleep(1.2)
    held=[stream()[0] for _ in range(4)]
    assert request('GET','/api/v1/stream')[0]==503
    assert request('GET','/api/status')[0]==200
    assert request('GET','/')[0]==200
    # Stalled subscribers must not stop the acquisition worker.
    lock(5800)
    time.sleep(.4)
    before=json.loads(request('GET','/api/status')[1])['captured_frames']
    time.sleep(.4)
    assert json.loads(request('GET','/api/status')[1])['captured_frames']>before+2
    for conn in held:conn.close()
    time.sleep(1.2)
    # A third-party consumer uses the shipped SDK without web assets.
    client=Client(f'http://127.0.0.1:{port}')
    client.lock(5800)
    receiver=client.frames(target_hz=5800000000,reconnect=False)
    frame=next(receiver)
    assert frame.target_hz==5800000000 and frame.frequency_hz==5800000000 and frame.bmp[:2]==b'BM'
    receiver.close()
    assert client.status()['frequency_locked']
    client.stop()
    import tempfile
    with tempfile.TemporaryDirectory() as output:
        result=subprocess.run([sys.executable,str(Path(__file__).resolve().parents[2]/'tools/fpv_receiver/receive.py'),
            '--url',client.base_url,'--lock-mhz','5860','--frames','2','--output',output],capture_output=True,text=True,timeout=10)
        assert result.returncode==0,(result.stdout,result.stderr)
        records=[json.loads(line) for line in result.stdout.splitlines()]
        assert len(records)==2 and all(r['standard']=='NTSC' for r in records)
        assert len(list(Path(output).rglob('*.bmp')))==2
    client.stop()
    for _ in range(100):
        if not client.status()['running']:break
        time.sleep(.02)
    scan_connection,scan_messages,_=stream()
    client.scan('5800-5800,5860-5860',settle_ms=0,repeat=False)
    scan_frames=[next_frame(scan_messages)[0] for _ in range(2)]
    assert [(f['target_hz'],f['standard']) for f in scan_frames]==[(5800000000,'PAL'),(5860000000,'NTSC')]
    scan_connection.close()
    print('PASS stream: live ordered frames, matching metadata, filters, resume, heartbeat, control, client bounds')
finally:
    for conn in connections:conn.close()
    p.terminate()
    try:p.wait(timeout=5)
    except subprocess.TimeoutExpired:p.kill();p.wait();raise
