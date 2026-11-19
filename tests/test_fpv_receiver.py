"""The host API must reject broken wire data and separate recordings by frequency."""
import io
import http.server
import threading
import urllib.parse
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/fpv_receiver'))
from fpv_client import Client, Frame, ProtocolError, iter_messages, save_frame

SESSION='a'*32

def bmp(height=576):
    size=1078+720*height
    data=bytearray(size)
    struct.pack_into('<2sIHHI',data,0,b'BM',size,0,0,1078)
    struct.pack_into('<IiiHHIIiiII',data,14,40,720,height,1,8,0,720*height,0,0,256,256)
    return bytes(data)

def metadata(hz=5800000000,frame_id=1,standard='PAL'):
    return dict(api_version=1,type='frame',session_id=SESSION,frame_id=frame_id,
        target_hz=hz,frequency_hz=hz,lo_hz=hz,channel=0,standard=standard,width=720,
        height=576 if standard=='PAL' else 480,captured_mono_ms=1234,epoch=1,
        sync_quality=99,simulate=True,dropped_before=0)

def part(kind,body,meta=None):
    h=f'--fpv-stream-v1\r\nContent-Type: {kind}\r\nContent-Length: {len(body)}\r\n'
    if meta is not None:h+='X-FPV-Metadata: '+json.dumps(meta)+'\r\n'
    return h.encode()+b'\r\n'+body+b'\r\n'

class ReceiverTest(unittest.TestCase):
    def test_frames_heartbeat_and_final_boundary(self):
        hello=dict(api_version=1,type='hello',session_id=SESSION)
        wire=part('application/json',json.dumps(hello).encode())
        for standard in ('PAL','NTSC'):
            meta=metadata(standard=standard);wire+=part('image/bmp',bmp(meta['height']),meta)
        wire+=b'--fpv-stream-v1--\r\n'
        messages=list(iter_messages(io.BytesIO(wire)))
        self.assertEqual(messages[0],(hello,None))
        self.assertEqual([m[0]['standard'] for m in messages[1:]],['PAL','NTSC'])
    def test_bounded_framing_and_metadata(self):
        valid=part('image/bmp',bmp(),metadata())
        invalid=[valid[:-20],valid.replace(b'Content-Length: 415798',b'Content-Length: 999999999'),
                 valid.replace(b'Content-Length:',b'Unknown:'),
                 valid.replace(b'"PAL"',b'"XXX"'),valid.replace(b'"width": 720',b'"width": 721'),
                 valid.replace(b'"frame_id": 1',b'"frame_id": true'),
                 valid.replace(b'BM',b'ZZ',1),valid.replace(b'"api_version": 1',b'"api_version": true'),b'x'*9000+b'\n']
        for wire in invalid:
            with self.subTest(wire=wire[:100]):
                with self.assertRaises(ProtocolError):list(iter_messages(io.BytesIO(wire)))
    def test_reconnect_after_partial_headers_and_device_restart(self):
        requests=[]
        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self,*args):pass
            def do_GET(self):
                requests.append(urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query))
                index=len(requests)
                session=SESSION if index<3 else 'b'*32
                cursor=0 if index!=2 else 1
                hello=dict(api_version=1,type='hello',session_id=session,cursor=cursor,
                           resume='session_reset' if index==3 else 'resumed')
                wire=part('application/json',json.dumps(hello).encode())
                meta=metadata(frame_id=2 if index==2 else 1,standard='NTSC' if index==3 else 'PAL')
                meta['session_id']=session
                wire+=part('image/bmp',bmp(meta['height']),meta)
                if index==1:wire+=b'--fpv-stream-v1\r\nContent-Ty'
                else:wire+=b'--fpv-stream-v1--\r\n'
                self.send_response(200)
                self.send_header('Content-Type','multipart/mixed; boundary=fpv-stream-v1')
                self.send_header('Connection','close');self.end_headers();self.wfile.write(wire)
        server=http.server.HTTPServer(('127.0.0.1',0),Handler)
        thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
        try:
            events=[]
            receiver=Client(f'http://127.0.0.1:{server.server_port}').frames(retry_seconds=0,on_event=events.append)
            try:
                first=next(receiver)
                self.assertEqual(first.metadata['frame_id'],1)
                first.metadata['frame_id']=999  # User annotations must not corrupt resume state.
                frames=[next(receiver) for _ in range(2)]
            finally:receiver.close()
            self.assertEqual([f.metadata['frame_id'] for f in frames],[2,1])
            self.assertEqual(frames[-1].metadata['session_id'],'b'*32)
            self.assertEqual(requests[1],dict(session_id=[SESSION],after=['1']))
            self.assertEqual(requests[2],dict(session_id=[SESSION],after=['2']))
            self.assertTrue(any(e['type']=='reconnect' for e in events))
        finally:server.shutdown();server.server_close();thread.join()

    def test_nominal_and_actual_frequency_are_distinct(self):
        meta=metadata();meta['frequency_hz']+=300
        frame=Frame(meta,bmp())
        self.assertEqual(frame.target_hz,5800000000)
        self.assertEqual(frame.frequency_hz,5800000300)

    def test_save_by_frequency_standard_and_session(self):
        with tempfile.TemporaryDirectory() as d:
            for hz,standard in ((5800000000,'PAL'),(5860000000,'NTSC')):
                meta=metadata(hz,standard=standard);frame=Frame(meta,bmp(meta['height']))
                image,record=save_frame(frame,Path(d))
                self.assertEqual(image.parent,Path(d)/f'{hz}Hz'/standard/SESSION)
                self.assertEqual(image.read_bytes(),frame.bmp)
                self.assertEqual(json.loads(record.read_text())['target_hz'],hz)
                self.assertEqual(json.loads(record.read_text())['frame_id'],1)
            self.assertEqual(len(list(Path(d).rglob('*.bmp'))),2)

if __name__=='__main__':unittest.main()
