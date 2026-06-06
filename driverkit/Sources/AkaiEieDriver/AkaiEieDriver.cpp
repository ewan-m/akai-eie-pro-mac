#include <AudioDriverKit/AudioDriverKit.h>
#include <DriverKit/DriverKit.h>
#include <USBDriverKit/AppleUSBDescriptorParsing.h>
#include <USBDriverKit/USBDriverKit.h>
#include <string.h>

#include "AkaiEieDriver.h"
#include "AkaiEieDriverConfig.hpp"
#include "../../../protocol/akai_eie_protocol.hpp"

bool AkaiEieDriver::init()
{
	if (!super::init())
		return false;

	ivars = IONewZero(AkaiEieDriver_IVars, 1);
	if (ivars == nullptr)
		return false;

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
		return false;

	return true;
}

kern_return_t IMPL(AkaiEieDriver, Start)
{
	kern_return_t ret = Start(provider, SUPERDISPATCH);
	if (ret != kIOReturnSuccess)
		return ret;

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
		ivars->audioDevice.reset();
		CloseUsbProvider();
		return ret;
	}

	return kIOReturnSuccess;
}

kern_return_t AkaiEieDriver::CaptureUsbProvider(IOService *provider)
{
	IOUSBHostDevice *usbDevice = OSDynamicCast(IOUSBHostDevice, provider);
	if (usbDevice == nullptr)
		return kIOReturnBadArgument;

	ivars->usbDevice = OSSharedPtr<IOUSBHostDevice>(usbDevice, OSRetain);

	const IOUSBDeviceDescriptor *descriptor =
		ivars->usbDevice->CopyDeviceDescriptor();
	if (descriptor == nullptr)
		return kIOReturnNoDevice;

	const uint16_t vendorId = descriptor->idVendor;
	const uint16_t productId = descriptor->idProduct;
	IOUSBHostFreeDescriptor(descriptor);

	if (vendorId != akai_eie::kVendorId || productId != akai_eie::kProductId)
		return kIOReturnUnsupported;

	kern_return_t ret = ivars->usbDevice->Open(this, 0, 0);
	if (ret != kIOReturnSuccess)
		return ret;
	ivars->usbDeviceOpen = true;

	ret = ivars->usbDevice->SetConfiguration(
		akai_eie_driver::kUsbConfigurationValue,
		true);
	if (ret != kIOReturnSuccess)
	{
		CloseUsbProvider();
		return ret;
	}

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
	return ret;
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
		return ret;

	ret = super::StartDevice(objectId, flags);
	if (ret != kIOReturnSuccess)
		StopPlayback();
	return ret;
}

