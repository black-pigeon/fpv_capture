# U220 analog FPV test source

This host tool generates a grayscale composite-video FM test card and streams
raw CS16 IQ through UHD to a USB U220. It can also replay existing DDM-P1 IQ.
No E200 firmware changes are required.

The generator is adapted from the DDM-P1
`plutosdr-fw/fpv_signal_test/generate_pal.py` reference. The reference
`e100_fpv_tx/e100_replay_iq_test.cpp` establishes the raw CS16 replay method;
its E100 IQTAXI registers, network upload and amplifier controls are specific
to E100. This implementation uses a new C++ UHD streaming transport for U220.
All required tool sources are in this directory; the reference trees are not
build dependencies and were not modified.

The connected device was enumerated on 2026-11-18 as UHD `type=b200`,
`serial=U220200`, `name=u220v2`, product B210 over USB 3. Its reported TX
range is 50 MHz–6 GHz. E200's software tuning range starts at 70 MHz.
DDM-P1's 7 GHz E100 command cannot be used with this U220.

## Desktop GUI

Launch from a desktop session (requires Python Tkinter; on Debian/Ubuntu the
package is `python3-tk`):

```sh
python3 tools/u220_fpv_tx/gui.py
```

The English window provides frequency in MHz (or a comma-separated list),
PAL/NTSC, TX attenuation, seconds per frequency, UHD channel and device
arguments. It shows a source-card preview and live subprocess logs.
`Check offline` prepares missing/stale build and waveform files, then checks
IQ and metadata without opening an SDR. `Start TX` performs the same preparation
and starts the bounded transmission; opening the window never starts TX.
Before TX, both PAL and NTSC clips are prepared, hash-checked and loaded so a
standard change does not need disk access during streaming.

During TX, edit frequency/list, PAL/NTSC, attenuation or seconds per frequency,
then click **Apply changes**. The same sender process and UHD device remain open.
The **Active** line changes only after the sender confirms device readback.
Only one update may be pending. Invalid settings leave the active settings intact.
Changing device arguments or the UHD TX channel requires stopping TX.

An attenuation-only update writes the gain without restarting the waveform or
resetting the dwell timer. A frequency/list, standard or duration change drains
the current burst, applies the settings and starts a new burst from the beginning
of the selected clip. This creates a brief gap and restarts the duration at the
first frequency in the list. Transmission still ends after the configured list
and duration; Apply changes does not enable infinite transmission.

After upgrading this GUI, close and reopen its window once to load the new code.
`Stop TX`, window close and terminal Ctrl-C request
an end-of-burst; a hung child process is terminated after a bounded grace period.

Attenuation is 0..89.75 dB in 0.25 dB steps: a larger value means a weaker
signal. The GUI starts at 89.75 dB, corresponding to the minimum UHD gain
on this AD9361 path. The sender computes the requested gain using the actual
UHD TX gain range and prints the gain/attenuation readback. This controls the
radio's relative TX level; it is not a calibrated dBm output or an external
attenuator value. `--gain` and `--attenuation` are mutually exclusive in the CLI.

For antenna-connected tests, set the desired frequency and start with maximum
attenuation, reducing attenuation as needed while observing the E200. Verify the
physical TX port against the selected channel. Opening the window leaves TX idle. During the subsequent underflow fix,
short hardware checks used maximum attenuation; recovery was tested by
intentionally pausing the IQ producer for 50 ms.

GUI model/process tests run without a display:

```sh
python3 -m unittest discover -s tests -p 'test_u220_fpv*.py' -v
```

Optional Tk widget smoke tests use a dedicated X display supplied through
`FPV_GUI_TEST_DISPLAY=:N`; they exercise only offline jobs and dummy subprocesses.
The ordinary test suite skips these display-dependent checks.

## Runtime control protocol

The GUI starts the sender with `--live-control`, `--alternate-input FILE` and
`--alternate-metadata FILE`, plus the initial waveform and attenuation settings.
Both metadata files must describe opposite PAL/NTSC standards at the same sample
rate. Offline mode validates these inputs without constructing a UHD device.
Live mode holds approximately 245.9 MB of default IQ data in host memory.

The sender reads one JSON object per line on stdin. Example:

