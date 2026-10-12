#include <AudioDriverKit/AudioDriverKit.h>
#include <DriverKit/DriverKit.h>
#include <string.h>

#include "AkaiEieDevice.h"
#include "AkaiEieDriver.h"
#include "AkaiEieDriverConfig.hpp"

namespace
{

	IOUserAudioStreamBasicDescription MakeFloat32StreamFormat(double sampleRate)
	{
		return {
			sampleRate,
			IOUserAudioFormatID::LinearPCM,
			static_cast<IOUserAudioFormatFlags>(
				IOUserAudioFormatFlags::FormatFlagsNativeFloatPacked),
			akai_eie_driver::kBytesPerFrame,
			1,
			akai_eie_driver::kBytesPerFrame,
			akai_eie_driver::kChannelCount,
			32,
			0};
	}

	template <typename T, size_t N>
	constexpr uint32_t CountOf(const T (&)[N])
	{
		return static_cast<uint32_t>(N);
	}

} // namespace

bool AkaiEieDevice::init(IOUserAudioDriver *driver,
						 bool supportsPrewarming,
						 OSString *deviceUid,
						 OSString *modelUid,
						 OSString *manufacturerUid,
						 uint32_t zeroTimestampPeriod)
{
	if (!super::init(driver,
					 supportsPrewarming,
					 deviceUid,
					 modelUid,
					 manufacturerUid,
					 zeroTimestampPeriod))
	{
		return false;
	}

	ivars = IONewZero(AkaiEieDevice_IVars, 1);
	if (ivars == nullptr)
		return false;
	ivars->zeroTimestampPeriod = zeroTimestampPeriod;

	kern_return_t ret = SetTransportType(IOUserAudioTransportType::USB);
	if (ret != kIOReturnSuccess)
		return false;

	ret = SetAvailableSampleRates(
		akai_eie_driver::kSupportedSampleRates,
		CountOf(akai_eie_driver::kSupportedSampleRates));
	if (ret != kIOReturnSuccess)
		return false;

	ret = SetSampleRate(akai_eie_driver::kDefaultSampleRateHz);
	if (ret != kIOReturnSuccess)
		return false;

	(void)SetInputSafetyOffset(akai_eie_driver::kInputSafetyOffsetFrames);
	ApplyRateLatencies(akai_eie_driver::kDefaultSampleRateHz);
	(void)SetPreferredChannelsForStereo(1, 2);

	// Inputs are four independent mic/line channels.
	IOUserAudioChannelLabel inputLabels[akai_eie_driver::kChannelCount] = {
		IOUserAudioChannelLabel::Discrete_0,
		IOUserAudioChannelLabel::Discrete_1,
		IOUserAudioChannelLabel::Discrete_2,
		IOUserAudioChannelLabel::Discrete_3,
	};
	// Outputs 1-2 are the main stereo pair. Labelling them Left/Right lets
	// CoreAudio spread mono sources across both sides; with all-discrete labels
	// it has no stereo pair to map onto and sends mono to output 1 only.
	IOUserAudioChannelLabel outputLabels[akai_eie_driver::kChannelCount] = {
		IOUserAudioChannelLabel::Left,
		IOUserAudioChannelLabel::Right,
		IOUserAudioChannelLabel::Discrete_2,
		IOUserAudioChannelLabel::Discrete_3,
	};
	(void)SetPreferredInputChannelLayout(inputLabels, akai_eie_driver::kChannelCount);
	(void)SetPreferredOutputChannelLayout(outputLabels, akai_eie_driver::kChannelCount);

	ret = ConfigureStreams(driver);
	if (ret != kIOReturnSuccess)
		return false;

	IOOperationHandler ioOperation = ^kern_return_t(
		IOUserAudioObjectID device,
		IOUserAudioIOOperation operation,
		uint32_t ioBufferFrameSize,
		uint64_t sampleTime,
		uint64_t hostTime) {
	  (void)device;
	  (void)hostTime;

	  if (ivars == nullptr || !ivars->ioStarted)
		  return kIOReturnSuccess;
	  if (ivars->transportDriver != nullptr)
		  ivars->transportDriver->NoteIOCycleFrames(ioBufferFrameSize);

	  if (operation == IOUserAudioIOOperationBeginRead)
		  return ReadInputFrames(sampleTime, ioBufferFrameSize);
	  else if (operation == IOUserAudioIOOperationWriteEnd)
		  return QueueOutputFrames(sampleTime, ioBufferFrameSize);

	  return kIOReturnSuccess;
	};

	ret = SetIOOperationHandler(ioOperation);
	if (ret != kIOReturnSuccess)
		return false;

	return true;
}