kern_return_t AkaiEieDriver::StartAudioClock()
{
	if (!ivars || !ivars->audioDevice ||
		!__atomic_load_n(&ivars->playbackRunning, __ATOMIC_ACQUIRE))
	{
		return kIOReturnNotReady;
	}

	const uint64_t feedbackFrames =
		__atomic_load_n(&ivars->syncFeedbackFrames, __ATOMIC_ACQUIRE);
	const uint64_t hostTime =
		__atomic_load_n(&ivars->syncLastHostTime, __ATOMIC_ACQUIRE);
	if (feedbackFrames == 0 || hostTime == 0)
		return kIOReturnNotReady;

	__atomic_store_n(
		&ivars->syncClockBaseFrames, feedbackFrames, __ATOMIC_RELEASE);
	ivars->playbackTimestampFrames = 0;
	ivars->audioDevice->UpdatePlaybackTimestamp(0, hostTime);
	(void)ivars->audioDevice->SetClockIsStable(true);
	return kIOReturnSuccess;
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

kern_return_t AkaiEieDriver::OpenSyncEndpoint()
{
	if (!ivars->usbDevice)
		return kIOReturnNoDevice;

	// Interface 1 carries device-clock feedback and the bulk capture stream.
	uintptr_t iterator = 0;
	kern_return_t ret = ivars->usbDevice->CreateInterfaceIterator(&iterator);
	if (ret != kIOReturnSuccess)
		return ret;

	OSSharedPtr<IOUSBHostInterface> dataInterface;
	while (true)
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

		if (interfaceNumber == akai_eie_driver::kDataInterfaceNumber)
		{
			dataInterface = candidate;
			break;
		}
	}
	(void)ivars->usbDevice->DestroyInterfaceIterator(iterator);

	if (!dataInterface)
		return kIOReturnNoDevice;

	ret = dataInterface->Open(this, 0, nullptr);
	if (ret != kIOReturnSuccess)
		return ret;

	ret = dataInterface->SelectAlternateSetting(
		akai_eie_driver::kPlaybackAlternateSetting);
	if (ret != kIOReturnSuccess)
	{
		dataInterface->Close(this, 0);
		return ret;
	}

	const IOUSBConfigurationDescriptor *configDesc =
		dataInterface->CopyConfigurationDescriptor();
	if (configDesc == nullptr)
	{
		dataInterface->Close(this, 0);
		return kIOReturnNoDevice;
	}

	const IOUSBInterfaceDescriptor *targetInterface = nullptr;
	const IOUSBInterfaceDescriptor *interfaceDescriptor = nullptr;
	while ((interfaceDescriptor = IOUSBGetNextInterfaceDescriptor(
				configDesc,
				reinterpret_cast<const IOUSBDescriptorHeader *>(
					interfaceDescriptor))) != nullptr)
	{
		if (interfaceDescriptor->bInterfaceNumber ==
				akai_eie_driver::kDataInterfaceNumber &&
			interfaceDescriptor->bAlternateSetting ==
				akai_eie_driver::kPlaybackAlternateSetting)
		{
			targetInterface = interfaceDescriptor;
			break;
		}
	}
	if (targetInterface == nullptr)
	{
		IOUSBHostFreeDescriptor(configDesc);
		dataInterface->Close(this, 0);
		return kIOReturnNotFound;
	}

	uint8_t usbSpeed = 0;
	ret = ivars->usbDevice->GetSpeed(&usbSpeed);
	if (ret != kIOReturnSuccess)
	{
		IOUSBHostFreeDescriptor(configDesc);
		dataInterface->Close(this, 0);
		return ret;
	}

	uint8_t syncAddress = 0;
	uint16_t syncMaxPacket = 0;
	uint32_t syncIntervalMicroframes = 0;
	uint8_t captureAddress = 0;
	uint16_t captureMaxPacket = 0;
	const IOUSBEndpointDescriptor *endpointDescriptor = nullptr;
	while ((endpointDescriptor = IOUSBGetNextEndpointDescriptor(
				configDesc,
				targetInterface,
				reinterpret_cast<const IOUSBDescriptorHeader *>(
					endpointDescriptor))) != nullptr)
	{
		const uint8_t endpointAddress =
			IOUSBGetEndpointAddress(endpointDescriptor);
		const uint16_t maxPacket =
			IOUSBGetEndpointMaxPacketSize(usbSpeed, endpointDescriptor);
		const uint32_t intervalMicroframes =
			IOUSBGetEndpointIntervalMicroframes(usbSpeed, endpointDescriptor);
		const bool isIsochronous =
			IOUSBGetEndpointType(endpointDescriptor) ==
			kIOUSBEndpointTypeIsochronous;
		const bool isBulk =
			IOUSBGetEndpointType(endpointDescriptor) ==
			kIOUSBEndpointTypeBulk;
		const bool isIn =
			IOUSBGetEndpointDirection(endpointDescriptor) ==
			kIOUSBEndpointDirectionIn;

		if (isIsochronous && isIn &&
			maxPacket <= akai_eie_driver::kSyncMaxPacketBytes)
		{
			syncAddress = endpointAddress;
			syncMaxPacket = maxPacket;
			syncIntervalMicroframes = intervalMicroframes;
		}
		if (isBulk && isIn &&
			endpointAddress == akai_eie::kKnownCaptureEndpointAddress)
		{
			captureAddress = endpointAddress;
			captureMaxPacket = maxPacket;
		}
	}

	IOUSBHostFreeDescriptor(configDesc);

	if (syncAddress == 0)
	{
		dataInterface->Close(this, 0);
		return kIOReturnNotFound;
	}
	if (captureAddress == 0)
	{
		dataInterface->Close(this, 0);
		return kIOReturnNotFound;
	}

	const uint32_t playbackPeriodMicroframes =
		ivars->playbackIntervalMicroframes *
		akai_eie_driver::kPlaybackIsoPacketCount;
	if (syncIntervalMicroframes == 0 ||
		playbackPeriodMicroframes == 0 ||
		playbackPeriodMicroframes % syncIntervalMicroframes != 0)
	{
		dataInterface->Close(this, 0);
		return kIOReturnUnsupported;
	}
	const uint32_t syncReportsPerPlaybackPeriod =
		playbackPeriodMicroframes / syncIntervalMicroframes;
	if (syncReportsPerPlaybackPeriod == 0 ||
		syncReportsPerPlaybackPeriod >
			akai_eie_driver::kSyncMaxReportsPerPlaybackPeriod)
	{
		dataInterface->Close(this, 0);
		return kIOReturnUnsupported;
	}

	ret = dataInterface->CopyPipe(syncAddress, ivars->syncPipe.attach());
	if (ret != kIOReturnSuccess || !ivars->syncPipe)
	{
		dataInterface->Close(this, 0);
		return ret != kIOReturnSuccess ? ret : kIOReturnNoDevice;
	}
	ret = dataInterface->CopyPipe(captureAddress, ivars->capturePipe.attach());
	if (ret != kIOReturnSuccess || !ivars->capturePipe)
	{
		ivars->syncPipe.reset();
		dataInterface->Close(this, 0);
		return ret != kIOReturnSuccess ? ret : kIOReturnNoDevice;
	}

	ivars->dataInterface = dataInterface;
	ivars->dataInterfaceOpen = true;
	ivars->syncEndpointAddress = syncAddress;
	ivars->syncMaxPacketSize = syncMaxPacket;
	ivars->syncIntervalMicroframes = syncIntervalMicroframes;
	ivars->syncReportsPerPlaybackPeriod = syncReportsPerPlaybackPeriod;
	ivars->captureEndpointAddress = captureAddress;
	ivars->captureMaxPacketSize = captureMaxPacket;

	return kIOReturnSuccess;
}

