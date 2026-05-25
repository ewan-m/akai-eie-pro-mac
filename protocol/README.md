# AKAI EIE Pro Protocol Layer

This directory contains the AKAI-specific wire protocol extracted from the
Linux driver. It intentionally has no dependency on ALSA, Linux USB, CoreAudio,
DriverKit, Objective-C, heap allocation, exceptions, or STL containers.

The goal is for the same code to back either:

- a DriverKit/AudioDriverKit dext, where USB transport is implemented with
  `IOUSBHostDevice`, `IOUSBHostInterface`, and `IOUSBHostPipe`; or
- a CoreAudio HAL plugin fallback, where a separate transport layer owns USB
  access and calls into this protocol layer for control sequences and audio
  packing.

## What Is Included

- Device identity constants for `09e8:0010`.
- The reset/sample-rate control transfer sequence.
- Playback frame timing and isochronous packet layout helpers.
- Playback encoding from 4-channel interleaved Float32 into 12-byte AKAI wire
  frames.
- Playback transfer filling for already-packed 12-byte AKAI wire frames.
- Capture decoding from the device's 64-byte USB frames into 1, 2, or 4
  channels of little-endian PCM.
- Capture decoding from the device's 64-byte USB frames into 4-channel
  interleaved Float32 for CoreAudio/AudioDriverKit stream buffers.

## Platform Adapter Responsibilities

The platform-specific driver still owns:

- matching and opening the USB device;
- selecting alternate setting `1` on interfaces `0` and `1`;
- translating `ControlTransfer` records into USB control requests;
- opening endpoint pipes, including capture endpoint `0x86`;
- scheduling isochronous playback/sync transfers and bulk capture transfers;
- exposing streams, controls, timestamps, and configuration changes to
  CoreAudio or ALSA.

## Entitlements To Start Now

For the DriverKit distribution path, request these Apple entitlements early:

- `com.apple.developer.driverkit`
- `com.apple.developer.driverkit.family.audio`
- `com.apple.developer.driverkit.transport.usb` for vendor `0x09e8`, product
  `0x0010`

That request is outside this repository, but it is the long pole for a
distributable macOS driver.
