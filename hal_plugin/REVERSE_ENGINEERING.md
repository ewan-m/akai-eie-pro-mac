# AKAI EIE Original HAL Notes

These notes came from the macOS 10.14-era bundle at:

```text
/tmp/akai-extract/hal/AKAI_EIE.plugin/Contents/MacOS/AKAI_EIE
```

## Bundle And ABI

- Original bundle ID: `com.akai.eie.hal`
- Original executable: `AKAI_EIE`
- Original version: `3.3.17`
- Original factory: `CreateAudio`
- Original CoreAudio type UUID: `F8BB1C28-BAE8-11D6-9C31-00039315CD46`

That type UUID is the older `AudioHardwarePlugInInterface` family, not the
modern `AudioServerPlugInDriverInterface` used by this project. Recreating the
old ABI would also require the original kernel side, so this project keeps the
modern HAL ABI while aligning bundle identity and observable properties.

## Driver Shape

The original binary exports and strings point to a Ploytec-derived stack:

- Core classes include `PGAsioDriver`, `PGOSXDevice`, `OSXDeviceManager`,
  `DriverConnection`, and `AudioSM`.
- It looks for an IOKit service named `PGKernelDeviceDrvAkaiEIE`.
- It uses user-client calls such as `IOServiceOpen`, `IOConnectCallStructMethod`,
  `IOConnectCallAsyncScalarMethod`, and `IOConnectMapMemory`.
- It also links IOKit and CoreAudio, but the HAL side delegates streaming to the
  kernel/user-client boundary instead of doing all USB transfers directly.

The current code cannot copy that boundary without the kext. The closest useful
alignment is to keep a standalone user-space USB transport, while matching the
same rate/property behavior and AKAI wire protocol.

## Observable Behavior Worth Mirroring

Strings in the original HAL show these behaviors:

- Settable `kAudioDevicePropertyNominalSampleRate`
- Settable `kAudioDevicePropertyBufferFrameSize`
- Preferences or settings keys including `ClockSource`, `USB1Mode`,
  `SampleRate`, `MixerBuffer`, `ReverbBuffers`, and routing fields
- Channel and stream names such as `Input`, `Output`, `Input L`, `Input R`,
  `USB 1`, `USB 2`, `USB 3`, and `USB 4`
- Clock source labels including `Analog` and `Analog - Sync Digital`

Implemented alignment in this project:

- Uses `com.akai.eie.hal` as the HAL bundle identity.
- Reports and accepts 44.1, 48, 88.2, and 96 kHz, matching the reset protocol
  already implemented in `protocol/`.
- Restarts the USB transport when the HAL nominal sample rate changes.
- Exposes a mutable buffer frame size property.
- Exposes CoreAudio element names for input and USB output channels.
- Starts playback isochronous output immediately and starts bulk capture on
  demand when CoreAudio reads input.
- Copies whatever playback frames are available and zero-fills only the missing
  tail on underrun, matching the Linux/Ploytec-style no-prime behavior.
- Uses filtered output-ring fill level as the playback clock servo error signal,
  with a deadband and fractional accumulator for sub-frame rate corrections.
- Limits playback isochronous lookahead to 8 transfers so future transfers are
  less likely to be filled with underrun padding before CoreAudio refills.
- Does not schedule the empty sync-feedback endpoint; the pipe is still detected
  and logged for diagnostics.
- Starts two larger capture bulk reads on demand when CoreAudio reads input,
  avoiding playback-only bulk traffic.

Not yet implemented:

- Mixer/routing/custom AKAI properties.
- S/PDIF or clock source switching.
- Old Ploytec shared-memory/user-client behavior.
