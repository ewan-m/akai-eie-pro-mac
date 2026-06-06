# AKAI EIE Pro DriverKit Backend

This is the target macOS architecture for the project: an AudioDriverKit driver
extension that owns the EIE Pro as real USB hardware and uses USBDriverKit for
the non-class-compliant transport.

Earlier user-space macOS experiments proved useful protocol details, but they
had to recreate too much of CoreAudio's device engine in a plug-in process.
The dext lets AudioDriverKit own the audio-device contract:

```text
CoreAudio HAL
  <-> AudioDriverKit IOUserAudioDriver / IOUserAudioDevice / IOUserAudioStream
  <-> AKAI EIE Pro dext code
  <-> USBDriverKit IOUSBHostDevice / IOUSBHostInterface / IOUSBHostPipe
  <-> AKAI EIE Pro 09e8:0010
```

## Code That Ports Directly

- `../protocol/akai_eie_protocol.*`
  - USB identity `09e8:0010`
  - reset/sample-rate control transfers
  - playback Float32 to AKAI 4-channel S24_3LE wire frames
  - capture 64-byte USB frame decoding
  - isochronous packet layout helpers
- Hardware observations from the earlier macOS prototype:
  - select alternate setting `1` on interfaces `0` and `1`
  - send the reset control-transfer sequence after alt-setting selection
  - use an isochronous OUT playback endpoint
  - use bulk IN capture endpoint `0x86`
  - schedule the small interface-1 isochronous IN sync endpoint and use its
    validated device-frame advances to pace playback packets

## Code That Should Not Port

- Legacy CoreAudio user-space object/property plumbing.
- User-space fake zero timestamp calculation.
- IOKit userspace `IOUSBLib` and low-latency isoch APIs.
- Ozzy's kext/user-client shared-memory boundary.

AudioDriverKit replaces those pieces with stream memory descriptors,
configuration-change callbacks, start/stop lifecycle calls, zero timestamp
state, and real-time `IOOperationHandler` callbacks.

## First Dext Shape

1. `AkaiEieDriver : IOUserAudioDriver`
   - match an `IOUSBHostDevice` provider for vendor `0x09e8`, product `0x0010`
   - open the USB device
   - select USB configuration `1` so the playback interface is enumerated
   - create one `AkaiEieDevice`
   - add the device with `AddObject`
   - register the service

2. `AkaiEieDevice : IOUserAudioDevice`
   - expose one 4-channel output `IOUserAudioStream`
   - expose one 4-channel input `IOUserAudioStream`
   - use interleaved Float32 stream buffers initially
   - advertise `48000` Hz while the initial playback transport is active
   - set transport type to `USB`
   - use a zero timestamp period derived from the sample rate

3. `IOOperationHandler`
   - on `IOUserAudioIOOperationWriteEnd`, read the host output buffer for the
     provided sample-time range wrapped by the device zero-timestamp period,
     encode with
     `akai_eie::EncodePlaybackFrameFromFloat32`, and stage USB playback frames
   - on `IOUserAudioIOOperationBeginRead`, return silence until capture
     `AsyncIO` is implemented
   - keep this callback allocation-free, lock-free if practical, and free of
     blocking USB calls

4. USB transport inside the dext
   - translate `akai_eie::ControlTransfer` into DriverKit device requests
   - use `IOUSBHostInterface::CopyPipe` for playback and capture pipes
   - use `IOUSBHostPipe::IsochIO` for playback
   - use `IOUSBHostPipe::AsyncIO` for capture endpoint `0x86`
   - require the interface-1 crystal-clock feedback endpoint before starting
     playback
   - update `IOUserAudioClockDevice::UpdateCurrentZeroTimestamp` only from
     validated feedback-frame counts and their USB completion timestamps

## Entitlements

Request these together for the DriverKit extension:

- `com.apple.developer.driverkit`
- `com.apple.developer.driverkit.family.audio`
- `com.apple.developer.driverkit.transport.usb`
  - `idVendor = 0x09e8`
  - `idProduct = 0x0010`

The host app also needs the System Extension entitlement so it can activate the
driver extension.