kern_return_t AkaiEieDriver::CreateSyncTransfers()
{
	if (!ivars->dataInterface || !ivars->syncPipe ||
		ivars->syncMaxPacketSize == 0 ||
		ivars->syncReportsPerPlaybackPeriod == 0)
	{
		return kIOReturnNotReady;
	}

	const uint32_t frameListBytes =
		static_cast<uint32_t>(sizeof(IOUSBIsochronousFrame)) *
		akai_eie_driver::kSyncPacketsPerTransfer;
	const uint32_t dataBytes =
		ivars->syncMaxPacketSize * akai_eie_driver::kSyncPacketsPerTransfer;

	for (uint32_t index = 0;
		 index < akai_eie_driver::kSyncTransferCount;
		 ++index)
	{
		AkaiEieSyncTransfer &transfer = ivars->syncTransfers[index];

		kern_return_t ret = ivars->dataInterface->CreateIOBuffer(
			kIOMemoryDirectionIn,
			dataBytes,
			transfer.dataBuffer.attach());
		if (ret != kIOReturnSuccess)
			return ret;
		ret = transfer.dataBuffer->SetLength(dataBytes);
		if (ret != kIOReturnSuccess)
			return ret;
		ret = transfer.dataBuffer->CreateMapping(
			0, 0, 0, 0, 0, transfer.dataMap.attach());
		if (ret != kIOReturnSuccess)
			return ret;
		transfer.dataAddress = reinterpret_cast<uint8_t *>(
			transfer.dataMap->GetAddress() + transfer.dataMap->GetOffset());

		ret = ivars->dataInterface->CreateIOBuffer(
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

		ret = CreateActionSyncComplete(
			sizeof(index), transfer.completion.attach());
		if (ret != kIOReturnSuccess)
			return ret;
		memcpy(transfer.completion->GetReference(), &index, sizeof(index));
	}

	return kIOReturnSuccess;
}

kern_return_t AkaiEieDriver::CreateCaptureTransfers()
{
	if (!ivars->dataInterface || !ivars->capturePipe ||
		ivars->captureMaxPacketSize == 0)
	{
		return kIOReturnNotReady;
	}

	for (uint32_t index = 0;
		 index < akai_eie_driver::kCaptureTransferCount;
		 ++index)
	{
		AkaiEieCaptureTransfer &transfer = ivars->captureTransfers[index];

		kern_return_t ret = ivars->dataInterface->CreateIOBuffer(
			kIOMemoryDirectionIn,
			akai_eie_driver::kCaptureTransferBytes,
			transfer.dataBuffer.attach());
		if (ret != kIOReturnSuccess)
			return ret;
		ret = transfer.dataBuffer->SetLength(
			akai_eie_driver::kCaptureTransferBytes);
		if (ret != kIOReturnSuccess)
			return ret;
		ret = transfer.dataBuffer->CreateMapping(
			0, 0, 0, 0, 0, transfer.dataMap.attach());
		if (ret != kIOReturnSuccess)
			return ret;
		transfer.dataAddress = reinterpret_cast<uint8_t *>(
			transfer.dataMap->GetAddress() + transfer.dataMap->GetOffset());

		ret = CreateActionCaptureComplete(
			sizeof(index), transfer.completion.attach());
		if (ret != kIOReturnSuccess)
			return ret;
		memcpy(transfer.completion->GetReference(), &index, sizeof(index));
	}

	return kIOReturnSuccess;
}

kern_return_t AkaiEieDriver::SubmitSyncTransfer(uint32_t index)
{
	if (index >= akai_eie_driver::kSyncTransferCount ||
		!ivars->dataInterface ||
		!ivars->syncPipe ||
		!__atomic_load_n(&ivars->playbackRunning, __ATOMIC_ACQUIRE))
	{
		return kIOReturnNotReady;
	}

	AkaiEieSyncTransfer &transfer = ivars->syncTransfers[index];

	for (uint32_t packetIndex = 0;
		 packetIndex < akai_eie_driver::kSyncPacketsPerTransfer;
		 ++packetIndex)
	{
		IOUSBIsochronousFrame &frame =
			transfer.frameListAddress[packetIndex];
		frame.status = kIOReturnInvalid;
		frame.requestCount = ivars->syncMaxPacketSize;
		frame.completeCount = 0;
		frame.reserved = 0;
		frame.timeStamp = 0;
	}

	memset(transfer.dataAddress, 0,
		   ivars->syncMaxPacketSize *
			   akai_eie_driver::kSyncPacketsPerTransfer);

	transfer.submitted = true;
	++ivars->syncInFlight;
	const kern_return_t ret = ivars->syncPipe->IsochIO(
		transfer.dataBuffer.get(),
		transfer.frameListBuffer.get(),
		0,
		transfer.completion.get());
	if (ret != kIOReturnSuccess)
	{
		transfer.submitted = false;
		--ivars->syncInFlight;
	}
	return ret;
}

kern_return_t AkaiEieDriver::SubmitCaptureTransfer(uint32_t index)
{
	if (index >= akai_eie_driver::kCaptureTransferCount ||
		!ivars->capturePipe ||
		!__atomic_load_n(&ivars->playbackRunning, __ATOMIC_ACQUIRE) ||
		!__atomic_load_n(&ivars->captureRunning, __ATOMIC_ACQUIRE))
	{
		return kIOReturnNotReady;
	}

	AkaiEieCaptureTransfer &transfer = ivars->captureTransfers[index];
	memset(transfer.dataAddress, 0, akai_eie_driver::kCaptureTransferBytes);
	transfer.submitted = true;
	++ivars->captureInFlight;
	const kern_return_t ret = ivars->capturePipe->AsyncIO(
		transfer.dataBuffer.get(),
		akai_eie_driver::kCaptureTransferBytes,
		transfer.completion.get(),
		0);
	if (ret != kIOReturnSuccess)
	{
		transfer.submitted = false;
		--ivars->captureInFlight;
	}
	return ret;
}

kern_return_t AkaiEieDriver::ValidateSyncClock()
{
	if (!ivars->syncPipe || ivars->syncReportsPerPlaybackPeriod == 0)
		return kIOReturnNotReady;

	AkaiEieSyncTransfer &transfer = ivars->syncTransfers[0];
	uint64_t lastHostTime = 0;
	for (uint32_t attempt = 0;
		 attempt < akai_eie_driver::kSyncPrimeAttemptCount;
		 ++attempt)
	{
		for (uint32_t packetIndex = 0;
			 packetIndex < akai_eie_driver::kSyncPacketsPerTransfer;
			 ++packetIndex)
		{
			IOUSBIsochronousFrame &frame =
				transfer.frameListAddress[packetIndex];
			frame.status = kIOReturnInvalid;
			frame.requestCount = ivars->syncMaxPacketSize;
			frame.completeCount = 0;
			frame.reserved = 0;
			frame.timeStamp = 0;
		}
		memset(transfer.dataAddress, 0,
			   ivars->syncMaxPacketSize *
				   akai_eie_driver::kSyncPacketsPerTransfer);

		const kern_return_t ret = ivars->syncPipe->IsochIO(
			transfer.dataBuffer.get(), transfer.frameListBuffer.get(), 0, nullptr);
		if (ret != kIOReturnSuccess)
			return ret;

		const IOUSBIsochronousFrame &frame = transfer.frameListAddress[0];
		if (frame.status != kIOReturnSuccess)
			continue;
		if (frame.completeCount == 0)
			continue;

		const uint8_t feedback = transfer.dataAddress[0];
		lastHostTime = frame.timeStamp;
		if (!akai_eie::IsReasonableSyncFeedback(feedback))
			continue;

		if (lastHostTime != 0)
		{
			__atomic_store_n(
				&ivars->syncFeedbackFrames,
				feedback,
				__ATOMIC_RELEASE);
			__atomic_store_n(
				&ivars->syncLastHostTime, lastHostTime, __ATOMIC_RELEASE);
			__atomic_store_n(
				&ivars->syncFramesElapsed,
				0,
				__ATOMIC_RELEASE);
			return kIOReturnSuccess;
		}
	}

	return kIOReturnNotResponding;
}

kern_return_t AkaiEieDriver::StartSyncTransfers()
{
	for (uint32_t index = 0;
		 index < akai_eie_driver::kSyncTransferCount;
		 ++index)
	{
		const kern_return_t ret = SubmitSyncTransfer(index);
		if (ret != kIOReturnSuccess)
			return ret;
	}

	return kIOReturnSuccess;
}

kern_return_t AkaiEieDriver::StartCaptureTransfers()
{
	for (uint32_t index = 0;
		 index < akai_eie_driver::kCaptureTransferCount;
		 ++index)
	{
		const kern_return_t ret = SubmitCaptureTransfer(index);
		if (ret != kIOReturnSuccess)
			return ret;
	}

	return kIOReturnSuccess;
}

kern_return_t AkaiEieDriver::OpenPlaybackInterface()
{
	if (!ivars->usbDevice)
		return kIOReturnNoDevice;

	uintptr_t iterator = 0;
	kern_return_t ret = ivars->usbDevice->CreateInterfaceIterator(&iterator);
	if (ret != kIOReturnSuccess)
		return ret;

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

		if (interfaceNumber == akai_eie_driver::kPlaybackInterfaceNumber)
			ivars->playbackInterface = candidate;
	}
	(void)ivars->usbDevice->DestroyInterfaceIterator(iterator);

	if (!ivars->playbackInterface)
		return kIOReturnNoDevice;

	ret = ivars->playbackInterface->Open(this, 0, nullptr);
	if (ret != kIOReturnSuccess)
		return ret;
	ivars->playbackInterfaceOpen = true;

	ret = ivars->playbackInterface->SelectAlternateSetting(
		akai_eie_driver::kPlaybackAlternateSetting);
	if (ret != kIOReturnSuccess)
		return ret;

	ret = ivars->playbackInterface->CopyPipe(
		akai_eie_driver::kPlaybackEndpointAddress,
		ivars->playbackPipe.attach());
	if (ret != kIOReturnSuccess || !ivars->playbackPipe)
		return ret != kIOReturnSuccess ? ret : kIOReturnNoDevice;

	IOUSBStandardEndpointDescriptors endpointDescriptors{};
	ret = ivars->playbackPipe->GetDescriptors(
		&endpointDescriptors, kIOUSBGetEndpointDescriptorCurrentPolicy);
	if (ret != kIOReturnSuccess)
		return ret;
	if (endpointDescriptors.descriptor.bEndpointAddress !=
		akai_eie_driver::kPlaybackEndpointAddress)
		return kIOReturnUnsupported;

	uint8_t usbSpeed = 0;
	ret = ivars->usbDevice->GetSpeed(&usbSpeed);
	if (ret != kIOReturnSuccess)
		return ret;
	ivars->playbackMaxPacketSize = IOUSBGetEndpointMaxPacketSize(
		usbSpeed, &endpointDescriptors.descriptor);
	ivars->playbackIntervalMicroframes = IOUSBGetEndpointIntervalMicroframes(
		usbSpeed, &endpointDescriptors.descriptor);
	const uint32_t playbackPeriodMicroframes =
		ivars->playbackIntervalMicroframes *
		akai_eie_driver::kPlaybackIsoPacketCount;
	if (usbSpeed != kIOUSBHostConnectionSpeedHigh ||
		ivars->playbackMaxPacketSize == 0 ||
		playbackPeriodMicroframes !=
			akai_eie_driver::kPlaybackTransferDurationMicroframes)
	{
		return kIOReturnUnsupported;
	}

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
			return ret;
	}

	return kIOReturnSuccess;
}