```json
{"id":1,"frequencies_hz":[5800000000],"standard":"ntsc","attenuation_db":89.75,"dwell_s":30}
```

IDs must be positive and increase. All five fields are required; device/channel
changes are not accepted. The input reader is bounded to 8192 bytes per line
and 16 queued requests. All UHD calls run in the sending thread.
Replies start with `CONTROL ` followed by a JSON object. Status is `ready`
(initial ID 0), `applied`, `rejected`, or `center` (next timed frequency, ID 0).
Replies contain the active standard, attenuation, duration, frequency list and
actual current frequency. Numeric reply fields may be JSON strings; consumers
should convert them to numbers. Rejections include `error`. Closing stdin stops
live transmission with an end-of-burst; ordinary CLI operation does not use stdin.

## Build and generate

Host dependencies: C++17 compiler, pkg-config, UHD development library,
Boost headers, OpenSSL development library, Python 3, NumPy and Pillow.
The installed C++ UHD API is used directly; UHD's Python binding is not needed.
Generated IQ and executables belong under the ignored repository `build/` tree.
Run from the repository root:

```sh
bash tools/u220_fpv_tx/build.sh
python3 tools/u220_fpv_tx/generate_fpv.py --standard pal --output-dir build/u220-fpv-source/pal
python3 tools/u220_fpv_tx/generate_fpv.py --standard ntsc --output-dir build/u220-fpv-source/ntsc
python3 -m unittest discover -s tests -p test_u220_fpv_source.py -v
```

Each output directory contains `test_card.png`, the `.cs16` file and
`signal.json` with the IQ hash, timing and FM parameters. Default PAL is
25 frames / 1 s / 122,880,000 bytes. Default NTSC is 30 frames / 1.001 s /
123,002,880 bytes. `--frames 2` creates a smaller loop for a quick check.
In ordinary CLI mode, one complete clip is held in the sender's memory and looped continuously;
file boundaries do not create bursts or USB pauses.

| Parameter | PAL | NTSC |
|---|---|---|
| Lines per frame | 625 | 525 |
| Active grayscale image | 720 × 576 | 720 × 480 |
| Frame rate | 25 Hz | 30000/1001 Hz |
| Field rate | 50 Hz | 60000/1001 Hz |
| Line rate | 15625 Hz | 15750000/1001 Hz |
| Equalizing/broad/equalizing pulses per field | 5 / 5 / 5 | 6 / 6 / 6 |
| IQ sample rate | 30.72 MSPS | 30.72 MSPS |

Both waveforms include the half-line offset of the second field and use the
reference 3 MHz video LPF. Default FM sensitivity is 8 MHz/V: the nominal
sync-tip to white span is 8 MHz (±4 MHz around the video midpoint). Filter
overshoot increases the instantaneous deviation; FM sidebands extend beyond
that deviation. These numbers are not an occupied-bandwidth measurement.
IQ magnitude defaults to 0.5 full scale; it is not calibrated RF power.

`--polarity -1` produces an inverted frequency-deviation version.
`--sensitivity 6000000` changes the FM span. These are monochrome timing test
sources, with no color burst, audio or transmitter-specific pre-emphasis;
they are not complete models of a commercial FPV transmitter.

## Offline validation

```sh
build/u220-fpv-source/u220_fpv_tx \
  --input build/u220-fpv-source/pal/pal_fpv_30m72.cs16 \
  --metadata build/u220-fpv-source/pal/signal.json \
  --freqs 5740000000,5800000000,5860000000 --dwell 15 --dry-run
```

The default mode is also offline. It validates file size, metadata SHA256 and
sample rate, and prints the frequency plan without creating a UHD device.
Metadata fixes the playback sample rate; a conflicting `--sample-rate` is an
error. The tests also verify field timing, active row parity, sync pulses,
quantized-IQ FM recovery and phase continuity at the loop boundary. They
recover a frame using known timing to check the grayscale content. They do
not exercise the E200 decoder, channel filter or the RF link.

## Conducted RF test

Use the confirmed U220 TX/RX port through a suitable external attenuator to
the E200 RX port. Confirm output power and receiver input limits for the actual
connection before transmitting. UHD gain is not E100's `--tx-atten`, and the
U220 does not use E100's `--amp` or `--packet-gap-us` settings. The default
UHD gain is 0 dB; that does not specify output power in dBm.

