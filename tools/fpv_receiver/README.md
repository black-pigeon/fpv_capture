# E200 FPV receiver and network API v1

The SDR exports validated captures over Ethernet. A host can receive all search
centers in one connection or subscribe to one nominal center. It can display,
record or analyze images without running those tasks on the SDR. The Python
SDK and recorder use only Python 3's standard library.

The FPGA still demodulates video, and the SDR still owns RF/DDC tuning and DMA.
No FPGA source, new bitstream, kernel change or separate network port is needed.
Install the rootfs containing this version of `fpv-webd`.

## Receive images

From the repository root, receive the currently active acquisition:

```sh
python3 tools/fpv_receiver/receive.py --url http://192.168.1.10 --output fpv-recordings
```

Start a repeated scan remotely, then receive every detected center:

```sh
python3 tools/fpv_receiver/receive.py --url http://192.168.1.10 \
  --scan-bands '5650-5800,5800-5950' --step-mhz 6 --gain-db 40 \
  --output fpv-recordings
```

Lock one frequency and continuously record its accepted captures:

```sh
python3 tools/fpv_receiver/receive.py --url http://192.168.1.10 \
  --lock-mhz 5800 --gain-db 40 --output fpv-recordings
```

Subscribe to a center without changing the SDR's current scan/lock mode:

```sh
python3 tools/fpv_receiver/receive.py --url http://192.168.1.10 \
  --frequency-mhz 5800 --output fpv-recordings --frames 100
```

Repeat `--frequency-mhz` to accept several exact nominal centers. One filter is
applied by the server; multiple filters use one all-center stream and filter on
the host. Filters must match the actual search grid or locked target; filtering
5800 MHz will not match a nominal 5798 MHz hit from the same transmitter.
`--frames 0` (default) records until Ctrl-C. Stopping the recorder closes only its
subscription; it leaves SDR acquisition running. Use `Client.stop()` or the web
UI to stop acquisition. `--lock-mhz`/`--scan-bands` control the shared receiver
and therefore also change what the web UI and other subscribers see.

Output is organized by nominal frequency, video standard and process session:

```text
fpv-recordings/
  5800000000Hz/PAL/<session_id>/00000000000000000001.bmp
  5800000000Hz/PAL/<session_id>/00000000000000000001.json
  5860000000Hz/NTSC/<session_id>/00000000000000000028.bmp
  5860000000Hz/NTSC/<session_id>/00000000000000000028.json
```

The JSON sidecar contains the image's metadata and host `received_unix_ns`.
The recorder prints one JSON line per saved frame on stdout and connection/gap
messages on stderr. BMPs are uncompressed 8-bit grayscale (about 416 kB for PAL
or 347 kB for NTSC); continuous recording consumes host disk space at the
actual capture rate. `--frames` bounds a recording by frame count.

## Python integration

Add `tools/fpv_receiver` to your Python import path, or copy `fpv_client.py` into
your application. `Client.frames()` yields a `Frame` containing metadata and
BMP bytes. Callers can create a window, queue or writer per `frame.target_hz`.

```python
from fpv_client import Client, save_frame

sdr = Client('http://192.168.1.10')
# Optional: sdr.lock(5800, gain_db=40)
# Optional: sdr.scan('5650-5950', step_mhz=6, gain_db=40)

stream = sdr.frames()  # target_hz=5800000000 selects one nominal center
try:
    for frame in stream:
        print(frame.target_hz, frame.metadata['standard'],
              frame.metadata['frame_id'], len(frame.bmp))
        save_frame(frame, 'fpv-recordings')
        # Or hand frame.bmp and frame.metadata to your own processing code.
finally:
    stream.close()
```

The iterator reconnects after transport failures, with the last processed
session/cursor. It raises `ProtocolError` for invalid protocol or image data.
Use `reconnect=False` to surface transport failures to your own connection
manager. `on_event=callback` exposes hello/heartbeat/reconnect messages.
`status()`, `scan()`, `lock()` and `stop()` wrap the existing HTTP control API.
A control request rejected while acquisition is switching raises `RuntimeError`;
wait for `status()['switching']`/`status()['stopping']` to clear before retrying.

## HTTP wire format

Request:

```http
GET /api/v1/stream HTTP/1.1
Host: 192.168.1.10
```

Optional query parameters:

| Parameter | Meaning |
| --- | --- |
| `target_hz` | Positive decimal integer; exact nominal scan/lock center. Omit for all centers. |
| `session_id` | The 32 lowercase hexadecimal characters returned by the prior connection. Requires `after`. |
| `after` | Last consumed global frame ID, decimal unsigned 64-bit. Requires `session_id`. Zero is valid. |

Example subscription URL:

```text
http://192.168.1.10/api/v1/stream?target_hz=5800000000
```

Response: HTTP 200 with `Content-Type: multipart/mixed; boundary=fpv-stream-v1`,
`Cache-Control: no-store` and `Connection: close`. There is no overall
Content-Length or HTTP chunk encoding. Read each MIME part using its own
Content-Length; do not search binary BMP data for boundary strings. Each part
is terminated by CRLF and the next part starts with `--fpv-stream-v1\r\n`.