kern_return_t AkaiEieDriver::CreatePlaybackTransfers()
{
	if (ivars->playbackMaxPacketSize == 0)
		return kIOReturnNotReady;

	const uint32_t frameListBytes =
		static_cast<uint32_t>(sizeof(IOUSBIsochronousFrame)) *
		akai_eie_driver::kPlaybackIsoPacketCount;
	const uint32_t dataBytes =
		ivars->playbackMaxPacketSize *
		akai_eie_driver::kPlaybackIsoPacketCount;

	for (uint32_t index = 0;
		 index < akai_eie_driver::kPlaybackTransferCount;
		 ++index)
	{
		AkaiEiePlaybackTransfer &transfer = ivars->playbackTransfers[index];
		kern_return_t ret = ivars->playbackInterface->CreateIOBuffer(
			kIOMemoryDirectionInOut,
			dataBytes,
			transfer.dataBuffer.attach());
		if (ret != kIOReturnSuccess)
			return ret;
		ret = transfer.dataBuffer->SetLength(dataBytes);
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
	ivars->captureRingAddress = IONewZero(
		uint8_t, akai_eie_driver::kCaptureRingBytes);
	if (ivars->captureRingAddress == nullptr)
	{
		StopPlayback();
		return kIOReturnNoMemory;
	}
	__atomic_store_n(&ivars->playbackReadFrame, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&ivars->playbackWriteFrame, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&ivars->captureReadFrame, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&ivars->captureWriteFrame, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&ivars->syncFeedbackFrames, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&ivars->syncClockBaseFrames, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&ivars->syncLastHostTime, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&ivars->syncFramesElapsed, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&ivars->playbackHasReceivedFrames, false, __ATOMIC_RELAXED);
	__atomic_store_n(&ivars->playbackTransfersStarted, false, __ATOMIC_RELAXED);
	__atomic_store_n(&ivars->captureRunning, false, __ATOMIC_RELAXED);
	ivars->playbackTimingState = {};
	ivars->playbackTimestampFrames = 0;

	kern_return_t ret = OpenPlaybackInterface();
	if (ret == kIOReturnSuccess)
		ret = OpenSyncEndpoint();
	if (ret == kIOReturnSuccess)
		ret = SendPlaybackSetup();
	if (ret == kIOReturnSuccess)
		ret = CreatePlaybackTransfers();
	if (ret == kIOReturnSuccess)
		ret = CreateSyncTransfers();
	if (ret == kIOReturnSuccess)
		ret = CreateCaptureTransfers();

	if (ret != kIOReturnSuccess)
	{
		StopPlayback();
		return ret;
	}

	ivars->nextUsbFrame = 0;
	ivars->playbackInFlight = 0;
	ivars->syncInFlight = 0;
	ivars->captureInFlight = 0;
	__atomic_store_n(&ivars->playbackRunning, true, __ATOMIC_RELEASE);
	__atomic_store_n(&ivars->captureRunning, true, __ATOMIC_RELEASE);

	ret = StartPlaybackTransfers();
	if (ret != kIOReturnSuccess)
	{
		StopPlayback();
		return ret;
	}
	ret = StartCaptureTransfers();
	if (ret != kIOReturnSuccess)
	{
		StopPlayback();
		return ret;
	}

	return kIOReturnSuccess;
}

kern_return_t AkaiEieDriver::EnqueuePlaybackFrames(const float *frames,
												   uint32_t frameCount)
{
	if (frames == nullptr || frameCount == 0 ||
		ivars->playbackRingAddress == nullptr ||
		!__atomic_load_n(&ivars->playbackRunning, __ATOMIC_ACQUIRE))
		return kIOReturnNotReady;

	if (frameCount > akai_eie_driver::kPlaybackRingFrameCount)
		return kIOReturnOverrun;

	const uint64_t writeFrame =
		__atomic_load_n(&ivars->playbackWriteFrame, __ATOMIC_RELAXED);
	const uint64_t readFrame =
		__atomic_load_n(&ivars->playbackReadFrame, __ATOMIC_ACQUIRE);
	if (writeFrame + frameCount - readFrame >
		akai_eie_driver::kPlaybackRingFrameCount)
		return kIOReturnOverrun;

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
	__atomic_store_n(
		&ivars->playbackHasReceivedFrames,
		true,
		__ATOMIC_RELEASE);
	return kIOReturnSuccess;
}

kern_return_t AkaiEieDriver::StoreCaptureBytes(const uint8_t *bytes,
											   uint32_t byteCount)
{
	if (bytes == nullptr ||
		ivars->captureRingAddress == nullptr)
	{
		return kIOReturnNotReady;
	}

	byteCount -= byteCount % akai_eie_driver::kCaptureBytesPerUsbFrame;
	uint32_t frameCount =
		byteCount / akai_eie_driver::kCaptureBytesPerUsbFrame;
	if (frameCount == 0)
		return kIOReturnSuccess;

	if (frameCount > akai_eie_driver::kCaptureRingFrameCount)
	{
		const uint32_t skippedFrames =
			frameCount - akai_eie_driver::kCaptureRingFrameCount;
		bytes += static_cast<size_t>(skippedFrames) *
				 akai_eie_driver::kCaptureBytesPerUsbFrame;
		frameCount = akai_eie_driver::kCaptureRingFrameCount;
	}

	const uint64_t writeFrame =
		__atomic_load_n(&ivars->captureWriteFrame, __ATOMIC_RELAXED);
	const uint64_t minimumReadFrame =
		writeFrame + frameCount > akai_eie_driver::kCaptureRingFrameCount
			? writeFrame + frameCount -
				  akai_eie_driver::kCaptureRingFrameCount
			: 0;
	uint64_t readFrame =
		__atomic_load_n(&ivars->captureReadFrame, __ATOMIC_ACQUIRE);
	while (readFrame < minimumReadFrame &&
		   !__atomic_compare_exchange_n(
			   &ivars->captureReadFrame,
			   &readFrame,
			   minimumReadFrame,
			   false,
			   __ATOMIC_RELEASE,
			   __ATOMIC_ACQUIRE))
	{
	}

	uint32_t decodedFrames = 0;
	while (decodedFrames < frameCount)
	{
		const uint64_t targetFrame = writeFrame + decodedFrames;
		const uint32_t firstRingFrame = static_cast<uint32_t>(
			targetFrame % akai_eie_driver::kCaptureRingFrameCount);
		const uint32_t framesLeft = frameCount - decodedFrames;
		const uint32_t contiguousFrameCount =
			framesLeft <=
					akai_eie_driver::kCaptureRingFrameCount - firstRingFrame
				? framesLeft
				: akai_eie_driver::kCaptureRingFrameCount - firstRingFrame;
		const size_t writtenFrames = akai_eie::DecodeCaptureBufferToFloat32(
			bytes + static_cast<size_t>(decodedFrames) *
						akai_eie_driver::kCaptureBytesPerUsbFrame,
			static_cast<size_t>(contiguousFrameCount) *
				akai_eie_driver::kCaptureBytesPerUsbFrame,
			reinterpret_cast<float *>(
				ivars->captureRingAddress +
				firstRingFrame * akai_eie_driver::kBytesPerFrame),
			contiguousFrameCount);
		if (writtenFrames == 0)
			return kIOReturnError;
		decodedFrames += static_cast<uint32_t>(writtenFrames);
	}

	if (decodedFrames == 0)
		return kIOReturnUnderrun;

	__atomic_store_n(
		&ivars->captureWriteFrame,
		writeFrame + decodedFrames,
		__ATOMIC_RELEASE);
	return kIOReturnSuccess;
}

kern_return_t AkaiEieDriver::CopyCaptureFrames(float *frames, uint32_t frameCount)
{
	if (frames == nullptr || frameCount == 0)
		return kIOReturnBadArgument;

	if (ivars->captureRingAddress == nullptr ||
		!__atomic_load_n(&ivars->captureRunning, __ATOMIC_ACQUIRE))
		return kIOReturnNotReady;

	const uint64_t readFrame =
		__atomic_load_n(&ivars->captureReadFrame, __ATOMIC_RELAXED);
	const uint64_t writeFrame =
		__atomic_load_n(&ivars->captureWriteFrame, __ATOMIC_ACQUIRE);
	const uint64_t availableFrames =
		writeFrame > readFrame ? writeFrame - readFrame : 0;
	const uint32_t framesToCopy =
		availableFrames < frameCount
			? static_cast<uint32_t>(availableFrames)
			: frameCount;

	uint32_t copiedFrames = 0;
	while (copiedFrames < framesToCopy)
	{
		const uint64_t sourceFrame = readFrame + copiedFrames;
		const uint32_t firstRingFrame = static_cast<uint32_t>(
			sourceFrame % akai_eie_driver::kCaptureRingFrameCount);
		const uint32_t framesLeft = framesToCopy - copiedFrames;
		const uint32_t contiguousFrameCount =
			framesLeft <=
					akai_eie_driver::kCaptureRingFrameCount - firstRingFrame
				? framesLeft
				: akai_eie_driver::kCaptureRingFrameCount - firstRingFrame;
		memcpy(
			frames + static_cast<size_t>(copiedFrames) *
						 akai_eie_driver::kChannelCount,
			ivars->captureRingAddress +
				firstRingFrame * akai_eie_driver::kBytesPerFrame,
			static_cast<size_t>(contiguousFrameCount) *
				akai_eie_driver::kBytesPerFrame);
		copiedFrames += contiguousFrameCount;
	}
	if (copiedFrames < frameCount)
	{
		memset(
			frames + static_cast<size_t>(copiedFrames) *
						 akai_eie_driver::kChannelCount,
			0,
			static_cast<size_t>(frameCount - copiedFrames) *
				akai_eie_driver::kBytesPerFrame);
	}

	__atomic_store_n(
		&ivars->captureReadFrame,
		readFrame + copiedFrames,
		__ATOMIC_RELEASE);
	return kIOReturnSuccess;
}

kern_return_t AkaiEieDriver::StartPlaybackTransfers()
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
	if (ret != kIOReturnSuccess)
		return ret;
	if (!ivars->syncPipe)
		return kIOReturnNotReady;

	ivars->nextUsbFrame =
		currentFrame + akai_eie_driver::kPlaybackScheduleLeadBusFrames;
	for (uint32_t index = 0;
		 index < akai_eie_driver::kPlaybackTransferCount;
		 ++index)
	{
		ret = SubmitPlaybackTransfer(index);
		if (ret != kIOReturnSuccess)
			return ret;
	}

	ret = ValidateSyncClock();
	if (ret != kIOReturnSuccess)
		return ret;
	ret = StartSyncTransfers();
	if (ret != kIOReturnSuccess)
		return ret;

	return kIOReturnSuccess;
}

