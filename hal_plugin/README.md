# AKAI EIE Pro HAL Plugin

This is a raw `AudioServerPlugInDriverInterface` implementation. It does not
use libASPL.

The current target is an experimental CoreAudio HAL device with user-space USB
transport:

- device name: `AKAI EIE Pro`
- bundle ID: `com.akai.eie.hal`
- transport: USB
- output stream: 4-channel, 44.1/48/88.2/96 kHz, Float32
- input stream: 4-channel, 44.1/48/88.2/96 kHz, Float32
- mutable CoreAudio nominal sample rate and buffer frame size
- input channel names: `Input 1` through `Input 4`
- output channel names: `USB 1` through `USB 4`
- `StartIO`: opens `09e8:0010`, selects configuration/alternate settings,
  sends the AKAI reset sequence for the active sample rate, and starts USB I/O
- `StopIO`: stops the USB worker and releases the device/interfaces
- input I/O: decodes the EIE bulk capture endpoint into interleaved Float32
- output I/O: converts interleaved Float32 to AKAI 4-channel S24_3LE wire
  frames, queues them to the playback isochronous OUT endpoint through IOKit
  low-latency isoch buffers, and zero-fills only the missing tail of a short
  transfer instead of waiting for a prime gate
- playback pacing: a nominal fractional accumulator currently drives transfer
  frame counts while the pending-frame servo telemetry remains available for
  diagnosis
- playback lookahead: keeps 8 transfers available for macOS/xHCI scheduling
  runway
- playback telemetry: reports buffer servo state plus late USB scheduling,
  submit retries, completion errors, packet-size variation, payload continuity,
  CoreAudio producer rate, source level/clipping, and bad isochronous packet
  statuses
- sync feedback endpoint: detected for diagnostics but not scheduled because
  playback pacing is derived from pending playback frames
- capture bulk reads: start on demand when CoreAudio reads input, using two
  16 KiB transfers to reduce callback pressure

The original 10.14-era AKAI HAL used the old CoreAudio plug-in ABI and talked
to a Ploytec kernel service rather than owning raw USB scheduling itself. This
project keeps a modern `AudioServerPlugInDriverInterface` and mirrors the
observable behavior that maps cleanly into user space. See
`REVERSE_ENGINEERING.md` for the current notes.

## Build

```bash
make -C hal_plugin
```

The bundle is written to:

```text
hal_plugin/build/AkaiEiePro.driver
```

## Install For Local Testing

```bash
hal_plugin/install.sh
```

That command copies the bundle to `/Library/Audio/Plug-Ins/HAL/` and restarts
`coreaudiod`, so it requires `sudo`.
