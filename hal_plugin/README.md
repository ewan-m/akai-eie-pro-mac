# AKAI EIE Pro HAL Plugin

This is a raw `AudioServerPlugInDriverInterface` implementation. It does not
use libASPL.

The current target is an experimental CoreAudio HAL device with playback USB
transport:

- device name: `AKAI EIE Pro`
- bundle ID: `com.interslice.akai-eie-pro`
- transport: USB
- output stream: 4-channel, 48 kHz, Float32
- input stream: 4-channel, 48 kHz, Float32
- `StartIO`: opens `09e8:0010`, selects configuration/alternate settings,
  sends the AKAI reset sequence, and starts isochronous playback writes
- `StopIO`: stops the USB worker and releases the device/interfaces
- input I/O: returns silence
- output I/O: converts interleaved Float32 to AKAI 4-channel S24_3LE wire
  frames and queues them to the playback isochronous OUT endpoint

Capture is not wired yet. The data interface is opened and the sync/capture
pipes are detected for logging, but the HAL input stream still zero-fills until
the bulk capture reader is added.

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