kern_return_t AkaiEieDriver::SubmitPlaybackTransfer(uint32_t index)
{
	if (index >= akai_eie_driver::kPlaybackTransferCount ||
		!ivars->playbackPipe ||
		!__atomic_load_n(&ivars->playbackRunning, __ATOMIC_ACQUIRE))
		return kIOReturnNotReady;

	const uint32_t feedbackFrames =
		__atomic_exchange_n(&ivars->syncFramesElapsed, 0, __ATOMIC_ACQ_REL);
	const uint32_t framesWanted = akai_eie::CalculatePlaybackFramesWanted(
		akai_eie_driver::kDefaultSampleRateHz,
		ivars->playbackTimingState,
		feedbackFrames);
	const uint32_t transferBytes =
		ivars->playbackMaxPacketSize *
		akai_eie_driver::kPlaybackIsoPacketCount;
	if (framesWanted * akai_eie_driver::kPlaybackBytesPerUsbFrame >
		transferBytes)
		return kIOReturnBadArgument;

	akai_eie::IsoPacket packets[akai_eie_driver::kPlaybackIsoPacketCount]{};
	if (!akai_eie::BuildIsoPacketLayout(
			framesWanted,
			akai_eie_driver::kPlaybackIsoPacketCount,
			akai_eie_driver::kPlaybackBytesPerUsbFrame,
			packets))
	{
		return kIOReturnBadArgument;
	}
	for (uint32_t packetIndex = 0;
		 packetIndex < akai_eie_driver::kPlaybackIsoPacketCount;
		 ++packetIndex)
	{
		if (packets[packetIndex].length_bytes > ivars->playbackMaxPacketSize)
			return kIOReturnBadArgument;
	}

	AkaiEiePlaybackTransfer &transfer = ivars->playbackTransfers[index];
	const uint64_t readFrame =
		__atomic_load_n(&ivars->playbackReadFrame, __ATOMIC_RELAXED);
	const uint64_t writeFrame =
		__atomic_load_n(&ivars->playbackWriteFrame, __ATOMIC_ACQUIRE);
	const uint64_t availableFrames =
		writeFrame > readFrame ? writeFrame - readFrame : 0;
	const uint32_t framesToCopy =
		availableFrames < framesWanted
			? static_cast<uint32_t>(availableFrames)
			: framesWanted;
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
	if (framesToCopy < framesWanted)
	{
		memset(transfer.dataAddress +
				   static_cast<size_t>(framesToCopy) *
					   akai_eie_driver::kPlaybackBytesPerUsbFrame,
			   0,
			   static_cast<size_t>(
				   framesWanted -
				   framesToCopy) *
				   akai_eie_driver::kPlaybackBytesPerUsbFrame);
	}
	__atomic_store_n(
		&ivars->playbackReadFrame,
		readFrame + framesToCopy,
		__ATOMIC_RELEASE);

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
		FinishPlaybackStop();
		return;
	}
	if (status != kIOReturnSuccess)
	{
		FailPlaybackStream(status);
		return;
	}
	for (uint32_t packetIndex = 0;
		 packetIndex < akai_eie_driver::kPlaybackIsoPacketCount;
		 ++packetIndex)
	{
		const IOUSBIsochronousFrame &frame =
			transfer.frameListAddress[packetIndex];
		if (frame.status != kIOReturnSuccess)
		{
			FailPlaybackStream(frame.status);
			return;
		}
	}

	const kern_return_t ret = SubmitPlaybackTransfer(index);
	if (ret != kIOReturnSuccess)
		FailPlaybackStream(ret);
}