This directory includes starter templates for the dext target:

- `Info.plist.template`
- `AkaiEieDriver.entitlements.template`

## Local Skeleton Check

The source uses DriverKit `.iig` headers. Xcode normally generates the matching
headers and glue source for the dext target, but the local check does the same
generation into `/private/tmp` and compiles the driver objects:

```bash
make -C driverkit check
```

This does not install or activate the dext. It verifies that the current
DriverKit driver builds against the local SDK.

To build the local activator app with the dext embedded:

```bash
make -C driverkit app
```

The app bundle is written to:

```text
driverkit/build/local/AkaiEieActivator.app
```

## Local Install / Activation

This local app is ad-hoc signed for development. It is not a polished,
notarized installer, so macOS security policy has to allow local DriverKit
system-extension development.

Ozzy's kext README uses `csrutil enable --without kext` in Recovery Mode
because that backend is a kernel extension. This project is a DriverKit system
extension, so use the System Extensions development path instead:

1. On Apple Silicon, boot Recovery, open Startup Security Utility, and use
   Reduced Security. If macOS offers an option for user-managed system
   extensions, enable it.
2. Boot back into macOS and enable System Extensions developer mode:

   ```bash
   systemextensionsctl developer on
   ```

3. Drag `driverkit/build/local/AkaiEieActivator.app` into `/Applications`, or
   copy and launch it from Terminal:

   ```bash
   ditto driverkit/build/local/AkaiEieActivator.app /Applications/AkaiEieActivator.app
   open /Applications/AkaiEieActivator.app
   ```

4. Click `Activate Driver`.
5. When macOS blocks the new system extension, open System Settings -> Privacy
   & Security and approve/enable the AKAI EIE Pro driver. Restart if macOS
   asks.

The containing app should normally run from `/Applications`. For quick local
build-path iteration, System Extensions developer mode also allows running the
same app from `driverkit/build/local`:

```bash
open driverkit/build/local/AkaiEieActivator.app
```

Press `Activate Driver` only for the initial installation. If a driver is
already installed, that button refuses an in-place replacement and directs you
to `Reload Driver`. For a subsequent local build, stop audio playback and press
`Reload Driver`; it submits a deactivation request first, then activates the
embedded replacement after the previous dext has released the USB device.
Approve the macOS authorization prompt if deactivation requires it.
`Deactivate Driver` removes the active driver without installing another
build. If a request fails, the app shows the `NSError` and launch-location
guidance; fix activation before DAW testing.

## Audio Test

The current transport exposes one 4-channel output stream and one 4-channel
input stream at `48000` Hz. It configures the USB device itself, claims the
output interface, claims the capture interface, and uses interface 1 for
device-clock feedback. Keep speaker/headphone levels low while testing local
builds.

After activation, open Audio MIDI Setup and confirm the AKAI EIE Pro appears as
a 4-in/4-out USB device at `48000` Hz. Then test playback and microphone input
in the DAW with the device set to 48 kHz. For normal `Reload Driver`
iterations, keep the interface connected; physically reconnect it only once if
a previously crashed/replaced dext has left macOS with a stale attachment.

## Immediate Build Order

1. Activate the local dext and confirm the 4-in/4-out AudioDriverKit device
   appears in Audio MIDI Setup.
2. Validate continuous playback and capture at `48000` Hz in DAWs.
3. Add sample-rate configuration changes.
4. Measure and tune latency/buffer-size reporting.

## Apple References

- [Creating an audio device driver](https://developer.apple.com/documentation/audiodriverkit/creating-an-audio-device-driver)
- [IOUserAudioDevice](https://developer.apple.com/documentation/audiodriverkit/iouseraudiodevice)
- [IOUserAudioStream](https://developer.apple.com/documentation/audiodriverkit/iouseraudiostream)
- [IOUSBHostPipe::IsochIO](https://developer.apple.com/documentation/usbdriverkit/iousbhostpipe/isochio)
- [Requesting Entitlements for DriverKit Development](https://developer.apple.com/documentation/driverkit/requesting-entitlements-for-driverkit-development)
