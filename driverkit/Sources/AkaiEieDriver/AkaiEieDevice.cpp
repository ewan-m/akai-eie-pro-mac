#include <AudioDriverKit/AudioDriverKit.h>
#include <DriverKit/DriverKit.h>
#include <os/log.h>
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

	void ZeroWrappedFrames(uint8_t *buffer,
						   uint32_t ringFrameCount,
						   uint64_t sampleTime,
						   uint32_t frameCount)
	{
		if (buffer == nullptr || ringFrameCount == 0 || frameCount == 0)
			return;

		while (frameCount > 0)
		{
			const uint32_t firstFrame =
				static_cast<uint32_t>(sampleTime % ringFrameCount);
			const uint32_t contiguousFrameCount =
				frameCount <= ringFrameCount - firstFrame
					? frameCount
					: ringFrameCount - firstFrame;

			memset(buffer + firstFrame * akai_eie_driver::kBytesPerFrame,
				   0,
				   contiguousFrameCount * akai_eie_driver::kBytesPerFrame);
			sampleTime += contiguousFrameCount;
			frameCount -= contiguousFrameCount;
		}
	}

	void UpdateMaxRelaxed(uint64_t *maxValue, uint64_t candidate)
	{
		uint64_t current = __atomic_load_n(maxValue, __ATOMIC_RELAXED);
		while (candidate > current &&
			   !__atomic_compare_exchange_n(
				   maxValue,
				   &current,
				   candidate,
				   false,
				   __ATOMIC_RELAXED,
				   __ATOMIC_RELAXED))
		{
		}
	}

	const char *ConfigChangeName(uint64_t changeAction)
	{
		switch (static_cast<IOUserAudioReservedConfigChangeAction>(changeAction))
		{
		case IOUserAudioReservedConfigChangeAction::SampleRate:
			return "SampleRate";
		case IOUserAudioReservedConfigChangeAction::RingBufferFrameSize:
			return "RingBufferFrameSize";
		case IOUserAudioReservedConfigChangeAction::StreamFormat:
			return "StreamFormat";
		default:
			return "DriverSpecific";
		}
	}

	void LogConfigChange(const char *phase,
						 uint64_t changeAction,
						 OSObject *changeInfo)
	{
		OSNumber *number = OSDynamicCast(OSNumber, changeInfo);
		if (number != nullptr)
		{
			os_log(OS_LOG_DEFAULT,
				   "AkaiEieDevice::%{public}s action=%{public}llu (%{public}s) number=%{public}llu",
				   phase,
				   static_cast<unsigned long long>(changeAction),
				   ConfigChangeName(changeAction),
				   static_cast<unsigned long long>(
					   number->unsigned64BitValue()));
			return;
		}

		OSData *data = OSDynamicCast(OSData, changeInfo);
		if (data != nullptr)
		{
			os_log(OS_LOG_DEFAULT,
				   "AkaiEieDevice::%{public}s action=%{public}llu (%{public}s) data=%{public}p",
				   phase,
				   static_cast<unsigned long long>(changeAction),
				   ConfigChangeName(changeAction),
				   data);
			return;
		}

		os_log(OS_LOG_DEFAULT,
			   "AkaiEieDevice::%{public}s action=%{public}llu (%{public}s) info=%{public}p",
			   phase,
			   static_cast<unsigned long long>(changeAction),
			   ConfigChangeName(changeAction),
			   changeInfo);
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
		os_log(OS_LOG_DEFAULT, "AkaiEieDevice::init super init failed");
		return false;
	}

	ivars = IONewZero(AkaiEieDevice_IVars, 1);
	if (ivars == nullptr)
	{
		os_log(OS_LOG_DEFAULT, "AkaiEieDevice::init failed to allocate ivars");
		return false;
	}
	ivars->zeroTimestampPeriod = zeroTimestampPeriod;

	kern_return_t ret = SetTransportType(IOUserAudioTransportType::USB);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT,
			   "AkaiEieDevice::init SetTransportType failed 0x%{public}x",
			   ret);
		return false;
	}

	ret = SetAvailableSampleRates(
		akai_eie_driver::kSupportedSampleRates,
		CountOf(akai_eie_driver::kSupportedSampleRates));
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT,
			   "AkaiEieDevice::init SetAvailableSampleRates failed 0x%{public}x",
			   ret);
		return false;
	}

	ret = SetSampleRate(akai_eie_driver::kDefaultSampleRateHz);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT,
			   "AkaiEieDevice::init SetSampleRate failed 0x%{public}x",
			   ret);
		return false;
	}

	(void)SetInputSafetyOffset(akai_eie_driver::kInputSafetyOffsetFrames);
	(void)SetOutputSafetyOffset(akai_eie_driver::kOutputSafetyOffsetFrames);
	(void)SetInputLatency(0);
	(void)SetOutputLatency(akai_eie_driver::kOutputTransportLatencyFrames);
	(void)SetPreferredChannelsForStereo(1, 2);

	IOUserAudioChannelLabel labels[akai_eie_driver::kChannelCount] = {
		IOUserAudioChannelLabel::Discrete_0,
		IOUserAudioChannelLabel::Discrete_1,
		IOUserAudioChannelLabel::Discrete_2,
		IOUserAudioChannelLabel::Discrete_3,
	};
	(void)SetPreferredInputChannelLayout(labels, akai_eie_driver::kChannelCount);
	(void)SetPreferredOutputChannelLayout(labels, akai_eie_driver::kChannelCount);

	ret = ConfigureStreams(driver);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT,
			   "AkaiEieDevice::init ConfigureStreams failed 0x%{public}x",
			   ret);
		return false;
	}

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

	  if (operation == IOUserAudioIOOperationBeginRead)
	  {
		  __atomic_add_fetch(
			  &ivars->readOperationCount,
			  1,
			  __ATOMIC_RELAXED);
		  UpdateMaxRelaxed(
			  &ivars->maxReadBufferFrameSize,
			  ioBufferFrameSize);
		  ReadInputFrames(sampleTime, ioBufferFrameSize);
	  }
	  else if (operation == IOUserAudioIOOperationWriteEnd)
	  {
		  __atomic_add_fetch(
			  &ivars->writeOperationCount,
			  1,
			  __ATOMIC_RELAXED);
		  UpdateMaxRelaxed(
			  &ivars->maxWriteBufferFrameSize,
			  ioBufferFrameSize);
		  QueueOutputFrames(sampleTime, ioBufferFrameSize);
	  }

	  return kIOReturnSuccess;
	};

	ret = SetIOOperationHandler(ioOperation);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT,
			   "AkaiEieDevice::init SetIOOperationHandler failed 0x%{public}x",
			   ret);
		return false;
	}

	return true;
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

	IOUserAudioStreamBasicDescription formats[] = {
		MakeFloat32StreamFormat(akai_eie_driver::kDefaultSampleRateHz),
	};

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
	{
		os_log(OS_LOG_DEFAULT,
			   "AkaiEieDevice::StartIO super failed 0x%{public}x",
			   ret);
		return ret;
	}

	ret = MapStreamBuffers();
	if (ret != kIOReturnSuccess)
	{
		(void)super::StopIO(flags);
		return ret;
	}

	__atomic_store_n(&ivars->writeOperationCount, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&ivars->readOperationCount, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&ivars->maxWriteBufferFrameSize, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&ivars->maxReadBufferFrameSize, 0, __ATOMIC_RELAXED);
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
	ZeroInputFrames(0, ivars->zeroTimestampPeriod);

	os_log(OS_LOG_DEFAULT,
		   "AkaiEieDevice::StartIO zeroTimestampPeriod=%{public}u streamBufferFrames=%{public}u",
		   ivars->zeroTimestampPeriod,
		   akai_eie_driver::kIOBufferFrameCount);
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