void AkaiEieDevice::ApplyRateLatencies(uint32_t sampleRateHz)
{
	(void)SetOutputSafetyOffset(
		akai_eie_driver::OutputSafetyOffsetFrames(sampleRateHz));
	(void)SetInputLatency(akai_eie_driver::InputLatencyFrames(sampleRateHz));
	(void)SetOutputLatency(akai_eie_driver::OutputLatencyFrames(sampleRateHz));
}

void AkaiEieDevice::SetTransportDriver(AkaiEieDriver *driver)
{
	ivars->transportDriver = driver;
}

void AkaiEieDevice::UpdatePlaybackTimestamp(uint64_t sampleTime,
											uint64_t hostTime)
{
	if (ivars == nullptr ||
		!__atomic_load_n(&ivars->ioStarted, __ATOMIC_ACQUIRE))
	{
		return;
	}

	ivars->zeroTimestampSampleTime = sampleTime;
	UpdateCurrentZeroTimestamp(sampleTime, hostTime);
}

kern_return_t AkaiEieDevice::ConfigureStreams(IOUserAudioDriver *driver)
{
	kern_return_t ret = IOBufferMemoryDescriptor::Create(
		kIOMemoryDirectionInOut,
		akai_eie_driver::kIOBufferBytes,
		0,
		ivars->outputBuffer.attach());
	if (ret != kIOReturnSuccess)
		return ret;

	ret = ivars->outputBuffer->SetLength(akai_eie_driver::kIOBufferBytes);
	if (ret != kIOReturnSuccess)
		return ret;

	ret = IOBufferMemoryDescriptor::Create(
		kIOMemoryDirectionInOut,
		akai_eie_driver::kIOBufferBytes,
		0,
		ivars->inputBuffer.attach());
	if (ret != kIOReturnSuccess)
		return ret;

	ret = ivars->inputBuffer->SetLength(akai_eie_driver::kIOBufferBytes);
	if (ret != kIOReturnSuccess)
		return ret;

	ivars->outputStream = IOUserAudioStream::Create(
		driver,
		IOUserAudioStreamDirection::Output,
		ivars->outputBuffer.get());
	if (!ivars->outputStream)
		return kIOReturnNoMemory;

	ivars->inputStream = IOUserAudioStream::Create(
		driver,
		IOUserAudioStreamDirection::Input,
		ivars->inputBuffer.get());
	if (!ivars->inputStream)
		return kIOReturnNoMemory;

	OSSharedPtr<OSString> outputName(
		OSString::withCString(akai_eie_driver::kOutputStreamName),
		OSNoRetain);
	OSSharedPtr<OSString> inputName(
		OSString::withCString(akai_eie_driver::kInputStreamName),
		OSNoRetain);
	if (!outputName || !inputName)
		return kIOReturnNoMemory;

	(void)ivars->outputStream->SetName(outputName.get());
	(void)ivars->inputStream->SetName(inputName.get());
	(void)ivars->outputStream->SetTerminalType(
		IOUserAudioStreamTerminalType::Line);
	(void)ivars->inputStream->SetTerminalType(
		IOUserAudioStreamTerminalType::Line);
	(void)ivars->outputStream->SetStartingChannel(1);
	(void)ivars->inputStream->SetStartingChannel(1);
	(void)ivars->outputStream->SetLatency(0);
	(void)ivars->inputStream->SetLatency(0);

	IOUserAudioStreamBasicDescription
		formats[CountOf(akai_eie_driver::kSupportedSampleRates)];
	for (uint32_t index = 0; index < CountOf(formats); ++index)
	{
		formats[index] = MakeFloat32StreamFormat(
			akai_eie_driver::kSupportedSampleRates[index]);
	}

	ret = ivars->outputStream->SetAvailableStreamFormats(
		formats, CountOf(formats));
	if (ret != kIOReturnSuccess)
		return ret;
	ret = ivars->inputStream->SetAvailableStreamFormats(
		formats, CountOf(formats));
	if (ret != kIOReturnSuccess)
		return ret;

	IOUserAudioStreamBasicDescription defaultFormat =
		MakeFloat32StreamFormat(akai_eie_driver::kDefaultSampleRateHz);
	ret = ivars->outputStream->SetCurrentStreamFormat(&defaultFormat);
	if (ret != kIOReturnSuccess)
		return ret;
	ret = ivars->inputStream->SetCurrentStreamFormat(&defaultFormat);
	if (ret != kIOReturnSuccess)
		return ret;

	ret = AddStream(ivars->outputStream.get());
	if (ret != kIOReturnSuccess)
		return ret;
	return AddStream(ivars->inputStream.get());
}