void AkaiEieDriver::SyncComplete_Impl(OSAction *action, IOReturn status)
{
	uint32_t index = akai_eie_driver::kSyncTransferCount;
	memcpy(&index, action->GetReference(), sizeof(index));
	if (index >= akai_eie_driver::kSyncTransferCount)
		return;

	AkaiEieSyncTransfer &transfer = ivars->syncTransfers[index];
	if (transfer.submitted)
	{
		transfer.submitted = false;
		if (ivars->syncInFlight > 0)
			--ivars->syncInFlight;
	}

	if (!__atomic_load_n(&ivars->playbackRunning, __ATOMIC_ACQUIRE))
	{
		FinishPlaybackStop();
		return;
	}

	if (status != kIOReturnSuccess)
	{
		FailPlaybackStream(status);
		return;
	}

	const IOUSBIsochronousFrame &frame = transfer.frameListAddress[0];
	if (frame.status == kIOReturnSuccess && frame.completeCount != 0)
	{
		const uint8_t feedback = transfer.dataAddress[0];
		if (akai_eie::IsReasonableSyncFeedback(feedback))
		{
			const uint64_t totalFeedbackFrames = __atomic_add_fetch(
				&ivars->syncFeedbackFrames, feedback, __ATOMIC_ACQ_REL);
			__atomic_add_fetch(
				&ivars->syncFramesElapsed, feedback, __ATOMIC_ACQ_REL);
			__atomic_store_n(
				&ivars->syncLastHostTime,
				frame.timeStamp,
				__ATOMIC_RELEASE);

			const uint64_t clockBaseFrames = __atomic_load_n(
				&ivars->syncClockBaseFrames, __ATOMIC_ACQUIRE);
			const uint64_t clockFrames =
				totalFeedbackFrames >= clockBaseFrames
					? totalFeedbackFrames - clockBaseFrames
					: 0;
			const uint32_t timestampPeriod =
				akai_eie_driver::ZeroTimestampPeriod(
					akai_eie_driver::kDefaultSampleRateHz);
			const uint64_t nextTimestampFrame =
				ivars->playbackTimestampFrames + timestampPeriod;
			if (clockFrames >= nextTimestampFrame && ivars->audioDevice)
			{
				ivars->playbackTimestampFrames = nextTimestampFrame;
				ivars->audioDevice->UpdatePlaybackTimestamp(
					nextTimestampFrame, frame.timeStamp);
			}
		}
	}

	const kern_return_t ret = SubmitSyncTransfer(index);
	if (ret != kIOReturnSuccess)
		FailPlaybackStream(ret);
}

