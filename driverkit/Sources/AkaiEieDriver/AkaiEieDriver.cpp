#include <AudioDriverKit/AudioDriverKit.h>
#include <DriverKit/DriverKit.h>
#include <USBDriverKit/USBDriverKit.h>
#include <os/log.h>
#include <string.h>

#include "AkaiEieDriver.h"
#include "AkaiEieDriverConfig.hpp"
#include "../../../protocol/akai_eie_protocol.hpp"

bool AkaiEieDriver::init()
{
	if (!super::init())
	{
		os_log(OS_LOG_DEFAULT, "AkaiEieDriver::init super init failed");
		return false;
	}

	ivars = IONewZero(AkaiEieDriver_IVars, 1);
	if (ivars == nullptr)
	{
		os_log(OS_LOG_DEFAULT, "AkaiEieDriver::init failed to allocate ivars");
		return false;
	}

	ivars->driverName = OSSharedPtr<OSString>(
		OSString::withCString(akai_eie_driver::kDriverName),
		OSNoRetain);
	ivars->deviceName = OSSharedPtr<OSString>(
		OSString::withCString(akai_eie_driver::kDeviceName),
		OSNoRetain);
	ivars->deviceUid = OSSharedPtr<OSString>(
		OSString::withCString(akai_eie_driver::kDeviceUid),
		OSNoRetain);
	ivars->modelUid = OSSharedPtr<OSString>(
		OSString::withCString(akai_eie_driver::kModelUid),
		OSNoRetain);
	ivars->manufacturerName = OSSharedPtr<OSString>(
		OSString::withCString(akai_eie_driver::kManufacturerName),
		OSNoRetain);

	if (!ivars->driverName || !ivars->deviceName || !ivars->deviceUid ||
		!ivars->modelUid || !ivars->manufacturerName)
	{
		os_log(OS_LOG_DEFAULT, "AkaiEieDriver::init failed to allocate strings");
		return false;
	}

	return true;
}

kern_return_t IMPL(AkaiEieDriver, Start)
{
	os_log(OS_LOG_DEFAULT, "AkaiEieDriver::Start");

	kern_return_t ret = Start(provider, SUPERDISPATCH);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT,
			   "AkaiEieDriver::Start super failed 0x%{public}x",
			   ret);
		return ret;
	}

	ret = SetName(ivars->driverName.get());
	if (ret != kIOReturnSuccess)
		return ret;

	ret = SetTransportType(IOUserAudioTransportType::USB);
	if (ret != kIOReturnSuccess)
		return ret;

	ret = CaptureUsbProvider(provider);
	if (ret != kIOReturnSuccess)
		return ret;

	ivars->workQueue = GetWorkQueue();
	if (!ivars->workQueue)
		return kIOReturnNoMemory;

	ret = CreateAudioDevice();
	if (ret != kIOReturnSuccess)
	{
		CloseUsbProvider();
		return ret;
	}

	ret = RegisterService();
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT,
			   "AkaiEieDriver::Start RegisterService failed 0x%{public}x",
			   ret);
		ivars->audioDevice.reset();
		CloseUsbProvider();
		return ret;
	}

	os_log(OS_LOG_DEFAULT,
		   "AkaiEieDriver::Start registered AudioDriverKit device for AKAI EIE Pro");
	return kIOReturnSuccess;
}

kern_return_t AkaiEieDriver::CaptureUsbProvider(IOService *provider)
{
	IOUSBHostDevice *usbDevice = OSDynamicCast(IOUSBHostDevice, provider);
	if (usbDevice == nullptr)
	{
		os_log(OS_LOG_DEFAULT,
			   "AkaiEieDriver::CaptureUsbProvider provider is not IOUSBHostDevice");
		return kIOReturnBadArgument;
	}

	ivars->usbDevice = OSSharedPtr<IOUSBHostDevice>(usbDevice, OSRetain);

	const IOUSBDeviceDescriptor *descriptor =
		ivars->usbDevice->CopyDeviceDescriptor();
	if (descriptor == nullptr)
	{
		os_log(OS_LOG_DEFAULT,
			   "AkaiEieDriver::CaptureUsbProvider missing device descriptor");
		return kIOReturnNoDevice;
	}

	const uint16_t vendorId = descriptor->idVendor;
	const uint16_t productId = descriptor->idProduct;
	IOUSBHostFreeDescriptor(descriptor);

	if (vendorId != akai_eie::kVendorId || productId != akai_eie::kProductId)
	{
		os_log(OS_LOG_DEFAULT,
			   "AkaiEieDriver::CaptureUsbProvider unexpected USB id %{public}04x:%{public}04x",
			   vendorId,
			   productId);
		return kIOReturnUnsupported;
	}

	kern_return_t ret = ivars->usbDevice->Open(this, 0, 0);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT,
			   "AkaiEieDriver::CaptureUsbProvider Open failed 0x%{public}x",
			   ret);
		return ret;
	}
	ivars->usbDeviceOpen = true;

	ret = ivars->usbDevice->SetConfiguration(
		akai_eie_driver::kUsbConfigurationValue,
		true);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT,
			   "AkaiEieDriver::CaptureUsbProvider SetConfiguration(%{public}u) failed 0x%{public}x",
			   akai_eie_driver::kUsbConfigurationValue,
			   ret);
		CloseUsbProvider();
		return ret;
	}

	os_log(OS_LOG_DEFAULT,
		   "AkaiEieDriver::CaptureUsbProvider matched AKAI EIE Pro %{public}04x:%{public}04x configuration %{public}u",
		   vendorId,
		   productId,
		   akai_eie_driver::kUsbConfigurationValue);
	return kIOReturnSuccess;
}