kern_return_t AkaiEieDevice::StartIO(IOUserAudioStartStopFlags flags)
{
	kern_return_t ret = super::StartIO(flags);
	if (ret != kIOReturnSuccess)
		return ret;

	ret = MapStreamBuffers();
	if (ret != kIOReturnSuccess)
	{
		(void)super::StopIO(flags);
		return ret;
	}

	ivars->ioStarted = true;

	if (ivars->transportDriver == nullptr)
		ret = kIOReturnNotReady;
	else
		ret = ivars->transportDriver->StartAudioClock();
	if (ret != kIOReturnSuccess)
	{
		ivars->ioStarted = false;
		UnmapStreamBuffers();
		(void)super::StopIO(flags);
		return ret;
	}

	(void)ivars->outputStream->SetStreamIsActive(true);
	(void)ivars->inputStream->SetStreamIsActive(true);
	return kIOReturnSuccess;
}

kern_return_t AkaiEieDevice::MapStreamBuffers()
{
	OSSharedPtr<IOMemoryDescriptor> outputDescriptor =
		ivars->outputStream->GetIOMemoryDescriptor();
	if (!outputDescriptor)
		return kIOReturnNoMemory;

	kern_return_t ret = outputDescriptor->CreateMapping(
		0, 0, 0, 0, 0, ivars->outputMap.attach());
	if (ret != kIOReturnSuccess)
		return ret;

	OSSharedPtr<IOMemoryDescriptor> inputDescriptor =
		ivars->inputStream->GetIOMemoryDescriptor();
	if (!inputDescriptor)
		return kIOReturnNoMemory;

	ret = inputDescriptor->CreateMapping(
		0, 0, 0, 0, 0, ivars->inputMap.attach());
	if (ret != kIOReturnSuccess)
		return ret;

	ivars->outputBufferAddress = reinterpret_cast<uint8_t *>(
		ivars->outputMap->GetAddress() + ivars->outputMap->GetOffset());
	ivars->inputBufferAddress = reinterpret_cast<uint8_t *>(
		ivars->inputMap->GetAddress() + ivars->inputMap->GetOffset());
	return kIOReturnSuccess;
}

kern_return_t AkaiEieDevice::ReadInputFrames(uint64_t sampleTime, uint32_t frameCount)
{
	if (ivars->transportDriver == nullptr ||
		ivars->inputBufferAddress == nullptr ||
		ivars->zeroTimestampPeriod == 0)
	{
		return kIOReturnNotReady;
	}

	// AudioDriverKit addresses stream windows modulo the hardware timeline period.
	while (frameCount > 0)
	{
		const uint32_t firstFrame = static_cast<uint32_t>(
			sampleTime % ivars->zeroTimestampPeriod);
		const uint32_t contiguousFrameCount =
			frameCount <= ivars->zeroTimestampPeriod - firstFrame
				? frameCount
				: ivars->zeroTimestampPeriod - firstFrame;
		float *frames = reinterpret_cast<float *>(
			ivars->inputBufferAddress +
			firstFrame * akai_eie_driver::kBytesPerFrame);
		const kern_return_t ret = ivars->transportDriver->CopyCaptureFrames(
			frames, contiguousFrameCount);
		if (ret != kIOReturnSuccess)
			return ret;
		sampleTime += contiguousFrameCount;
		frameCount -= contiguousFrameCount;
	}
	return kIOReturnSuccess;
}