void AkaiEieDriver::CaptureComplete_Impl(OSAction *action,
										 IOReturn status,
										 uint32_t actualByteCount,
										 uint64_t completionTimestamp)
{
	(void)completionTimestamp;

	uint32_t index = akai_eie_driver::kCaptureTransferCount;
	memcpy(&index, action->GetReference(), sizeof(index));
	if (index >= akai_eie_driver::kCaptureTransferCount)
		return;

	AkaiEieCaptureTransfer &transfer = ivars->captureTransfers[index];
	if (transfer.submitted)
	{
		transfer.submitted = false;
		if (ivars->captureInFlight > 0)
			--ivars->captureInFlight;
	}

	if (!__atomic_load_n(&ivars->playbackRunning, __ATOMIC_ACQUIRE) ||
		!__atomic_load_n(&ivars->captureRunning, __ATOMIC_ACQUIRE))
	{
		FinishPlaybackStop();
		return;
	}

	if (status != kIOReturnSuccess)
	{
		FailPlaybackStream(status);
		return;
	}

	if (actualByteCount > akai_eie_driver::kCaptureTransferBytes)
	{
		FailPlaybackStream(kIOReturnOverrun);
		return;
	}
	actualByteCount -= actualByteCount %
					   akai_eie_driver::kCaptureBytesPerUsbFrame;
	const kern_return_t storeResult =
		StoreCaptureBytes(transfer.dataAddress, actualByteCount);
	if (storeResult != kIOReturnSuccess)
	{
		FailPlaybackStream(storeResult);
		return;
	}

	const kern_return_t ret = SubmitCaptureTransfer(index);
	if (ret != kIOReturnSuccess)
		FailPlaybackStream(ret);
}

