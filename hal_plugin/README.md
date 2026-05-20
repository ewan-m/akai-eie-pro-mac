# AKAI EIE Pro HAL Plugin

This is a raw `AudioServerPlugInDriverInterface` implementation. It does not
use libASPL.

The current target is a minimal silent CoreAudio device that loads without
USB transport:

- device name: `AKAI EIE Pro`
- bundle ID: `com.interslice.akai-eie-pro`
- transport: USB
- output stream: 4-channel, 48 kHz, Float32
- input stream: 4-channel, 48 kHz, Float32
- `StartIO` and `StopIO`: log and succeed
- input I/O: returns silence
- output I/O: accepts and discards samples

The `protocol/` module is linked into the bundle but is not called from the
I/O path yet. The next layer should add a transport object that feeds real USB
capture data into the input stream and consumes output mix data for playback.

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
