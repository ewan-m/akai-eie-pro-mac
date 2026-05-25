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

void ZeroWrappedFrames(uint8_t *buffer, uint64_t sampleTime, uint32_t frameCount)
{
	if (buffer == nullptr || frameCount == 0)
		return;

	const uint32_t firstFrame =
		static_cast<uint32_t>(sampleTime % akai_eie_driver::kIOBufferFrameCount);
	const uint32_t firstFrameCount =
		frameCount <= akai_eie_driver::kIOBufferFrameCount - firstFrame
			? frameCount
			: akai_eie_driver::kIOBufferFrameCount - firstFrame;
	const uint32_t remainingFrameCount = frameCount - firstFrameCount;

	memset(buffer + firstFrame * akai_eie_driver::kBytesPerFrame,
		   0,
		   firstFrameCount * akai_eie_driver::kBytesPerFrame);

	if (remainingFrameCount > 0)
	{
		memset(buffer,
			   0,
			   remainingFrameCount * akai_eie_driver::kBytesPerFrame);
	}
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

	(void)SetInputSafetyOffset(akai_eie_driver::kSafetyOffsetFrames);
	(void)SetOutputSafetyOffset(akai_eie_driver::kSafetyOffsetFrames);
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

	ret = ConfigureZeroTimestampTimer();
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT,
			   "AkaiEieDevice::init ConfigureZeroTimestampTimer failed 0x%{public}x",
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
			ZeroInputFrames(sampleTime, ioBufferFrameSize);
		}
		else if (operation == IOUserAudioIOOperationWriteEnd)
		{
			__atomic_add_fetch(
				&ivars->writeOperationCount,
				1,
				__ATOMIC_RELAXED);
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

kern_return_t AkaiEieDevice::ConfigureZeroTimestampTimer()
{
	OSSharedPtr<IODispatchQueue> workQueue = GetWorkQueue();
	if (!workQueue)
		return kIOReturnNoMemory;

	kern_return_t ret = IOTimerDispatchSource::Create(
		workQueue.get(), ivars->zeroTimestampTimer.attach());
	if (ret != kIOReturnSuccess)
		return ret;

	OSSharedPtr<OSAction> timerAction;
	ret = CreateActionTimerOccurred(0, timerAction.attach());
	if (ret != kIOReturnSuccess)
		return ret;

	ret = ivars->zeroTimestampTimer->SetHandler(timerAction.get());
	if (ret != kIOReturnSuccess)
		return ret;

	mach_timebase_info_data_t timebase{};
	ret = mach_timebase_info(&timebase);
	if (ret != kIOReturnSuccess || timebase.numer == 0)
		return ret != kIOReturnSuccess ? ret : kIOReturnError;

	const uint64_t timestampIntervalNs =
		NSEC_PER_SEC / akai_eie_driver::kZeroTimestampCadenceDivisor;
	ivars->zeroTimestampIntervalTicks =
		timestampIntervalNs * timebase.denom / timebase.numer;
	return ivars->zeroTimestampIntervalTicks == 0 ? kIOReturnError
												  : kIOReturnSuccess;
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

	ivars->ioStarted = true;
	__atomic_store_n(&ivars->writeOperationCount, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&ivars->readOperationCount, 0, __ATOMIC_RELAXED);

	ret = StartZeroTimestampTimer();
	if (ret != kIOReturnSuccess)
	{
		ivars->ioStarted = false;
		UnmapStreamBuffers();
		(void)super::StopIO(flags);
		return ret;
	}

	(void)ivars->outputStream->SetStreamIsActive(true);
	(void)ivars->inputStream->SetStreamIsActive(true);
	ZeroInputFrames(0, akai_eie_driver::kIOBufferFrameCount);

	os_log(OS_LOG_DEFAULT, "AkaiEieDevice::StartIO");
	return kIOReturnSuccess;
}

kern_return_t AkaiEieDevice::StartZeroTimestampTimer()
{
	if (!ivars->zeroTimestampTimer)
		return kIOReturnNoDevice;

	ivars->zeroTimestampSampleTime = 0;
	const uint64_t hostTime = mach_absolute_time();
	ivars->zeroTimestampNextHostTime =
		hostTime + ivars->zeroTimestampIntervalTicks;
	UpdateCurrentZeroTimestamp(ivars->zeroTimestampSampleTime, hostTime);

	kern_return_t ret = ivars->zeroTimestampTimer->SetEnable(true);
	if (ret != kIOReturnSuccess)
		return ret;

	ret = ivars->zeroTimestampTimer->WakeAtTime(
		kIOTimerClockMachAbsoluteTime,
		ivars->zeroTimestampNextHostTime,
		0);
	if (ret != kIOReturnSuccess)
		(void)ivars->zeroTimestampTimer->SetEnable(false);
	return ret;
}

void AkaiEieDevice::StopZeroTimestampTimer()
{
	if (ivars->zeroTimestampTimer)
		(void)ivars->zeroTimestampTimer->SetEnable(false);
}

void AkaiEieDevice::TimerOccurred_Impl(OSAction *action, uint64_t time)
{
	(void)action;
	(void)time;
	if (ivars == nullptr || !ivars->ioStarted || !ivars->zeroTimestampTimer)
		return;

	ivars->zeroTimestampSampleTime += ivars->zeroTimestampPeriod;
	UpdateCurrentZeroTimestamp(
		ivars->zeroTimestampSampleTime,
		ivars->zeroTimestampNextHostTime);
	ivars->zeroTimestampNextHostTime += ivars->zeroTimestampIntervalTicks;

	const kern_return_t ret = ivars->zeroTimestampTimer->WakeAtTime(
		kIOTimerClockMachAbsoluteTime,
		ivars->zeroTimestampNextHostTime,
		0);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT,
			   "AkaiEieDevice::TimerOccurred WakeAtTime failed 0x%{public}x",
			   ret);
	}
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
	ZeroWrappedFrames(ivars->inputBufferAddress, sampleTime, frameCount);
}

void AkaiEieDevice::QueueOutputFrames(uint64_t sampleTime, uint32_t frameCount)
{
	if (ivars->transportDriver == nullptr ||
		ivars->outputBufferAddress == nullptr)
		return;

	while (frameCount > 0)
	{
		const uint32_t firstFrame = static_cast<uint32_t>(
			sampleTime % akai_eie_driver::kIOBufferFrameCount);
		const uint32_t contiguousFrameCount =
			frameCount <= akai_eie_driver::kIOBufferFrameCount - firstFrame
				? frameCount
				: akai_eie_driver::kIOBufferFrameCount - firstFrame;
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
			"AkaiEieDevice::StopIO writes=%{public}llu reads=%{public}llu zeroTimestamp=%{public}llu",
			static_cast<unsigned long long>(
				__atomic_load_n(&ivars->writeOperationCount, __ATOMIC_RELAXED)),
			static_cast<unsigned long long>(
				__atomic_load_n(&ivars->readOperationCount, __ATOMIC_RELAXED)),
			static_cast<unsigned long long>(ivars->zeroTimestampSampleTime));
		ivars->ioStarted = false;
		StopZeroTimestampTimer();
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
		StopZeroTimestampTimer();
		UnmapStreamBuffers();
		ivars->zeroTimestampTimer.reset();
		ivars->outputStream.reset();
		ivars->inputStream.reset();
		ivars->outputBuffer.reset();
		ivars->inputBuffer.reset();
	}
	IOSafeDeleteNULL(ivars, AkaiEieDevice_IVars, 1);
	super::free();
}