kern_return_t AkaiEieDriver::CreateAudioDevice()
{
	ivars->audioDevice = OSSharedPtr<AkaiEieDevice>(
		OSTypeAlloc(AkaiEieDevice),
		OSNoRetain);
	if (!ivars->audioDevice)
		return kIOReturnNoMemory;

	const bool ok = ivars->audioDevice->init(
		this,
		false,
		ivars->deviceUid.get(),
		ivars->modelUid.get(),
		ivars->manufacturerName.get(),
		akai_eie_driver::ZeroTimestampPeriod(
			akai_eie_driver::kDefaultSampleRateHz));
	if (!ok)
		return kIOReturnNoMemory;

	ivars->audioDevice->SetTransportDriver(this);

	kern_return_t ret = ivars->audioDevice->SetName(ivars->deviceName.get());
	if (ret != kIOReturnSuccess)
		return ret;

	ret = AddObject(ivars->audioDevice.get());
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT,
			   "AkaiEieDriver::CreateAudioDevice AddObject failed 0x%{public}x",
			   ret);
		return ret;
	}

	return kIOReturnSuccess;
}

kern_return_t AkaiEieDriver::StartDevice(IOUserAudioObjectID objectId,
										 IOUserAudioStartStopFlags flags)
{
	if (!ivars || !ivars->audioDevice ||
		objectId != ivars->audioDevice->GetObjectID())
	{
		return kIOReturnBadArgument;
	}

	kern_return_t ret = StartPlayback();
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT,
			   "AkaiEieDriver::StartDevice StartPlayback failed 0x%{public}x",
			   ret);
		return ret;
	}

	ret = super::StartDevice(objectId, flags);
	if (ret != kIOReturnSuccess)
		StopPlayback();
	return ret;
}

kern_return_t AkaiEieDriver::StopDevice(IOUserAudioObjectID objectId,
										IOUserAudioStartStopFlags flags)
{
	if (!ivars || !ivars->audioDevice ||
		objectId != ivars->audioDevice->GetObjectID())
	{
		return kIOReturnBadArgument;
	}

	const kern_return_t ret = super::StopDevice(objectId, flags);
	StopPlayback();
	return ret;
}

