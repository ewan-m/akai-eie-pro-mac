# AKAI EIE Pro DriverKit Backend

This is the target macOS architecture for the project: an AudioDriverKit driver
extension that owns the EIE Pro as real USB hardware and uses USBDriverKit for
the non-class-compliant transport.

The current HAL plugin proved the AKAI protocol pieces, but it also has to
recreate too much of CoreAudio's device engine in a plug-in process. The dext
should instead let AudioDriverKit own the audio-device contract:

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
- The HAL USB transport's hardware observations:
  - select alternate setting `1` on interfaces `0` and `1`
  - send the reset control-transfer sequence after alt-setting selection
  - use an isochronous OUT playback endpoint
  - use bulk IN capture endpoint `0x86`
  - ignore the empty/suspicious sync endpoint until hardware evidence says
    otherwise

## Code That Should Not Port

- `AudioServerPlugInDriverInterface` object/property plumbing.
- HAL-side fake zero timestamp calculation.
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
     provided sample-time range, encode with
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
   - update `IOUserAudioClockDevice::UpdateCurrentZeroTimestamp` from USB
     completion timestamps and submitted hardware frame counts

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

System Extensions normally require the containing app to run from
`/Applications`. For that path:

```bash
ditto driverkit/build/local/AkaiEieActivator.app /Applications/AkaiEieActivator.app
open /Applications/AkaiEieActivator.app
```

For local build-path iteration after disabling SIP, enable System Extensions
developer mode once and run the app from `driverkit/build/local`:

```bash
systemextensionsctl developer on
open driverkit/build/local/AkaiEieActivator.app
```

Press `Activate Driver` only for the initial installation. If a driver is
already installed, that button refuses an in-place replacement and directs you
to `Reload Driver`. For a subsequent local build, stop audio playback and press
`Reload Driver`; it submits a deactivation request first, then activates the
embedded replacement after the previous dext has released the USB device.
Approve the macOS authorization prompt if deactivation requires it.
`Deactivate Driver` removes the active driver without installing another
build. If a request fails, the app log shows the `NSError` and
launch-location guidance; `sysextd` logs provide the next validation failure.

## Playback Test

The current transport is playback-only at `48000` Hz, configures the USB
device itself, and claims only the output USB interface. Keep the interface
connected for normal `Reload Driver` iterations; physically reconnect it only
once if a previously crashed/replaced dext has left macOS with a stale
attachment.

```bash
log stream --style compact --predicate 'eventMessage CONTAINS[c] "AkaiEie" OR process == "coreaudiod"'
```

Starting audio should produce:

```text
AkaiEieDriver::CaptureUsbProvider matched AKAI EIE Pro 09e8:0010 configuration 1
AkaiEieDriver::StartPlayback ready 48 kHz endpoint 0x02
AkaiEieDevice::StartIO
AkaiEieDriver::StartPlayback started with 3840 primed frames
```

If USB setup or transfer submission fails, `StartDevice StartPlayback failed`
or `PlaybackComplete resubmit failed` identifies the next transport issue.
Stopping playback logs ring/USB counters followed by
`AkaiEieDriver::StopPlayback drained`. Input is intentionally silent until
capture is implemented.

## Immediate Build Order

1. Activate the local dext and confirm the 4-in/4-out AudioDriverKit device
   appears in Audio MIDI Setup.
2. Test the playback-only `IsochIO` path at `48000` Hz.
3. Add capture `AsyncIO`.
4. Add sample-rate configuration changes.

## Apple References

- [Creating an audio device driver](https://developer.apple.com/documentation/audiodriverkit/creating-an-audio-device-driver)
- [IOUserAudioDevice](https://developer.apple.com/documentation/audiodriverkit/iouseraudiodevice)
- [IOUserAudioStream](https://developer.apple.com/documentation/audiodriverkit/iouseraudiostream)
- [IOUSBHostPipe::IsochIO](https://developer.apple.com/documentation/usbdriverkit/iousbhostpipe/isochio)
- [Requesting Entitlements for DriverKit Development](https://developer.apple.com/documentation/driverkit/requesting-entitlements-for-driverkit-development)