Probe reported channel 0 as `FE-TX2` and channel 1 as `FE-TX1`. Verify the
physical connector mapping rather than inferring TX1 from channel 0. The tool
prints the selected frontend, antenna and actual rate/gain/bandwidth on RF start.

After connecting the conducted test path, adding `--transmit` starts a bounded
sequence. This command is a test recipe. Short maximum-attenuation hardware checks
have verified streaming and underflow recovery; they do not qualify E200
detection or image quality:

```sh
build/u220-fpv-source/u220_fpv_tx \
  --args type=b200,serial=U220200 --channel 0 \
  --input build/u220-fpv-source/pal/pal_fpv_30m72.cs16 \
  --metadata build/u220-fpv-source/pal/signal.json \
  --freqs 5740000000,5800000000,5860000000 \
  --dwell 15 --settle 0.2 --gain 0 --bandwidth 16000000 --transmit
```

In the E200 web page, clear stale results and scan 5700–5900 MHz in **Fine**
mode with 150 ms observation. Check each transmitted frequency separately:
PAL/NTSC classification, a new validated image containing the U220 test card,
and the capture timestamp. A result frequency is a DDC center, so it may be
offset from the known source LO, and neighboring centers may report the same
source. Adjust dwell to cover at least one complete receiver sweep; 15 s is a
starting point, not a guaranteed bound for arbitrary scan settings. Repeat with
the NTSC file and metadata to verify automatic standard changes. Stop TX and
clear the web history for a separate no-source false-positive check.

The sender fixes its master clock at 61.44 MHz and requires the actual TX sample
rate to match the file. It waits for minimum settling and, where available, LO
lock. It sends one continuous burst per frequency, handles short writes, checks
asynchronous errors, and waits for a burst acknowledgment before retuning.
The default USB TX queue is increased to 256 frames unless `num_send_frames`
is explicitly supplied in `--args`. B2xx's 16-frame default at its usual
8176-byte transfer size provides about 1 ms of SC16 queue headroom at
30.72 MSPS; 256 frames provide about 17 ms. This reduces sensitivity to host
scheduling jitter without changing the IQ rate. The effective frame count is
printed in `PLAN`.

Ordinary `UNDERFLOW` and `UNDERFLOW_IN_PACKET` events are counted and the
sender continues. The DSP uses `underflow_policy=next_packet` to resume RF
when the next IQ packet arrives. Repeated warning logs are throttled at counts
1, 2, 4, 8, ... so logging does not starve the producer. Each `RESULT` line
reports accepted samples, recoverable `underflows`, fatal `async_errors`,
`continuous` and end-of-burst acknowledgment. A recovered run may return zero
with `underflows>0` and `continuous=0`: it completed the requested samples but
contained signal gaps and must not be treated as an uninterrupted RF test.
Receiver video lock can be disturbed by those gaps.

Seconds per frequency sets nominal IQ sample duration. Time spent recovering
from underflow can extend wall-clock duration. USB/device failures, send
no-progress/timeout, fatal asynchronous events, missing final acknowledgment,
rate/frequency coercion or interruption still return nonzero. Ctrl-C or
`Stop TX` requests an end-of-burst; already queued samples may still drain.

The two TX channels share AD9361 RF resources. This tool sends on one selected
channel and changes RF centers sequentially; it does not create several
independent RF LOs.

## Existing DDM-P1 recordings

For an existing headerless little-endian signed 16-bit interleaved IQ file,
use the file's actual rate explicitly, without this generator's metadata:

```sh
build/u220-fpv-source/u220_fpv_tx \
  --input /absolute/path/to/pal_fpv_30m72.cs16 --sample-rate 30720000 \
  --freqs 5800000000 --dwell 15 --dry-run
```

The DDM-P1 PAL file is 30.72 MSPS; the referenced `fpv2_61p44M.cs16` recording
is 61.44 MSPS. Replaying at a different rate changes video timing and can
invalidate PAL/NTSC detection. Raw files without metadata cannot have their
rate inferred or authenticated by this tool. Files must be nonempty, consist
of complete 4-byte complex samples and fit the 512 MiB host replay limit.