kern_return_t AkaiEieDriver::OpenPlaybackInterface()
{
	if (!ivars->usbDevice)
		return kIOReturnNoDevice;

	uintptr_t iterator = 0;
	kern_return_t ret = ivars->usbDevice->CreateInterfaceIterator(&iterator);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT,
			   "AkaiEieDriver::OpenPlaybackInterface CreateInterfaceIterator failed 0x%{public}x",
			   ret);
		return ret;
	}

	while (!ivars->playbackInterface)
	{
		OSSharedPtr<IOUSBHostInterface> candidate;
		ret = ivars->usbDevice->CopyInterface(iterator, candidate.attach());
		if (ret != kIOReturnSuccess || !candidate)
			break;

		const IOUSBConfigurationDescriptor *configuration =
			candidate->CopyConfigurationDescriptor();
		const IOUSBInterfaceDescriptor *descriptor = configuration == nullptr
														 ? nullptr
														 : candidate->GetInterfaceDescriptor(configuration);
		const uint8_t interfaceNumber =
			descriptor == nullptr ? UINT8_MAX : descriptor->bInterfaceNumber;
		if (configuration != nullptr)
			IOUSBHostFreeDescriptor(configuration);

		os_log(OS_LOG_DEFAULT,
			   "AkaiEieDriver::OpenPlaybackInterface found interface %{public}u",
			   interfaceNumber);
		if (interfaceNumber == akai_eie_driver::kPlaybackInterfaceNumber)
			ivars->playbackInterface = candidate;
	}
	(void)ivars->usbDevice->DestroyInterfaceIterator(iterator);

	if (!ivars->playbackInterface)
	{
		os_log(OS_LOG_DEFAULT,
			   "AkaiEieDriver::OpenPlaybackInterface output interface unavailable 0x%{public}x",
			   ret);
		return kIOReturnNoDevice;
	}

	ret = ivars->playbackInterface->Open(this, 0, nullptr);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT,
			   "AkaiEieDriver::OpenPlaybackInterface output Open failed 0x%{public}x",
			   ret);
		return ret;
	}
	ivars->playbackInterfaceOpen = true;

	ret = ivars->playbackInterface->SelectAlternateSetting(
		akai_eie_driver::kPlaybackAlternateSetting);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT,
			   "AkaiEieDriver::OpenPlaybackInterface output alternate setting failed 0x%{public}x",
			   ret);
		return ret;
	}

	ret = ivars->playbackInterface->CopyPipe(
		akai_eie_driver::kPlaybackEndpointAddress,
		ivars->playbackPipe.attach());
	if (ret != kIOReturnSuccess || !ivars->playbackPipe)
	{
		os_log(OS_LOG_DEFAULT,
			   "AkaiEieDriver::OpenPlaybackInterface output pipe failed 0x%{public}x",
			   ret);
		return ret != kIOReturnSuccess ? ret : kIOReturnNoDevice;
	}

	IOUSBStandardEndpointDescriptors endpointDescriptors{};
	ret = ivars->playbackPipe->GetDescriptors(
		&endpointDescriptors, kIOUSBGetEndpointDescriptorCurrentPolicy);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT,
			   "AkaiEieDriver::OpenPlaybackInterface output descriptor failed 0x%{public}x",
			   ret);
		return ret;
	}
	if (endpointDescriptors.descriptor.bEndpointAddress !=
		akai_eie_driver::kPlaybackEndpointAddress)
		return kIOReturnUnsupported;

	return kIOReturnSuccess;
}

kern_return_t AkaiEieDriver::SendPlaybackSetup()
{
	akai_eie::ControlTransfer transfers[akai_eie::kResetControlTransferCount]{};
	const size_t transferCount = akai_eie::BuildResetControlTransfers(
		akai_eie_driver::kDefaultSampleRateHz,
		transfers,
		akai_eie::kResetControlTransferCount);
	if (transferCount != akai_eie::kResetControlTransferCount)
		return kIOReturnUnsupported;

	OSSharedPtr<IOBufferMemoryDescriptor> dataBuffer;
	kern_return_t ret = ivars->usbDevice->CreateIOBuffer(
		kIOMemoryDirectionInOut,
		akai_eie::kMaxControlPayloadBytes,
		dataBuffer.attach());
	if (ret != kIOReturnSuccess)
		return ret;

	ret = dataBuffer->SetLength(akai_eie::kMaxControlPayloadBytes);
	if (ret != kIOReturnSuccess)
		return ret;

	OSSharedPtr<IOMemoryMap> dataMap;
	ret = dataBuffer->CreateMapping(0, 0, 0, 0, 0, dataMap.attach());
	if (ret != kIOReturnSuccess)
		return ret;
	uint8_t *dataAddress = reinterpret_cast<uint8_t *>(
		dataMap->GetAddress() + dataMap->GetOffset());

	for (size_t index = 0; index < transferCount; ++index)
	{
		const akai_eie::ControlTransfer &transfer = transfers[index];
		IOMemoryDescriptor *requestBuffer = nullptr;
		if (transfer.length > 0)
		{
			memset(dataAddress, 0, akai_eie::kMaxControlPayloadBytes);
			if (!akai_eie::IsDeviceToHost(transfer))
				memcpy(dataAddress, transfer.data, transfer.length);
			ret = dataBuffer->SetLength(transfer.length);
			if (ret != kIOReturnSuccess)
				return ret;
			requestBuffer = dataBuffer.get();
		}

		uint16_t bytesTransferred = 0;
		ret = ivars->usbDevice->DeviceRequest(
			this,
			transfer.request_type,
			transfer.request,
			transfer.value,
			transfer.index,
			transfer.length,
			requestBuffer,
			&bytesTransferred,
			1000);
		if (ret != kIOReturnSuccess)
		{
			os_log(OS_LOG_DEFAULT,
				   "AkaiEieDriver::SendPlaybackSetup transfer %{public}lu failed 0x%{public}x",
				   static_cast<unsigned long>(index),
				   ret);
			return ret;
		}
	}

	return kIOReturnSuccess;
}