void AkaiEieDriver::FailPlaybackStream(IOReturn status)
{
	(void)status;
	const bool wasRunning =
		__atomic_exchange_n(&ivars->playbackRunning, false, __ATOMIC_ACQ_REL);
	if (!wasRunning)
		return;
	__atomic_store_n(&ivars->captureRunning, false, __ATOMIC_RELEASE);

	if (ivars->audioDevice)
		(void)ivars->audioDevice->SetClockIsStable(false);

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

	if (ivars->syncPipe)
		(void)ivars->syncPipe->Abort(
			kIOUSBAbortAsynchronous, kIOReturnAborted, nullptr);
	if (ivars->capturePipe)
		(void)ivars->capturePipe->Abort(
			kIOUSBAbortAsynchronous, kIOReturnAborted, nullptr);
	if (ivars->playbackPipe)
		(void)ivars->playbackPipe->Abort(
			kIOUSBAbortAsynchronous, kIOReturnAborted, nullptr);
	FinishPlaybackStop();
}

void AkaiEieDriver::FinishPlaybackStop()
{
	if (!__atomic_load_n(&ivars->playbackStopPending, __ATOMIC_ACQUIRE) ||
		ivars->playbackInFlight != 0 ||
		ivars->syncInFlight != 0 ||
		ivars->captureInFlight != 0)
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
	ReleaseCaptureResources();
	ReleaseSyncResources();
	ReleasePlaybackResources();
	ivars->dataInterface.reset();
	__atomic_store_n(&ivars->playbackStopPending, false, __ATOMIC_RELEASE);
}

void AkaiEieDriver::ReleaseCaptureResources()
{
	for (uint32_t index = 0;
		 index < akai_eie_driver::kCaptureTransferCount;
		 ++index)
	{
		AkaiEieCaptureTransfer &transfer = ivars->captureTransfers[index];
		transfer.submitted = false;
		transfer.dataAddress = nullptr;
		transfer.completion.reset();
		transfer.dataMap.reset();
		transfer.dataBuffer.reset();
	}

	ivars->capturePipe.reset();
	if (ivars->captureRingAddress != nullptr)
	{
		IOSafeDeleteNULL(
			ivars->captureRingAddress,
			uint8_t,
			akai_eie_driver::kCaptureRingBytes);
	}
	ivars->captureReadFrame = 0;
	ivars->captureWriteFrame = 0;
	ivars->captureInFlight = 0;
	ivars->captureMaxPacketSize = 0;
	ivars->captureEndpointAddress = 0;
	ivars->captureRunning = false;
}

void AkaiEieDriver::ReleaseSyncResources()
{
	for (uint32_t index = 0;
		 index < akai_eie_driver::kSyncTransferCount;
		 ++index)
	{
		AkaiEieSyncTransfer &transfer = ivars->syncTransfers[index];
		transfer.submitted = false;
		transfer.dataAddress = nullptr;
		transfer.frameListAddress = nullptr;
		transfer.completion.reset();
		transfer.frameListMap.reset();
		transfer.frameListBuffer.reset();
		transfer.dataMap.reset();
		transfer.dataBuffer.reset();
	}

	ivars->syncPipe.reset();
	ivars->syncInFlight = 0;
	ivars->syncEndpointAddress = 0;
	ivars->syncMaxPacketSize = 0;
	ivars->syncIntervalMicroframes = 0;
	ivars->syncReportsPerPlaybackPeriod = 0;
	ivars->syncFramesElapsed = 0;
	ivars->syncClockBaseFrames = 0;
	ivars->syncLastHostTime = 0;
	ivars->playbackTimingState = {};
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
	ivars->playbackTimestampFrames = 0;
	ivars->playbackInFlight = 0;
	ivars->playbackMaxPacketSize = 0;
	ivars->playbackIntervalMicroframes = 0;
	ivars->playbackHasReceivedFrames = false;
	ivars->playbackTransfersStarted = false;
}

void AkaiEieDriver::StopPlayback()
{
	if (ivars == nullptr)
		return;

	const bool hadPlaybackResources =
		ivars->playbackRingAddress != nullptr || ivars->playbackPipe ||
		ivars->captureRingAddress != nullptr || ivars->capturePipe ||
		ivars->playbackInterface || ivars->dataInterface ||
		ivars->playbackInterfaceOpen || ivars->dataInterfaceOpen;
	__atomic_store_n(&ivars->playbackRunning, false, __ATOMIC_RELEASE);
	__atomic_store_n(&ivars->captureRunning, false, __ATOMIC_RELEASE);
	if (ivars->audioDevice)
		(void)ivars->audioDevice->SetClockIsStable(false);
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

	if (ivars->syncPipe)
	{
		OSSharedPtr<IOUSBHostPipe> syncPipe = ivars->syncPipe;
		(void)syncPipe->Abort(
			kIOUSBAbortSynchronous,
			kIOReturnAborted,
			nullptr);
	}
	if (ivars->capturePipe)
	{
		OSSharedPtr<IOUSBHostPipe> capturePipe = ivars->capturePipe;
		(void)capturePipe->Abort(
			kIOUSBAbortSynchronous,
			kIOReturnAborted,
			nullptr);
	}
	if (ivars->playbackPipe)
	{
		OSSharedPtr<IOUSBHostPipe> playbackPipe = ivars->playbackPipe;
		(void)playbackPipe->Abort(
			kIOUSBAbortSynchronous,
			kIOReturnAborted,
			nullptr);
	}
	FinishPlaybackStop();
}

kern_return_t IMPL(AkaiEieDriver, Stop)
{
	StopPlayback();
	if (ivars->audioDevice)
		(void)RemoveObject(ivars->audioDevice.get());
	CloseUsbProvider();
	ivars->audioDevice.reset();
	ivars->workQueue.reset();

	return Stop(provider, SUPERDISPATCH);
}

void AkaiEieDriver::CloseUsbProvider()
{
	if (ivars == nullptr)
		return;

	if (ivars->usbDevice && ivars->usbDeviceOpen)
	{
		(void)ivars->usbDevice->Close(this, 0);
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
