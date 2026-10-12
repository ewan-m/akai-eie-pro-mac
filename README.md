# AKAI EIE Pro Driver for Apple Silicon Macs

Free, open-source macOS driver work for the AKAI EIE Pro USB audio interface
(`09e8:0010`). The goal is to keep this otherwise useful interface working on
modern Apple Silicon Macs after official driver support stopped.

This is hardware-driver work in progress. Keep speaker and headphone levels low
while testing, and expect rough edges.

## Current Status

- **DriverKit system extension**: the active implementation is in `driverkit/`
  and uses AudioDriverKit plus USBDriverKit.
- **Playback/capture**: initial 4-out/4-in DriverKit path is working at
  48 kHz in local Apple Silicon testing. 44.1, 88.2 and 96 kHz are exposed
  and selectable from Audio MIDI Setup or a DAW but still need hardware
  testing.
- **Buffer size**: clients can pick the IO buffer size (CoreAudio's usual
  range) from Audio MIDI Setup or their DAW.
- **Still to do**: latency tuning, signing, entitlement approval, packaging,
  and broader macOS/DAW testing.
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

## Local DriverKit Install

This is a local development install, not a signed/notarized release. macOS may
reject the ad-hoc signed DriverKit extension on the default security policy.

1. Build the activator app:

   ```bash
   make -C driverkit app
   ```

2. Enable local DriverKit/system-extension development:

   - On Apple Silicon, boot Recovery, open Startup Security Utility, and use
     Reduced Security. If macOS offers an option for user-managed system
     extensions, enable it.
   - Boot back into macOS and enable System Extensions developer mode:

     ```bash
     systemextensionsctl developer on
     ```

   This project is a DriverKit system extension, so the
   important part is allowing local system-extension development builds.

3. Drag `driverkit/build/local/AkaiEieActivator.app` into `/Applications`, or
   copy and launch it from Terminal:

   ```bash
   ditto driverkit/build/local/AkaiEieActivator.app /Applications/AkaiEieActivator.app
   open /Applications/AkaiEieActivator.app
   ```

4. Click `Activate Driver`.
5. Open System Settings -> Privacy & Security and approve/enable the AKAI EIE
   Pro system extension when macOS asks. Restart if macOS requires it.
6. For later local builds, quit audio apps, rebuild, reopen
   `/Applications/AkaiEieActivator.app`, and click `Reload Driver`.

See `driverkit/README.md` for lower-level activation notes and Apple
entitlement requirements.

## Known Limitations

- Capture is new and still needs broader DAW validation.
- Audio is currently focused on 48 kHz validation.
- Distribution needs the appropriate Apple DriverKit entitlements and signing
  setup.
- This is not a polished end-user installer yet.

## License

Free software under the GNU General Public License v2.0. See `LICENSE`.

Not affiliated with Akai Professional or inMusic.