kern_return_t AkaiEieDriver::CreatePlaybackTransfers()
{
	const uint32_t frameListBytes =
		static_cast<uint32_t>(sizeof(IOUSBIsochronousFrame)) *
		akai_eie_driver::kPlaybackIsoPacketCount;

	for (uint32_t index = 0;
		 index < akai_eie_driver::kPlaybackTransferCount;
		 ++index)
	{
		AkaiEiePlaybackTransfer &transfer = ivars->playbackTransfers[index];
		kern_return_t ret = ivars->playbackInterface->CreateIOBuffer(
			kIOMemoryDirectionInOut,
			akai_eie_driver::kPlaybackTransferBytes,
			transfer.dataBuffer.attach());
		if (ret != kIOReturnSuccess)
			return ret;
		ret = transfer.dataBuffer->SetLength(
			akai_eie_driver::kPlaybackTransferBytes);
		if (ret != kIOReturnSuccess)
			return ret;
		ret = transfer.dataBuffer->CreateMapping(
			0, 0, 0, 0, 0, transfer.dataMap.attach());
		if (ret != kIOReturnSuccess)
			return ret;
		transfer.dataAddress = reinterpret_cast<uint8_t *>(
			transfer.dataMap->GetAddress() + transfer.dataMap->GetOffset());

		ret = ivars->playbackInterface->CreateIOBuffer(
			kIOMemoryDirectionInOut,
			frameListBytes,
			transfer.frameListBuffer.attach());
		if (ret != kIOReturnSuccess)
			return ret;
		ret = transfer.frameListBuffer->SetLength(frameListBytes);
		if (ret != kIOReturnSuccess)
			return ret;
		ret = transfer.frameListBuffer->CreateMapping(
			0, 0, 0, 0, 0, transfer.frameListMap.attach());
		if (ret != kIOReturnSuccess)
			return ret;
		transfer.frameListAddress =
			reinterpret_cast<IOUSBIsochronousFrame *>(
				transfer.frameListMap->GetAddress() +
				transfer.frameListMap->GetOffset());

		ret = CreateActionPlaybackComplete(
			sizeof(index), transfer.completion.attach());
		if (ret != kIOReturnSuccess)
			return ret;
		memcpy(transfer.completion->GetReference(), &index, sizeof(index));
	}
	return kIOReturnSuccess;
}

kern_return_t AkaiEieDriver::StartPlayback()
{
	if (__atomic_load_n(&ivars->playbackRunning, __ATOMIC_ACQUIRE))
		return kIOReturnSuccess;
	if (__atomic_load_n(&ivars->playbackStopPending, __ATOMIC_ACQUIRE))
		return kIOReturnBusy;

	ivars->playbackRingAddress = IONewZero(
		uint8_t, akai_eie_driver::kPlaybackRingBytes);
	if (ivars->playbackRingAddress == nullptr)
		return kIOReturnNoMemory;
	__atomic_store_n(&ivars->playbackReadFrame, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&ivars->playbackWriteFrame, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&ivars->playbackEnqueuedFrames, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&ivars->playbackSubmittedFrames, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&ivars->playbackUnderrunFrames, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&ivars->playbackDroppedFrames, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&ivars->playbackPacketErrorCount, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&ivars->playbackHasReceivedFrames, false, __ATOMIC_RELAXED);
	__atomic_store_n(&ivars->playbackTransfersStarted, false, __ATOMIC_RELAXED);

	kern_return_t ret = OpenPlaybackInterface();
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT,
			   "AkaiEieDriver::StartPlayback interface setup failed 0x%{public}x",
			   ret);
	}
	if (ret == kIOReturnSuccess)
	{
		ret = SendPlaybackSetup();
		if (ret != kIOReturnSuccess)
		{
			os_log(OS_LOG_DEFAULT,
				   "AkaiEieDriver::StartPlayback device setup failed 0x%{public}x",
				   ret);
		}
	}
	if (ret == kIOReturnSuccess)
	{
		ret = CreatePlaybackTransfers();
		if (ret != kIOReturnSuccess)
		{
			os_log(OS_LOG_DEFAULT,
				   "AkaiEieDriver::StartPlayback buffer setup failed 0x%{public}x",
				   ret);
		}
	}

	if (ret != kIOReturnSuccess)
	{
		StopPlayback();
		return ret;
	}

	ivars->nextUsbFrame = 0;
	ivars->playbackInFlight = 0;
	__atomic_store_n(&ivars->playbackRunning, true, __ATOMIC_RELEASE);

	os_log(OS_LOG_DEFAULT,
		   "AkaiEieDriver::StartPlayback ready 48 kHz endpoint 0x%{public}02x",
		   akai_eie_driver::kPlaybackEndpointAddress);
	return kIOReturnSuccess;
}

