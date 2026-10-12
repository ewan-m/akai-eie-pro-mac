#ifndef AKAI_EIE_DRIVER_CONFIG_HPP
#define AKAI_EIE_DRIVER_CONFIG_HPP

#include <stdint.h>

namespace akai_eie_driver
{

	constexpr const char *kDriverName = "AKAI EIE Pro Driver";
	constexpr const char *kDeviceName = "AKAI EIE Pro";
	constexpr const char *kDeviceUid = "com.interslice.akai-eie-pro.device";
	constexpr const char *kModelUid = "com.interslice.akai-eie-pro.model";
	constexpr const char *kManufacturerName = "Akai Professional";
	constexpr const char *kOutputStreamName = "AKAI EIE Pro Output";
	constexpr const char *kInputStreamName = "AKAI EIE Pro Input";

	constexpr uint32_t kChannelCount = 4;
	constexpr uint32_t kDefaultSampleRateHz = 48000;
	constexpr uint32_t kIOBufferFrameCount = 32768;
	constexpr uint32_t kBytesPerSample = sizeof(float);
	constexpr uint32_t kBytesPerFrame = kChannelCount * kBytesPerSample;
	constexpr uint32_t kIOBufferBytes = kIOBufferFrameCount * kBytesPerFrame;

	// Same set as the Linux reference driver. All rates use the same USB
	// alternate setting; the rate itself goes in the reset control sequence.
	constexpr double kSupportedSampleRates[] = {
		44100.0,
		48000.0,
		88200.0,
		96000.0,
	};

	// The zero-timestamp period doubles as the length of the HAL's IO ring
	// buffer, and CoreAudio derives the maximum IO buffer size a client may pick
	// from it. At one 5 ms transfer (240 frames) the HAL only offered 15-90
	// frames; this leaves room for the usual 32-4096 range at every rate.
	constexpr uint32_t kZeroTimestampPeriodFrames = 16384;
	static_assert(kZeroTimestampPeriodFrames <= kIOBufferFrameCount,
				  "stream buffers must hold a whole zero-timestamp period");

	constexpr uint8_t kUsbConfigurationValue = 1;
	constexpr uint8_t kPlaybackInterfaceNumber = 0;
	constexpr uint8_t kDataInterfaceNumber = 1;
	constexpr uint8_t kPlaybackAlternateSetting = 1;
	constexpr uint8_t kPlaybackEndpointAddress = 0x02;
	constexpr uint32_t kPlaybackIsoPacketCount = 40;
	constexpr uint32_t kPlaybackTransferCount = 8;
	constexpr uint32_t kPlaybackBytesPerUsbFrame = 12;
	constexpr uint32_t kUsbMicroframesPerBusFrame = 8;
	constexpr uint32_t kPlaybackTransferDurationBusFrames = 5;
	constexpr uint32_t kPlaybackTransferDurationMicroframes =
		kPlaybackTransferDurationBusFrames * kUsbMicroframesPerBusFrame;
	constexpr uint32_t kPlaybackScheduleLeadBusFrames = 16;
	constexpr uint32_t kInputSafetyOffsetFrames = 0;

	// Capture backlog beyond the cushion that is dropped after the HAL skips
	// cycles, so input latency does not creep up for the rest of the session.
	constexpr uint32_t kCaptureMaxExcessFrames = 2048;

	// How often the measured output latency is logged, in playback transfers
	// (2000 x 5 ms = 10 s).
	constexpr uint32_t kLatencyReportTransfers = 2000;
	constexpr uint32_t kPlaybackRingFrameCount = kIOBufferFrameCount;
	constexpr uint32_t kPlaybackRingBytes =
		kPlaybackRingFrameCount * kPlaybackBytesPerUsbFrame;
	constexpr uint32_t kCaptureTransferCount = 8;
	constexpr uint32_t kCaptureTransferBytes = 4096;
	constexpr uint32_t kCaptureBytesPerUsbFrame = 64;
	constexpr uint32_t kCaptureRingFrameCount = kIOBufferFrameCount;
	constexpr uint32_t kCaptureRingBytes =
		kCaptureRingFrameCount * kBytesPerFrame;

	// Interface 1 exposes a small isochronous IN packet containing device-clock
	// frame advances once per millisecond. Keep individual reports available to
	// the playback clock, while queuing enough reads to survive callback jitter.
	constexpr uint32_t kSyncPacketsPerTransfer = 1;
	constexpr uint32_t kSyncTransferCount = 64;
	constexpr uint32_t kSyncMaxReportsPerPlaybackPeriod = kPlaybackIsoPacketCount;
	constexpr uint16_t kSyncMaxPacketBytes = 64;
	constexpr uint32_t kSyncPrimeDurationBusFrames =
		kPlaybackScheduleLeadBusFrames +
		kPlaybackTransferDurationBusFrames * 2;
	constexpr uint32_t kSyncPrimeAttemptCount =
		kSyncPrimeDurationBusFrames;

	// Frames carried by one 5 ms playback transfer.
	constexpr uint32_t TransferFrames(uint32_t sampleRateHz)
	{
		return sampleRateHz * kPlaybackTransferDurationBusFrames / 1000;
	}

	// Audio queued in scheduled-but-unplayed USB transfers. The schedule lead
	// only positions the first transfer; after that, transfers are scheduled
	// back to back, so each resubmission lands kPlaybackTransferCount transfer
	// durations ahead of the one that just finished.
	constexpr uint32_t OutputTransportLatencyFrames(uint32_t sampleRateHz)
	{
		return sampleRateHz *
			   (kPlaybackTransferCount * kPlaybackTransferDurationBusFrames) /
			   1000;
	}

	// Playback copies each IO cycle into the FIFO as soon as the HAL writes it,
	// so the HAL never needs to stay ahead of a hardware read position. The USB
	// queue is reported once, as latency; also reporting it as safety offset
	// made clients compensate for it twice.
	constexpr uint32_t OutputSafetyOffsetFrames(uint32_t sampleRateHz)
	{
		(void)sampleRateHz;
		return 0;
	}

	// FIFO cushions between the CoreAudio IO thread and the USB completions.
	// Each side waits until this much audio beyond one IO cycle is queued before
	// draining, and re-primes after an underrun rather than playing fragments.
	// Without this, ordinary scheduling jitter shows up as clicks.
	constexpr uint32_t PlaybackPrimeFrames(uint32_t sampleRateHz)
	{
		return TransferFrames(sampleRateHz);
	}

	constexpr uint32_t CapturePrimeFrames(uint32_t sampleRateHz)
	{
		return 256 * sampleRateHz / 48000;
	}

	constexpr uint32_t OutputLatencyFrames(uint32_t sampleRateHz)
	{
		return OutputTransportLatencyFrames(sampleRateHz) +
			   PlaybackPrimeFrames(sampleRateHz);
	}

	constexpr uint32_t InputLatencyFrames(uint32_t sampleRateHz)
	{
		return CapturePrimeFrames(sampleRateHz);
	}

	inline bool IsSupportedSampleRate(double sampleRate)
	{
		for (double supportedRate : kSupportedSampleRates)
		{
			if (sampleRate == supportedRate)
				return true;
		}
		return false;
	}

} // namespace akai_eie_driver

#endif // AKAI_EIE_DRIVER_CONFIG_HPP