void AkaiEieDevice::ZeroInputFrames(uint64_t sampleTime, uint32_t frameCount)
{
	ZeroWrappedFrames(
		ivars->inputBufferAddress,
		ivars->zeroTimestampPeriod,
		sampleTime,
		frameCount);
}

void AkaiEieDevice::ReadInputFrames(uint64_t sampleTime, uint32_t frameCount)
{
	if (ivars->transportDriver == nullptr ||
		ivars->inputBufferAddress == nullptr ||
		ivars->zeroTimestampPeriod == 0)
	{
		ZeroInputFrames(sampleTime, frameCount);
		return;
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
		ivars->transportDriver->CopyCaptureFrames(
			frames, contiguousFrameCount);
		sampleTime += contiguousFrameCount;
		frameCount -= contiguousFrameCount;
	}
}

void AkaiEieDevice::QueueOutputFrames(uint64_t sampleTime, uint32_t frameCount)
{
	if (ivars->transportDriver == nullptr ||
		ivars->outputBufferAddress == nullptr ||
		ivars->zeroTimestampPeriod == 0)
		return;

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
		ivars->transportDriver->EnqueuePlaybackFrames(
			frames, contiguousFrameCount);
		sampleTime += contiguousFrameCount;
		frameCount -= contiguousFrameCount;
	}
}