void AkaiEieDriver::EnqueuePlaybackFrames(const float *frames,
										  uint32_t frameCount)
{
	if (frames == nullptr || frameCount == 0 ||
		ivars->playbackRingAddress == nullptr ||
		!__atomic_load_n(&ivars->playbackRunning, __ATOMIC_ACQUIRE))
		return;

	if (frameCount > akai_eie_driver::kPlaybackRingFrameCount)
	{
		__atomic_add_fetch(
			&ivars->playbackDroppedFrames,
			frameCount - akai_eie_driver::kPlaybackRingFrameCount,
			__ATOMIC_RELAXED);
		frames += static_cast<size_t>(
			frameCount - akai_eie_driver::kPlaybackRingFrameCount) *
			akai_eie_driver::kChannelCount;
		frameCount = akai_eie_driver::kPlaybackRingFrameCount;
	}

	const uint64_t writeFrame =
		__atomic_load_n(&ivars->playbackWriteFrame, __ATOMIC_RELAXED);
	const uint64_t minimumReadFrame =
		writeFrame + frameCount > akai_eie_driver::kPlaybackRingFrameCount
			? writeFrame + frameCount -
				  akai_eie_driver::kPlaybackRingFrameCount
			: 0;
	uint64_t readFrame =
		__atomic_load_n(&ivars->playbackReadFrame, __ATOMIC_ACQUIRE);
	uint64_t overwrittenFrames = 0;
	while (readFrame < minimumReadFrame &&
		   !__atomic_compare_exchange_n(
			   &ivars->playbackReadFrame,
			   &readFrame,
			   minimumReadFrame,
			   false,
			   __ATOMIC_RELEASE,
			   __ATOMIC_ACQUIRE))
	{
	}
	if (readFrame < minimumReadFrame)
		overwrittenFrames = minimumReadFrame - readFrame;
	if (overwrittenFrames > 0)
	{
		__atomic_add_fetch(
			&ivars->playbackDroppedFrames,
			overwrittenFrames,
			__ATOMIC_RELAXED);
	}

	const uint32_t firstRingFrame = static_cast<uint32_t>(
		writeFrame % akai_eie_driver::kPlaybackRingFrameCount);
	const uint32_t firstFrameCount =
		frameCount <= akai_eie_driver::kPlaybackRingFrameCount - firstRingFrame
			? frameCount
			: akai_eie_driver::kPlaybackRingFrameCount - firstRingFrame;
	const uint32_t remainingFrameCount = frameCount - firstFrameCount;

	(void)akai_eie::EncodePlaybackBufferFromFloat32(
		frames,
		firstFrameCount,
		ivars->playbackRingAddress +
			firstRingFrame * akai_eie_driver::kPlaybackBytesPerUsbFrame,
		static_cast<size_t>(firstFrameCount) *
			akai_eie_driver::kPlaybackBytesPerUsbFrame);
	if (remainingFrameCount > 0)
	{
		(void)akai_eie::EncodePlaybackBufferFromFloat32(
			frames + static_cast<size_t>(firstFrameCount) *
						 akai_eie_driver::kChannelCount,
			remainingFrameCount,
			ivars->playbackRingAddress,
			static_cast<size_t>(remainingFrameCount) *
				akai_eie_driver::kPlaybackBytesPerUsbFrame);
	}

	__atomic_store_n(
		&ivars->playbackWriteFrame,
		writeFrame + frameCount,
		__ATOMIC_RELEASE);
	__atomic_add_fetch(
		&ivars->playbackEnqueuedFrames,
		frameCount,
		__ATOMIC_RELAXED);
	__atomic_store_n(
		&ivars->playbackHasReceivedFrames,
		true,
		__ATOMIC_RELEASE);

	const uint64_t queuedFrames =
		writeFrame + frameCount -
		__atomic_load_n(&ivars->playbackReadFrame, __ATOMIC_ACQUIRE);
	if (!__atomic_load_n(&ivars->playbackTransfersStarted, __ATOMIC_ACQUIRE) &&
		queuedFrames >= akai_eie_driver::kPlaybackPrimeFrameCount)
	{
		const kern_return_t ret = StartPrimedPlaybackTransfers();
		if (ret != kIOReturnSuccess)
		{
			os_log(OS_LOG_DEFAULT,
				   "AkaiEieDriver::EnqueuePlaybackFrames initial submit failed 0x%{public}x",
				   ret);
			StopPlayback();
		}
	}
}

kern_return_t AkaiEieDriver::StartPrimedPlaybackTransfers()
{
	if (!__atomic_load_n(&ivars->playbackRunning, __ATOMIC_ACQUIRE))
		return kIOReturnNotReady;

	bool expected = false;
	if (!__atomic_compare_exchange_n(
			&ivars->playbackTransfersStarted,
			&expected,
			true,
			false,
			__ATOMIC_ACQ_REL,
			__ATOMIC_ACQUIRE))
	{
		return kIOReturnSuccess;
	}

	uint64_t currentFrame = 0;
	uint64_t currentTime = 0;
	kern_return_t ret = ivars->playbackInterface->GetFrameNumber(
		&currentFrame, &currentTime);
	if (ret == kIOReturnSuccess)
	{
		ivars->nextUsbFrame =
			currentFrame + akai_eie_driver::kPlaybackScheduleLeadBusFrames;
		for (uint32_t index = 0;
			 index < akai_eie_driver::kPlaybackTransferCount;
			 ++index)
		{
			ret = SubmitPlaybackTransfer(index);
			if (ret != kIOReturnSuccess)
				break;
		}
	}
	if (ret != kIOReturnSuccess)
		return ret;

	os_log(OS_LOG_DEFAULT,
		   "AkaiEieDriver::StartPlayback started with %{public}u primed frames",
		   akai_eie_driver::kPlaybackPrimeFrameCount);
	return kIOReturnSuccess;
}