The first part is `application/json` with a hello object:

```json
{"api_version":1,"type":"hello","session_id":"0123456789abcdef0123456789abcdef","latest_frame_id":42,"cursor":42,"resume":"live","history_frames":8,"target_hz":0}
```

A new connection starts live, after its hello cursor. It does not replay the
web UI's historical detection cache. A matching session and `after` can replay
retained frames. `resume` is `live`, `resumed`, `session_reset` or `cursor_ahead`;
the last two start live because the requested cursor is no longer meaningful.
The history holds eight publications globally, shared across all frequencies.

Each image part looks like this (the body is binary, not the placeholder):

```http
--fpv-stream-v1
Content-Type: image/bmp
Content-Length: 415798
X-FPV-Metadata: {"api_version":1,"type":"frame","session_id":"0123456789abcdef0123456789abcdef","frame_id":43,"target_hz":5800000000,"frequency_hz":5800000000,"lo_hz":5800000000,"channel":0,"standard":"PAL","width":720,"height":576,"captured_mono_ms":123456,"epoch":7,"sync_quality":99,"simulate":false,"dropped_before":0}

<415798 BMP bytes followed by CRLF>
```

The wire uses CRLF for all shown line endings. The X-FPV-Metadata JSON and BMP
come from the same immutable publication, including when the radio retunes or
changes standard. Header names are case-insensitive. Additional JSON fields
may be introduced without changing `api_version`; incompatible changes will
use a new API version.

| Frame field | Meaning |
| --- | --- |
| `api_version` | Integer `1`. |
| `type` | `frame`. |
| `session_id` | Random identity of this `fpv-webd` process; changes on restart. |
| `frame_id` | Global monotonically increasing publication ID within this session, across all frequencies. Not an FPGA broadcast-frame counter. |
| `target_hz` | Nominal search/lock center. Use this for stable frequency grouping and exact subscription filters. |
| `frequency_hz` | Actual DDC center after NCO quantization, in Hz. This is not a precise transmitter carrier estimate. |
| `lo_hz` | AD9361 RF LO, in Hz. |
| `channel` | DDC channel, 0..3. |
| `standard` | `PAL` or `NTSC`, automatically detected. |
| `width`, `height` | 720×576 for PAL, 720×480 for NTSC. |
| `captured_mono_ms` | Device monotonic milliseconds when the validated image was published. Not UTC. |
| `epoch` | FPGA tuning epoch; unchanged across ordinary fixed-frequency frames. |
| `sync_quality` | Existing detector sync score, 0..100; not a calibrated signal probability. |
| `simulate` | `true` only for explicitly simulated host operation. |
| `dropped_before` | Number of global history publications evicted before this client's cursor advanced. Includes other frequencies even for a filtered subscription. |

Idle or filtered-out acquisition produces an `application/json` heartbeat after
about one second without a matching frame:

```json
{"api_version":1,"type":"heartbeat","session_id":"0123456789abcdef0123456789abcdef","cursor":50,"latest_frame_id":50,"dropped_before":0}
```

Cursor advances over filtered-out frequencies. Frame ID gaps on a filtered
stream are normal and do not alone imply a lost matching image. Overrun counts
are reported once, on the next frame or heartbeat. A process restart is a new
session, so IDs from different sessions cannot be compared.

There are four concurrent streaming subscriptions. A fifth gets HTTP 503;
malformed/duplicate/unknown parameters get HTTP 400. Existing web/status/control
requests keep using the normal HTTP request loop. A subscriber with no socket
send progress for two seconds is disconnected. No network send runs under the
capture/feed mutex. Frames use an eight-entry reference-counted ring; slow
clients cannot cause an unbounded SDR queue or block radio acquisition. A
client must reconnect after EOF, timeout or a truncated part; a partial image
must never be saved as a complete frame. TCP preserves bytes within a working
connection; it does not make the bounded application history lossless.

## Capture continuity

During a scan, a frequency produces images only when its RF window and DDC
phase are visited and valid video is captured. Individual frequency recordings
therefore have scan gaps. In lock mode, every successful publication is offered
immediately to subscribers, independent of the web page's polling interval.
The existing capture loop, shared DMA and its 50 ms scheduling pause remain;
this interface does not guarantee 25/30 fps or eliminate radio/DMA capture
failures. No codec or frame-rate conversion runs on the SDR.

## Offline checks

```sh
cmake -S app -B build/host -DCMAKE_BUILD_TYPE=Release
cmake --build build/host -j8
ctest --test-dir build/host --output-on-failure
python3 -m unittest discover -s tests -p test_fpv_receiver.py -v
```

Run `build/host/fpv-webd --simulate --listen 127.0.0.1 --port 8080 --web-root app/web`
and point the recorder at `http://127.0.0.1:8080`. Simulation provides PAL near
5800 MHz and NTSC near 5860 MHz. Both are synthetic and marked `simulate:true`.
The tests cover protocol validation, separate frequency recordings, actual
loopback HTTP streaming, resume/restart, filters, client limits, coexistence
with web/control requests and capture progress with stalled subscribers.