kern_return_t AkaiEieDevice::StopIO(IOUserAudioStartStopFlags flags)
{
	if (ivars != nullptr)
	{
		os_log(
			OS_LOG_DEFAULT,
			"AkaiEieDevice::StopIO writes=%{public}llu reads=%{public}llu "
			"maxWriteFrames=%{public}llu maxReadFrames=%{public}llu "
			"zeroTimestamp=%{public}llu",
			static_cast<unsigned long long>(
				__atomic_load_n(&ivars->writeOperationCount, __ATOMIC_RELAXED)),
			static_cast<unsigned long long>(
				__atomic_load_n(&ivars->readOperationCount, __ATOMIC_RELAXED)),
			static_cast<unsigned long long>(
				__atomic_load_n(&ivars->maxWriteBufferFrameSize, __ATOMIC_RELAXED)),
			static_cast<unsigned long long>(
				__atomic_load_n(&ivars->maxReadBufferFrameSize, __ATOMIC_RELAXED)),
			static_cast<unsigned long long>(ivars->zeroTimestampSampleTime));
		ivars->ioStarted = false;
		if (ivars->outputStream)
			(void)ivars->outputStream->SetStreamIsActive(false);
		if (ivars->inputStream)
			(void)ivars->inputStream->SetStreamIsActive(false);
		UnmapStreamBuffers();
	}

	kern_return_t ret = super::StopIO(flags);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT,
			   "AkaiEieDevice::StopIO super failed 0x%{public}x",
			   ret);
	}
	return ret;
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

	const uint32_t sampleRateHz = static_cast<uint32_t>(sampleRate);
	ivars->zeroTimestampPeriod =
		akai_eie_driver::ZeroTimestampPeriod(sampleRateHz);
	kern_return_t ret = SetZeroTimeStampPeriod(ivars->zeroTimestampPeriod);
	if (ret != kIOReturnSuccess)
		return ret;

	ret = SetSampleRate(sampleRate);
	if (ret != kIOReturnSuccess)
		return ret;

	if (ivars->outputStream)
		(void)ivars->outputStream->DeviceSampleRateChanged(sampleRate);
	if (ivars->inputStream)
		(void)ivars->inputStream->DeviceSampleRateChanged(sampleRate);

	os_log(OS_LOG_DEFAULT,
		   "AkaiEieDevice::HandleChangeSampleRate %{public}.0f",
		   sampleRate);
	return kIOReturnSuccess;
}

kern_return_t AkaiEieDevice::PerformDeviceConfigurationChange(
	uint64_t changeAction,
	OSObject *changeInfo)
{
	LogConfigChange(
		"PerformDeviceConfigurationChange",
		changeAction,
		changeInfo);
	const kern_return_t ret =
		super::PerformDeviceConfigurationChange(changeAction, changeInfo);
	os_log(OS_LOG_DEFAULT,
		   "AkaiEieDevice::PerformDeviceConfigurationChange result=0x%{public}x",
		   ret);
	return ret;
}

kern_return_t AkaiEieDevice::AbortDeviceConfigurationChange(
	uint64_t changeAction,
	OSObject *changeInfo)
{
	LogConfigChange(
		"AbortDeviceConfigurationChange",
		changeAction,
		changeInfo);
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