kern_return_t AkaiEieDriver::SubmitPlaybackTransfer(uint32_t index)
{
	if (index >= akai_eie_driver::kPlaybackTransferCount ||
		!ivars->playbackPipe ||
		!__atomic_load_n(&ivars->playbackRunning, __ATOMIC_ACQUIRE))
		return kIOReturnNotReady;

	AkaiEiePlaybackTransfer &transfer = ivars->playbackTransfers[index];
	const uint64_t readFrame =
		__atomic_load_n(&ivars->playbackReadFrame, __ATOMIC_RELAXED);
	const uint64_t writeFrame =
		__atomic_load_n(&ivars->playbackWriteFrame, __ATOMIC_ACQUIRE);
	const uint64_t availableFrames =
		writeFrame > readFrame ? writeFrame - readFrame : 0;
	const uint32_t framesToCopy =
		availableFrames < akai_eie_driver::kPlaybackFramesPerTransfer
			? static_cast<uint32_t>(availableFrames)
			: akai_eie_driver::kPlaybackFramesPerTransfer;
	const uint32_t firstRingFrame = static_cast<uint32_t>(
		readFrame % akai_eie_driver::kPlaybackRingFrameCount);
	const uint32_t firstFrameCount =
		framesToCopy <=
				akai_eie_driver::kPlaybackRingFrameCount - firstRingFrame
			? framesToCopy
			: akai_eie_driver::kPlaybackRingFrameCount - firstRingFrame;
	const uint32_t remainingFrameCount = framesToCopy - firstFrameCount;

	memcpy(transfer.dataAddress,
		   ivars->playbackRingAddress +
			   firstRingFrame * akai_eie_driver::kPlaybackBytesPerUsbFrame,
		   static_cast<size_t>(firstFrameCount) *
			   akai_eie_driver::kPlaybackBytesPerUsbFrame);
	if (remainingFrameCount > 0)
	{
		memcpy(transfer.dataAddress +
				   static_cast<size_t>(firstFrameCount) *
					   akai_eie_driver::kPlaybackBytesPerUsbFrame,
			   ivars->playbackRingAddress,
			   static_cast<size_t>(remainingFrameCount) *
				   akai_eie_driver::kPlaybackBytesPerUsbFrame);
	}
	if (framesToCopy < akai_eie_driver::kPlaybackFramesPerTransfer)
	{
		memset(transfer.dataAddress +
				   static_cast<size_t>(framesToCopy) *
					   akai_eie_driver::kPlaybackBytesPerUsbFrame,
			   0,
			   static_cast<size_t>(
				   akai_eie_driver::kPlaybackFramesPerTransfer -
				   framesToCopy) *
				   akai_eie_driver::kPlaybackBytesPerUsbFrame);
		if (__atomic_load_n(
				&ivars->playbackHasReceivedFrames,
				__ATOMIC_ACQUIRE))
		{
			__atomic_add_fetch(
				&ivars->playbackUnderrunFrames,
				akai_eie_driver::kPlaybackFramesPerTransfer -
					framesToCopy,
				__ATOMIC_RELAXED);
		}
	}
	__atomic_add_fetch(
		&ivars->playbackSubmittedFrames,
		framesToCopy,
		__ATOMIC_RELAXED);
	__atomic_store_n(
		&ivars->playbackReadFrame,
		readFrame + framesToCopy,
		__ATOMIC_RELEASE);

	akai_eie::IsoPacket packets[akai_eie_driver::kPlaybackIsoPacketCount]{};
	if (!akai_eie::BuildIsoPacketLayout(
			akai_eie_driver::kPlaybackFramesPerTransfer,
			akai_eie_driver::kPlaybackIsoPacketCount,
			akai_eie_driver::kPlaybackBytesPerUsbFrame,
			packets))
		return kIOReturnBadArgument;

	for (uint32_t packetIndex = 0;
		 packetIndex < akai_eie_driver::kPlaybackIsoPacketCount;
		 ++packetIndex)
	{
		IOUSBIsochronousFrame &frame =
			transfer.frameListAddress[packetIndex];
		frame.status = kIOReturnInvalid;
		frame.requestCount = packets[packetIndex].length_bytes;
		frame.completeCount = 0;
		frame.reserved = 0;
		frame.timeStamp = 0;
	}

	uint64_t currentFrame = 0;
	uint64_t currentTime = 0;
	kern_return_t ret = ivars->playbackInterface->GetFrameNumber(
		&currentFrame, &currentTime);
	if (ret != kIOReturnSuccess)
		return ret;
	if (ivars->nextUsbFrame <
		currentFrame + akai_eie_driver::kPlaybackScheduleLeadBusFrames)
	{
		ivars->nextUsbFrame =
			currentFrame + akai_eie_driver::kPlaybackScheduleLeadBusFrames;
	}

	const uint64_t firstFrame = ivars->nextUsbFrame;
	ivars->nextUsbFrame +=
		akai_eie_driver::kPlaybackTransferDurationBusFrames;
	transfer.submitted = true;
	++ivars->playbackInFlight;
	ret = ivars->playbackPipe->IsochIO(
		transfer.dataBuffer.get(),
		transfer.frameListBuffer.get(),
		firstFrame,
		transfer.completion.get());
	if (ret != kIOReturnSuccess)
	{
		transfer.submitted = false;
		--ivars->playbackInFlight;
	}
	return ret;
}