kern_return_t AkaiEieDevice::QueueOutputFrames(uint64_t sampleTime, uint32_t frameCount)
{
	if (ivars->transportDriver == nullptr ||
		ivars->outputBufferAddress == nullptr ||
		ivars->zeroTimestampPeriod == 0)
		return kIOReturnNotReady;

	// AudioDriverKit addresses stream windows modulo the hardware timeline period.
	while (frameCount > 0)
	{
		const uint32_t firstFrame = static_cast<uint32_t>(
			sampleTime % ivars->zeroTimestampPeriod);
		const uint32_t contiguousFrameCount =
			frameCount <= ivars->zeroTimestampPeriod - firstFrame
				? frameCount
				: ivars->zeroTimestampPeriod - firstFrame;
		const float *frames = reinterpret_cast<const float *>(
			ivars->outputBufferAddress +
			firstFrame * akai_eie_driver::kBytesPerFrame);
		const kern_return_t ret = ivars->transportDriver->EnqueuePlaybackFrames(
			frames, contiguousFrameCount);
		if (ret != kIOReturnSuccess)
			return ret;
		sampleTime += contiguousFrameCount;
		frameCount -= contiguousFrameCount;
	}
	return kIOReturnSuccess;
}

kern_return_t AkaiEieDevice::StopIO(IOUserAudioStartStopFlags flags)
{
	if (ivars != nullptr)
	{
		ivars->ioStarted = false;
		if (ivars->outputStream)
			(void)ivars->outputStream->SetStreamIsActive(false);
		if (ivars->inputStream)
			(void)ivars->inputStream->SetStreamIsActive(false);
		UnmapStreamBuffers();
	}

	return super::StopIO(flags);
}

void AkaiEieDevice::UnmapStreamBuffers()
{
	ivars->outputBufferAddress = nullptr;
	ivars->inputBufferAddress = nullptr;
	ivars->outputMap.reset();
	ivars->inputMap.reset();
}

kern_return_t AkaiEieDevice::HandleChangeSampleRate(double sampleRate)
{
	if (!akai_eie_driver::IsSupportedSampleRate(sampleRate))
		return kIOReturnUnsupported;

	// The HAL stops IO before a configuration change, so the transport picks
	// the new rate up from its next StartPlayback reset sequence. The
	// zero-timestamp period is a fixed frame count and does not change.
	const uint32_t sampleRateHz = static_cast<uint32_t>(sampleRate);
	if (ivars->transportDriver != nullptr)
		ivars->transportDriver->SetTransportSampleRate(sampleRateHz);
	ApplyRateLatencies(sampleRateHz);

	kern_return_t ret = SetSampleRate(sampleRate);
	if (ret != kIOReturnSuccess)
		return ret;

	if (ivars->outputStream)
		(void)ivars->outputStream->DeviceSampleRateChanged(sampleRate);
	if (ivars->inputStream)
		(void)ivars->inputStream->DeviceSampleRateChanged(sampleRate);

	return kIOReturnSuccess;
}

kern_return_t AkaiEieDevice::PerformDeviceConfigurationChange(
	uint64_t changeAction,
	OSObject *changeInfo)
{
	return super::PerformDeviceConfigurationChange(changeAction, changeInfo);
}

kern_return_t AkaiEieDevice::AbortDeviceConfigurationChange(
	uint64_t changeAction,
	OSObject *changeInfo)
{
	return super::AbortDeviceConfigurationChange(changeAction, changeInfo);
}

void AkaiEieDevice::free()
{
	if (ivars != nullptr)
	{
		UnmapStreamBuffers();
		ivars->outputStream.reset();
		ivars->inputStream.reset();
		ivars->outputBuffer.reset();
		ivars->inputBuffer.reset();
	}
	IOSafeDeleteNULL(ivars, AkaiEieDevice_IVars, 1);
	super::free();
}
