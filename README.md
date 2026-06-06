# AKAI EIE Pro Driver for Apple Silicon Macs

Free, open-source macOS driver work for the AKAI EIE Pro USB audio interface
(`09e8:0010`). The goal is to keep this otherwise useful interface working on
modern Apple Silicon Macs after official driver support stopped.

This is hardware-driver work in progress. Keep speaker and headphone levels low
while testing, and expect rough edges.

## Current Status

- **DriverKit system extension**: the active implementation is in `driverkit/`
  and uses AudioDriverKit plus USBDriverKit.
- **Playback**: working at 48 kHz in local Apple Silicon testing.
- **Still to do**: capture, sample-rate switching, latency tuning, signing,
  entitlement approval, packaging, and broader macOS testing.
- **Shared protocol layer**: `protocol/` contains the AKAI-specific USB control
  sequence, packet sizing, playback packing, and capture decoding helpers used
  by the DriverKit code.

## Project Layout

```text
driverkit/  macOS AudioDriverKit / USBDriverKit implementation
protocol/   portable AKAI EIE Pro USB protocol helpers
index.html  simple public project page for GitHub Pages
```

## Local DriverKit Checks

The macOS path is an AudioDriverKit system extension with a small activator app.
It lets AudioDriverKit own the CoreAudio device contract while USBDriverKit
talks to the non-class-compliant EIE Pro hardware.

Useful checks:

```bash
make -C driverkit check
make -C driverkit app
```

The local app bundle is written to:

```text
driverkit/build/local/AkaiEieActivator.app
```

See `driverkit/README.md` for local activation notes, playback-test behavior,
logging commands, and Apple entitlement requirements.

## Known Limitations

- Capture is not implemented yet.
- Playback is currently focused on 48 kHz validation.
- Distribution needs the appropriate Apple DriverKit entitlements and signing
  setup.
- This is not a polished end-user installer yet.

## License

Free software under the GNU General Public License v2.0. See `LICENSE`.

Not affiliated with Akai Professional or inMusic.