void AkaiEieDriver::PlaybackComplete_Impl(OSAction *action, IOReturn status)
{
	uint32_t index = akai_eie_driver::kPlaybackTransferCount;
	memcpy(&index, action->GetReference(), sizeof(index));
	if (index >= akai_eie_driver::kPlaybackTransferCount)
		return;

	AkaiEiePlaybackTransfer &transfer = ivars->playbackTransfers[index];
	if (transfer.submitted)
	{
		transfer.submitted = false;
		if (ivars->playbackInFlight > 0)
			--ivars->playbackInFlight;
	}

	if (!__atomic_load_n(&ivars->playbackRunning, __ATOMIC_ACQUIRE))
	{
		if (__atomic_load_n(&ivars->playbackStopPending, __ATOMIC_ACQUIRE) &&
			ivars->playbackInFlight == 0)
		{
			FinishPlaybackStop();
		}
		return;
	}
	if (status != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT,
			   "AkaiEieDriver::PlaybackComplete transfer failed 0x%{public}x",
			   status);
	}
	for (uint32_t packetIndex = 0;
		 packetIndex < akai_eie_driver::kPlaybackIsoPacketCount;
		 ++packetIndex)
	{
		if (transfer.frameListAddress[packetIndex].status != kIOReturnSuccess)
		{
			__atomic_add_fetch(
				&ivars->playbackPacketErrorCount,
				1,
				__ATOMIC_RELAXED);
		}
	}

	const kern_return_t ret = SubmitPlaybackTransfer(index);
	if (ret != kIOReturnSuccess)
	{
		__atomic_store_n(&ivars->playbackRunning, false, __ATOMIC_RELEASE);
		os_log(OS_LOG_DEFAULT,
			   "AkaiEieDriver::PlaybackComplete resubmit failed 0x%{public}x",
			   ret);
	}
}

void AkaiEieDriver::FinishPlaybackStop()
{
	if (!__atomic_load_n(&ivars->playbackStopPending, __ATOMIC_ACQUIRE) ||
		ivars->playbackInFlight != 0)
	{
		return;
	}

	if (ivars->playbackInterface && ivars->playbackInterfaceOpen)
	{
		(void)ivars->playbackInterface->Close(this, 0);
		ivars->playbackInterfaceOpen = false;
	}
	if (ivars->dataInterface && ivars->dataInterfaceOpen)
	{
		(void)ivars->dataInterface->Close(this, 0);
		ivars->dataInterfaceOpen = false;
	}
	ReleasePlaybackResources();
	__atomic_store_n(&ivars->playbackStopPending, false, __ATOMIC_RELEASE);
	os_log(OS_LOG_DEFAULT, "AkaiEieDriver::StopPlayback drained");
}

void AkaiEieDriver::ReleasePlaybackResources()
{
	for (uint32_t index = 0;
		 index < akai_eie_driver::kPlaybackTransferCount;
		 ++index)
	{
		AkaiEiePlaybackTransfer &transfer = ivars->playbackTransfers[index];
		transfer.submitted = false;
		transfer.dataAddress = nullptr;
		transfer.frameListAddress = nullptr;
		transfer.completion.reset();
		transfer.frameListMap.reset();
		transfer.frameListBuffer.reset();
		transfer.dataMap.reset();
		transfer.dataBuffer.reset();
	}

	ivars->playbackPipe.reset();
	ivars->playbackInterface.reset();
	ivars->dataInterface.reset();
	if (ivars->playbackRingAddress != nullptr)
	{
		IOSafeDeleteNULL(
			ivars->playbackRingAddress,
			uint8_t,
			akai_eie_driver::kPlaybackRingBytes);
	}
	ivars->playbackReadFrame = 0;
	ivars->playbackWriteFrame = 0;
	ivars->nextUsbFrame = 0;
	ivars->playbackInFlight = 0;
	ivars->playbackHasReceivedFrames = false;
	ivars->playbackTransfersStarted = false;
}

