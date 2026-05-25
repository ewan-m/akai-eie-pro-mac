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
constexpr uint32_t kZeroTimestampCadenceDivisor = 4;
constexpr uint32_t kIOBufferFrameCount = 32768;
constexpr uint32_t kBytesPerSample = sizeof(float);
constexpr uint32_t kBytesPerFrame = kChannelCount * kBytesPerSample;
constexpr uint32_t kIOBufferBytes = kIOBufferFrameCount * kBytesPerFrame;
constexpr uint32_t kSafetyOffsetFrames = 64;

constexpr double kSupportedSampleRates[] = {
	48000.0,
};

// Playback USB transport is brought up at the device's stable 48 kHz mode.
constexpr uint8_t kUsbConfigurationValue = 1;
constexpr uint8_t kPlaybackInterfaceNumber = 0;
constexpr uint8_t kDataInterfaceNumber = 1;
constexpr uint8_t kPlaybackAlternateSetting = 1;
constexpr uint8_t kPlaybackEndpointAddress = 0x02;
constexpr uint32_t kPlaybackIsoPacketCount = 40;
constexpr uint32_t kPlaybackTransferCount = 8;
constexpr uint32_t kPlaybackFramesPerTransfer = 240;
constexpr uint32_t kPlaybackBytesPerUsbFrame = 12;
constexpr uint32_t kPlaybackTransferBytes =
	kPlaybackFramesPerTransfer * kPlaybackBytesPerUsbFrame;
constexpr uint32_t kPlaybackTransferDurationBusFrames = 5;
constexpr uint32_t kPlaybackScheduleLeadBusFrames = 16;
constexpr uint32_t kPlaybackPrimeFrameCount =
	kPlaybackFramesPerTransfer * kPlaybackTransferCount * 2;
constexpr uint32_t kPlaybackRingFrameCount = 8192;
constexpr uint32_t kPlaybackRingBytes =
	kPlaybackRingFrameCount * kPlaybackBytesPerUsbFrame;
static_assert(kPlaybackPrimeFrameCount <= kPlaybackRingFrameCount);

constexpr uint32_t ZeroTimestampPeriod(uint32_t sampleRateHz)
{
	return sampleRateHz / kZeroTimestampCadenceDivisor;
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