void AkaiEieDriver::StopPlayback()
{
	if (ivars == nullptr)
		return;

	const bool hadPlaybackResources =
		ivars->playbackRingAddress != nullptr || ivars->playbackPipe ||
		ivars->playbackInterface || ivars->dataInterface ||
		ivars->playbackInterfaceOpen || ivars->dataInterfaceOpen;
	__atomic_store_n(&ivars->playbackRunning, false, __ATOMIC_RELEASE);
	if (!hadPlaybackResources)
		return;
	bool expected = false;
	if (!__atomic_compare_exchange_n(
			&ivars->playbackStopPending,
			&expected,
			true,
			false,
			__ATOMIC_ACQ_REL,
			__ATOMIC_ACQUIRE))
	{
		return;
	}

	os_log(
		OS_LOG_DEFAULT,
		"AkaiEieDriver::StopPlayback enqueued=%{public}llu submitted=%{public}llu underrun=%{public}llu dropped=%{public}llu packetErrors=%{public}llu",
		static_cast<unsigned long long>(
			__atomic_load_n(&ivars->playbackEnqueuedFrames, __ATOMIC_RELAXED)),
		static_cast<unsigned long long>(
			__atomic_load_n(&ivars->playbackSubmittedFrames, __ATOMIC_RELAXED)),
		static_cast<unsigned long long>(
			__atomic_load_n(&ivars->playbackUnderrunFrames, __ATOMIC_RELAXED)),
		static_cast<unsigned long long>(
			__atomic_load_n(&ivars->playbackDroppedFrames, __ATOMIC_RELAXED)),
		static_cast<unsigned long long>(
			__atomic_load_n(&ivars->playbackPacketErrorCount, __ATOMIC_RELAXED)));
	if (ivars->playbackPipe)
	{
		OSSharedPtr<IOUSBHostPipe> playbackPipe = ivars->playbackPipe;
		const kern_return_t abortResult = playbackPipe->Abort(
			kIOUSBAbortAsynchronous,
			kIOReturnAborted,
			nullptr);
		if (abortResult != kIOReturnSuccess)
		{
			os_log(OS_LOG_DEFAULT,
				   "AkaiEieDriver::StopPlayback abort failed 0x%{public}x",
				   abortResult);
		}
	}
	FinishPlaybackStop();
}

kern_return_t IMPL(AkaiEieDriver, Stop)
{
	os_log(OS_LOG_DEFAULT, "AkaiEieDriver::Stop");

	StopPlayback();
	if (ivars->audioDevice)
	{
		os_log(OS_LOG_DEFAULT, "AkaiEieDriver::Stop removing audio device");
		const kern_return_t removeResult =
			RemoveObject(ivars->audioDevice.get());
		if (removeResult != kIOReturnSuccess)
		{
			os_log(OS_LOG_DEFAULT,
				   "AkaiEieDriver::Stop RemoveObject failed 0x%{public}x",
				   removeResult);
		}
	}
	os_log(OS_LOG_DEFAULT, "AkaiEieDriver::Stop closing USB provider");
	CloseUsbProvider();
	os_log(OS_LOG_DEFAULT, "AkaiEieDriver::Stop releasing objects");
	ivars->audioDevice.reset();
	ivars->workQueue.reset();

	os_log(OS_LOG_DEFAULT, "AkaiEieDriver::Stop calling super");
	return Stop(provider, SUPERDISPATCH);
}

void AkaiEieDriver::CloseUsbProvider()
{
	if (ivars == nullptr)
		return;

	if (ivars->usbDevice && ivars->usbDeviceOpen)
	{
		const kern_return_t ret = ivars->usbDevice->Close(this, 0);
		if (ret != kIOReturnSuccess)
		{
			os_log(OS_LOG_DEFAULT,
				   "AkaiEieDriver::CloseUsbProvider Close failed 0x%{public}x",
				   ret);
		}
		ivars->usbDeviceOpen = false;
	}
	ivars->usbDevice.reset();
}

void AkaiEieDriver::free()
{
	if (ivars != nullptr)
	{
		StopPlayback();
		CloseUsbProvider();
		ivars->audioDevice.reset();
		ivars->workQueue.reset();
		ivars->driverName.reset();
		ivars->deviceName.reset();
		ivars->deviceUid.reset();
		ivars->modelUid.reset();
		ivars->manufacturerName.reset();
	}
	IOSafeDeleteNULL(ivars, AkaiEieDriver_IVars, 1);
	super::free();
}
