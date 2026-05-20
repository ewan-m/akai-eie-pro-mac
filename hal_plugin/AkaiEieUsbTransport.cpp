#include "AkaiEieUsbTransport.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/usb/USB.h>
#include <os/log.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <new>

namespace akai_eie_hal
{
	namespace
	{

		using DeviceInterface = IOUSBDeviceInterface942;
		using InterfaceInterface = IOUSBInterfaceInterface942;

		constexpr std::uint32_t kSampleRateHz = 48000;
		constexpr std::size_t kOutputRingFrameCount = kSampleRateHz / 2;
		constexpr std::size_t kOutputPrimeFrameCount = kSampleRateHz / 20;
		constexpr std::size_t kOutputReprimeFrameCount = kOutputPrimeFrameCount;
		constexpr std::size_t kOutputLowWaterFrameCount = kSampleRateHz / 40;
		constexpr std::size_t kOutputHighWaterFrameCount = kSampleRateHz / 5;
		constexpr std::size_t kPlaybackTransferCount = 6;
		constexpr std::size_t kSyncTransferCount = 2;
		constexpr std::size_t kMaxSyncPacketBytes = 64;
		constexpr std::uint32_t kPlaybackIsoPacketCount =
			akai_eie::kDefaultPlaybackIsoPacketCount;
		constexpr std::uint32_t kMaxPlaybackFramesPerTransfer = 480;
		constexpr std::size_t kMaxPlaybackTransferBytes =
			kMaxPlaybackFramesPerTransfer * akai_eie::kPlaybackBytesPerFrame;
		constexpr std::uint64_t kUsbScheduleLeadBusFrames = 16;
		constexpr std::uint64_t kUsbScheduleRecoveryLeadBusFrames = 24;
		constexpr std::uint32_t kIsoSubmitAttemptCount = 4;
		constexpr std::uint32_t kMaxConsecutivePlaybackErrors = 3;
		constexpr std::uint32_t kMaxConsecutiveEmptySyncPackets = 64;
		constexpr std::chrono::milliseconds kStartTimeout{12000};
		constexpr std::chrono::milliseconds kRestartBackoff{250};

		std::uint64_t PlaybackTransferDurationBusFrames(
			std::uint32_t frameListMicros) noexcept
		{
			const std::uint64_t durationMicros =
				static_cast<std::uint64_t>(kPlaybackIsoPacketCount) * std::max<std::uint32_t>(frameListMicros, 1);
			return std::max<std::uint64_t>(1, (durationMicros + 999) / 1000);
		}

		const char *IOReturnName(IOReturn value) noexcept
		{
			switch (value)
			{
			case kIOReturnSuccess:
				return "success";
			case kIOReturnExclusiveAccess:
				return "exclusive access";
			case kIOReturnNoDevice:
				return "no device";
			case kIOReturnNotOpen:
				return "not open";
			case kIOReturnNoMemory:
				return "no memory";
			case kIOReturnNotFound:
				return "not found";
			case kIOReturnAborted:
				return "aborted";
			case kIOReturnIsoTooOld:
				return "iso too old";
			case kIOReturnIsoTooNew:
				return "iso too new";
			case kIOReturnBadArgument:
				return "bad argument";
			case kIOReturnNoBandwidth:
				return "no bandwidth";
			case kIOReturnUnderrun:
				return "underrun";
			case kIOReturnOverrun:
				return "overrun";
			default:
				return "error";
			}
		}

		bool IsPlausibleStandardSyncFeedback(
			std::uint32_t framesPerMillisecond) noexcept
		{
			constexpr std::uint32_t kNominalFramesPerMillisecond =
				kSampleRateHz / 1000;
			constexpr std::uint32_t kToleranceFrames = 8;
			return framesPerMillisecond >= kNominalFramesPerMillisecond - kToleranceFrames && framesPerMillisecond <= kNominalFramesPerMillisecond + kToleranceFrames && framesPerMillisecond < 50;
		}

		std::uint32_t DecodeSyncFeedbackFrames(
			const std::uint8_t *buffer, std::uint16_t actualBytes) noexcept
		{
			if (buffer == nullptr || actualBytes == 0)
				return 0;

			// The EIE Pro reports a custom one-byte elapsed-frame count; accept
			// standard USB Audio fixed-point feedback as a fallback.
			const std::uint8_t akaiFrames = buffer[0];
			if (akai_eie::IsReasonableSyncFeedback(akaiFrames))
				return akaiFrames;

			if (actualBytes >= 3)
			{
				const std::uint32_t raw10_14 =
					static_cast<std::uint32_t>(buffer[0]) | (static_cast<std::uint32_t>(buffer[1]) << 8) | (static_cast<std::uint32_t>(buffer[2]) << 16);
				const std::uint32_t frames =
					(raw10_14 + (1u << 13)) >> 14;
				if (IsPlausibleStandardSyncFeedback(frames))
					return frames;
			}

			if (actualBytes >= 4)
			{
				const std::uint32_t raw16_16 =
					static_cast<std::uint32_t>(buffer[0]) | (static_cast<std::uint32_t>(buffer[1]) << 8) | (static_cast<std::uint32_t>(buffer[2]) << 16) | (static_cast<std::uint32_t>(buffer[3]) << 24);
				const std::uint32_t frames = static_cast<std::uint32_t>(
					((static_cast<std::uint64_t>(raw16_16) * 8u) + (1u << 15)) >> 16);
				if (IsPlausibleStandardSyncFeedback(frames))
					return frames;
			}

			return 0;
		}

		bool SetMatchingNumber(CFMutableDictionaryRef dictionary, const char *key,
							   std::uint16_t value) noexcept
		{
			if (dictionary == nullptr || key == nullptr)
				return false;

			CFStringRef cfKey = CFStringCreateWithCString(
				kCFAllocatorDefault, key, kCFStringEncodingUTF8);
			if (cfKey == nullptr)
				return false;

			const SInt32 signedValue = static_cast<SInt32>(value);
			CFNumberRef number = CFNumberCreate(kCFAllocatorDefault,
												kCFNumberSInt32Type, &signedValue);
			if (number == nullptr)
			{
				CFRelease(cfKey);
				return false;
			}

			CFDictionarySetValue(dictionary, cfKey, number);
			CFRelease(number);
			CFRelease(cfKey);
			return true;
		}

		io_service_t FindDeviceWithClassName(const char *className) noexcept
		{
			CFMutableDictionaryRef matching = IOServiceMatching(className);
			if (matching == nullptr)
				return IO_OBJECT_NULL;

			if (!SetMatchingNumber(matching, kUSBVendorID, akai_eie::kVendorId) || !SetMatchingNumber(matching, kUSBProductID,
																									  akai_eie::kProductId))
			{
				CFRelease(matching);
				return IO_OBJECT_NULL;
			}

			io_iterator_t iterator = IO_OBJECT_NULL;
			const kern_return_t result = IOServiceGetMatchingServices(
				kIOMainPortDefault, matching, &iterator);
			if (result != KERN_SUCCESS)
				return IO_OBJECT_NULL;

			io_service_t service = IOIteratorNext(iterator);
			IOObjectRelease(iterator);
			return service;
		}

		io_service_t FindAkaiDeviceService() noexcept
		{
			io_service_t service = FindDeviceWithClassName(kIOUSBDeviceClassName);
			if (service != IO_OBJECT_NULL)
				return service;
			return FindDeviceWithClassName(kIOUSBHostDeviceClassName);
		}

		bool CreateDeviceInterface(io_service_t service,
								   DeviceInterface ***outDevice) noexcept
		{
			if (service == IO_OBJECT_NULL || outDevice == nullptr)
				return false;

			IOCFPlugInInterface **plugin = nullptr;
			SInt32 score = 0;
			IOReturn result = IOCreatePlugInInterfaceForService(service,
																kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plugin,
																&score);
			if (result != kIOReturnSuccess || plugin == nullptr)
			{
				os_log_error(OS_LOG_DEFAULT,
							 "AKAI EIE Pro USB: device plugin failed %{public}s (%{public}#x)",
							 IOReturnName(result), result);
				return false;
			}

			result = (*plugin)->QueryInterface(plugin,
											   CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID942),
											   reinterpret_cast<LPVOID *>(outDevice));
			(*plugin)->Release(plugin);

			if (result != kIOReturnSuccess || *outDevice == nullptr)
			{
				os_log_error(OS_LOG_DEFAULT,
							 "AKAI EIE Pro USB: device interface failed %{public}s (%{public}#x)",
							 IOReturnName(result), result);
				return false;
			}

			return true;
		}

		bool CreateInterfaceInterface(io_service_t service,
									  InterfaceInterface ***outInterface) noexcept
		{
			if (service == IO_OBJECT_NULL || outInterface == nullptr)
				return false;

			IOCFPlugInInterface **plugin = nullptr;
			SInt32 score = 0;
			IOReturn result = IOCreatePlugInInterfaceForService(service,
																kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &plugin,
																&score);
			if (result != kIOReturnSuccess || plugin == nullptr)
				return false;

			result = (*plugin)->QueryInterface(plugin,
											   CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID942),
											   reinterpret_cast<LPVOID *>(outInterface));
			(*plugin)->Release(plugin);
			return result == kIOReturnSuccess && *outInterface != nullptr;
		}

		void ReleaseInterface(InterfaceInterface ***interfacePtr,
							  bool *interfaceOpen) noexcept
		{
			if (interfacePtr == nullptr || *interfacePtr == nullptr)
				return;

			if (interfaceOpen != nullptr && *interfaceOpen)
			{
				(**interfacePtr)->USBInterfaceClose(*interfacePtr);
				*interfaceOpen = false;
			}
			(**interfacePtr)->Release(*interfacePtr);
			*interfacePtr = nullptr;
		}

	} // namespace

	struct AkaiEieUsbTransport::Impl
	{
		struct PlaybackTransfer
		{
			AkaiEieUsbTransport *owner = nullptr;
			std::size_t index = 0;
			bool submitted = false;
			std::array<std::uint8_t, kMaxPlaybackTransferBytes> buffer{};
			std::array<IOUSBIsocFrame, kPlaybackIsoPacketCount> frames{};
		};

		struct SyncTransfer
		{
			AkaiEieUsbTransport *owner = nullptr;
			std::size_t index = 0;
			bool submitted = false;
			std::array<std::uint8_t, kMaxSyncPacketBytes> buffer{};
			IOUSBIsocFrame frame{};
		};

		DeviceInterface **device = nullptr;
		InterfaceInterface **playInterface = nullptr;
		InterfaceInterface **dataInterface = nullptr;
		InterfaceInterface **syncInterface = nullptr;
		CFRunLoopSourceRef playAsyncSource = nullptr;
		CFRunLoopSourceRef dataAsyncSource = nullptr;
		bool deviceOpen = false;
		bool playInterfaceOpen = false;
		bool dataInterfaceOpen = false;
		UInt8 playPipe = 0;
		UInt8 syncPipe = 0;
		UInt8 capturePipe = 0;
		UInt16 playMaxPacketSize = 0;
		UInt16 syncMaxPacketSize = 0;
		UInt16 captureMaxPacketSize = 0;
		std::uint32_t frameListMicros = 1000;
		std::uint64_t nextFrameStart = 0;
		std::uint64_t syncNextFrameStart = 0;
		std::atomic<std::uint32_t> syncFeedbackFrames{0};
		akai_eie::PlaybackTimingState playbackTiming{};
		std::array<PlaybackTransfer, kPlaybackTransferCount> playbackTransfers{};
		std::array<SyncTransfer, kSyncTransferCount> syncTransfers{};

		std::mutex outputMutex;
		std::array<std::uint8_t,
				   kOutputRingFrameCount * akai_eie::kPlaybackBytesPerFrame>
			outputRing{};
		std::size_t outputReadFrame = 0;
		std::size_t outputWriteFrame = 0;
		std::size_t outputFrameCount = 0;
		bool outputPrimed = false;
		std::uint64_t underrunCount = 0;
		std::uint64_t rebufferCount = 0;
		std::uint64_t droppedFrameCount = 0;
		std::uint64_t syncErrorCount = 0;
		std::uint64_t playbackCompletionErrorCount = 0;
		std::uint32_t consecutivePlaybackErrorCount = 0;
		std::uint32_t consecutiveEmptySyncPacketCount = 0;
		bool syncFeedbackDisabled = false;
		bool syncFeedbackEmptyLogged = false;

		std::mutex runLoopMutex;
		CFRunLoopRef workerRunLoop = nullptr;
	};

	AkaiEieUsbTransport::AkaiEieUsbTransport()
		: impl_(new (std::nothrow) Impl)
	{
		if (impl_ != nullptr)
		{
			for (std::size_t i = 0; i < impl_->playbackTransfers.size(); ++i)
			{
				impl_->playbackTransfers[i].owner = this;
				impl_->playbackTransfers[i].index = i;
			}
			for (std::size_t i = 0; i < impl_->syncTransfers.size(); ++i)
			{
				impl_->syncTransfers[i].owner = this;
				impl_->syncTransfers[i].index = i;
			}
		}
	}

	AkaiEieUsbTransport::~AkaiEieUsbTransport()
	{
		Stop();
		delete impl_;
		impl_ = nullptr;
	}

	bool AkaiEieUsbTransport::Start() noexcept
	{
		if (impl_ == nullptr)
			return false;

		std::unique_lock<std::mutex> lock(startMutex_);
		if (started_.load())
			return true;
		if (workerThread_.joinable())
		{
			lock.unlock();
			Stop();
			lock.lock();
		}

		ResetOutputRing();
		stopRequested_.store(false);
		restartRequested_.store(false);
		inFlightTransfers_.store(0);
		startCompleted_ = false;
		startSucceeded_ = false;

		try
		{
			workerThread_ = std::thread(&AkaiEieUsbTransport::WorkerMain, this);
		}
		catch (...)
		{
			os_log_error(OS_LOG_DEFAULT,
						 "AKAI EIE Pro USB: failed to start worker thread");
			return false;
		}

		const bool completed = startCondition_.wait_for(lock, kStartTimeout,
														[this]() noexcept
														{ return startCompleted_; });
		if (!completed)
		{
			os_log_error(OS_LOG_DEFAULT,
						 "AKAI EIE Pro USB: worker start timed out");
			lock.unlock();
			Stop();
			return false;
		}

		const bool ok = startSucceeded_;
		lock.unlock();
		if (!ok && workerThread_.joinable())
			workerThread_.join();
		return ok;
	}

	void AkaiEieUsbTransport::Stop() noexcept
	{
		if (impl_ == nullptr)
			return;

		stopRequested_.store(true);
		restartRequested_.store(false);

		CFRunLoopRef runLoop = nullptr;
		{
			std::lock_guard<std::mutex> lock(impl_->runLoopMutex);
			runLoop = impl_->workerRunLoop;
			if (runLoop != nullptr)
				CFRetain(runLoop);
		}
		if (runLoop != nullptr)
		{
			CFRunLoopStop(runLoop);
			CFRelease(runLoop);
		}

		if (workerThread_.joinable())
			workerThread_.join();

		started_.store(false);
		ResetOutputRing();
	}

	void AkaiEieUsbTransport::EnqueueOutput(const Float32 *interleaved,
											std::uint32_t frameCount) noexcept
	{
		if (impl_ == nullptr || interleaved == nullptr || frameCount == 0 || !started_.load())
		{
			return;
		}

		std::lock_guard<std::mutex> lock(impl_->outputMutex);
		for (std::uint32_t frame = 0; frame < frameCount; ++frame)
		{
			if (impl_->outputFrameCount == kOutputRingFrameCount)
			{
				impl_->outputReadFrame =
					(impl_->outputReadFrame + 1) % kOutputRingFrameCount;
				--impl_->outputFrameCount;
				++impl_->droppedFrameCount;
			}

			akai_eie::EncodePlaybackFrameFromFloat32(
				interleaved + frame * akai_eie::kPlaybackChannelCount,
				impl_->outputRing.data() + impl_->outputWriteFrame * akai_eie::kPlaybackBytesPerFrame);
			impl_->outputWriteFrame =
				(impl_->outputWriteFrame + 1) % kOutputRingFrameCount;
			++impl_->outputFrameCount;
		}

		if (!impl_->outputPrimed && impl_->outputFrameCount >= kOutputPrimeFrameCount)
		{
			impl_->outputPrimed = true;
			os_log(OS_LOG_DEFAULT,
				   "AKAI EIE Pro USB: playback buffer primed %{public}zu frames",
				   impl_->outputFrameCount);
		}
	}

	void AkaiEieUsbTransport::WorkerMain() noexcept
	{
		CFRunLoopRef runLoop = CFRunLoopGetCurrent();
		CFRetain(runLoop);
		{
			std::lock_guard<std::mutex> lock(impl_->runLoopMutex);
			impl_->workerRunLoop = runLoop;
		}

		auto abortActivePipes = [this]() noexcept
		{
			if (impl_->playInterface != nullptr && impl_->playPipe != 0)
				(*impl_->playInterface)->AbortPipe(impl_->playInterface, impl_->playPipe);
			if (impl_->syncInterface != nullptr && impl_->syncPipe != 0)
				(*impl_->syncInterface)->AbortPipe(impl_->syncInterface, impl_->syncPipe);
		};
		auto clearSubmittedTransfers = [this]() noexcept
		{
			for (auto &transfer : impl_->playbackTransfers)
				transfer.submitted = false;
			for (auto &transfer : impl_->syncTransfers)
				transfer.submitted = false;
			inFlightTransfers_.store(0);
		};
		auto drainInFlightTransfers = [this, &clearSubmittedTransfers]() noexcept
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			while (inFlightTransfers_.load() > 0 && std::chrono::steady_clock::now() < deadline)
			{
				CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, true);
			}
			const std::uint32_t remaining = inFlightTransfers_.load();
			if (remaining > 0)
			{
				os_log_error(OS_LOG_DEFAULT,
							 "AKAI EIE Pro USB: timed out waiting for %{public}u transfers",
							 remaining);
				clearSubmittedTransfers();
				stopRequested_.store(true);
				return false;
			}
			return true;
		};
		auto publishStartResult = [this](bool open) noexcept
		{
			bool shouldNotify = false;
			{
				std::lock_guard<std::mutex> lock(startMutex_);
				if (!startCompleted_)
				{
					startSucceeded_ = open;
					startCompleted_ = true;
					shouldNotify = true;
				}
			}
			if (shouldNotify)
				startCondition_.notify_all();
		};
		auto startResultPublished = [this]() noexcept
		{
			std::lock_guard<std::mutex> lock(startMutex_);
			return startCompleted_;
		};
		auto waitBeforeRetry = [this]() noexcept
		{
			const auto deadline = std::chrono::steady_clock::now() + kRestartBackoff;
			while (!stopRequested_.load() && std::chrono::steady_clock::now() < deadline)
				std::this_thread::sleep_for(std::chrono::milliseconds(25));
		};

		while (!stopRequested_.load())
		{
			restartRequested_.store(false);

			const bool usbOpen = OpenUsb();
			const bool open =
				usbOpen && StartSyncTransfers() && StartPlaybackTransfers();
			if (!open)
			{
				if (usbOpen)
				{
					abortActivePipes();
					if (!drainInFlightTransfers())
					{
						CloseUsb();
						break;
					}
				}
				CloseUsb();
				if (!startResultPublished())
				{
					publishStartResult(false);
					break;
				}
				waitBeforeRetry();
				continue;
			}

			started_.store(true);
			publishStartResult(true);
			while (!stopRequested_.load() && !restartRequested_.load())
			{
				CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.002, true);
			}

			started_.store(false);
			abortActivePipes();
			const bool drained = drainInFlightTransfers();
			CloseUsb();
			if (!drained)
				break;

			if (!restartRequested_.load() || stopRequested_.load())
				break;

			ResetOutputRing();
			waitBeforeRetry();
		}

		started_.store(false);
		publishStartResult(false);
		CloseUsb();

		{
			std::lock_guard<std::mutex> lock(impl_->runLoopMutex);
			impl_->workerRunLoop = nullptr;
		}
		CFRelease(runLoop);
	}

	bool AkaiEieUsbTransport::OpenUsb() noexcept
	{
		io_service_t service = FindAkaiDeviceService();
		if (service == IO_OBJECT_NULL)
		{
			os_log_error(OS_LOG_DEFAULT,
						 "AKAI EIE Pro USB: device 09e8:0010 not found");
			return false;
		}

		const bool deviceInterfaceOk = CreateDeviceInterface(service, &impl_->device);
		IOObjectRelease(service);
		if (!deviceInterfaceOk)
			return false;

		IOReturn result = (*impl_->device)->USBDeviceOpen(impl_->device);
		if (result == kIOReturnExclusiveAccess)
			result = (*impl_->device)->USBDeviceOpenSeize(impl_->device);
		if (result != kIOReturnSuccess)
		{
			os_log_error(OS_LOG_DEFAULT,
						 "AKAI EIE Pro USB: USBDeviceOpen failed %{public}s (%{public}#x)",
						 IOReturnName(result), result);
			return false;
		}
		impl_->deviceOpen = true;

		UInt8 configuration = 0;
		(*impl_->device)->GetConfiguration(impl_->device, &configuration);

		IOUSBConfigurationDescriptorPtr descriptor = nullptr;
		result = (*impl_->device)->GetConfigurationDescriptorPtr(impl_->device, 0, &descriptor);
		const UInt8 desiredConfiguration =
			(result == kIOReturnSuccess && descriptor != nullptr)
				? descriptor->bConfigurationValue
				: 1;
		if (configuration != desiredConfiguration)
		{
			result = (*impl_->device)->SetConfiguration(impl_->device, desiredConfiguration);
			if (result != kIOReturnSuccess)
			{
				os_log_error(OS_LOG_DEFAULT,
							 "AKAI EIE Pro USB: SetConfiguration(%{public}u) failed %{public}s (%{public}#x)",
							 desiredConfiguration, IOReturnName(result), result);
				return false;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}

		auto openInterface = [this](UInt8 number, InterfaceInterface ***outInterface,
									bool *outOpen) noexcept -> bool
		{
			IOUSBFindInterfaceRequest request{};
			request.bInterfaceClass = kIOUSBFindInterfaceDontCare;
			request.bInterfaceSubClass = kIOUSBFindInterfaceDontCare;
			request.bInterfaceProtocol = kIOUSBFindInterfaceDontCare;
			request.bAlternateSetting = kIOUSBFindInterfaceDontCare;

			io_iterator_t iterator = IO_OBJECT_NULL;
			IOReturn iteratorResult = (*impl_->device)->CreateInterfaceIterator(impl_->device, &request, &iterator);
			if (iteratorResult != kIOReturnSuccess)
				return false;

			bool found = false;
			io_service_t interfaceService = IO_OBJECT_NULL;
			while ((interfaceService = IOIteratorNext(iterator)) != IO_OBJECT_NULL)
			{
				InterfaceInterface **candidate = nullptr;
				const bool created = CreateInterfaceInterface(
					interfaceService, &candidate);
				IOObjectRelease(interfaceService);
				if (!created)
					continue;

				UInt8 interfaceNumber = 0xff;
				(*candidate)->GetInterfaceNumber(candidate, &interfaceNumber);
				if (interfaceNumber != number)
				{
					(*candidate)->Release(candidate);
					continue;
				}

				IOReturn openResult =
					(*candidate)->USBInterfaceOpen(candidate);
				if (openResult == kIOReturnExclusiveAccess)
					openResult = (*candidate)->USBInterfaceOpenSeize(candidate);
				if (openResult != kIOReturnSuccess)
				{
					os_log_error(OS_LOG_DEFAULT,
								 "AKAI EIE Pro USB: interface %{public}u open failed %{public}s (%{public}#x)",
								 number, IOReturnName(openResult), openResult);
					(*candidate)->Release(candidate);
					break;
				}

				IOReturn altResult =
					(*candidate)->SetAlternateInterface(candidate, 1);
				if (altResult != kIOReturnSuccess)
				{
					os_log_error(OS_LOG_DEFAULT,
								 "AKAI EIE Pro USB: interface %{public}u alt 1 failed %{public}s (%{public}#x)",
								 number, IOReturnName(altResult), altResult);
					(*candidate)->USBInterfaceClose(candidate);
					(*candidate)->Release(candidate);
					break;
				}

				*outInterface = candidate;
				*outOpen = true;
				found = true;
				break;
			}

			IOObjectRelease(iterator);
			return found;
		};

		if (!openInterface(0, &impl_->playInterface, &impl_->playInterfaceOpen))
		{
			os_log_error(OS_LOG_DEFAULT,
						 "AKAI EIE Pro USB: playback interface 0 not available");
			return false;
		}
		if (!openInterface(1, &impl_->dataInterface, &impl_->dataInterfaceOpen))
		{
			os_log_error(OS_LOG_DEFAULT,
						 "AKAI EIE Pro USB: data interface 1 not available");
			return false;
		}

		auto findPipe = [](InterfaceInterface **usbInterface, UInt8 direction,
						   UInt8 transferType, bool matchEndpoint, UInt8 endpointNumber,
						   bool requireSmallPacket, UInt8 *outPipe,
						   UInt16 *outMaxPacket) noexcept -> bool
		{
			UInt8 endpointCount = 0;
			if ((*usbInterface)->GetNumEndpoints(usbInterface, &endpointCount) != kIOReturnSuccess)
			{
				return false;
			}

			for (UInt8 pipe = 1; pipe <= endpointCount; ++pipe)
			{
				UInt8 pipeDirection = 0;
				UInt8 pipeNumber = 0;
				UInt8 pipeTransferType = 0;
				UInt16 maxPacketSize = 0;
				UInt8 interval = 0;
				if ((*usbInterface)->GetPipeProperties(usbInterface, pipe, &pipeDirection, &pipeNumber, &pipeTransferType, &maxPacketSize, &interval) != kIOReturnSuccess)
				{
					continue;
				}
				(void)interval;

				if (pipeDirection != direction || pipeTransferType != transferType)
				{
					continue;
				}
				if (matchEndpoint && pipeNumber != endpointNumber)
					continue;
				if (requireSmallPacket && maxPacketSize >= 64)
					continue;

				*outPipe = pipe;
				*outMaxPacket = maxPacketSize;
				return true;
			}
			return false;
		};

		if (!findPipe(impl_->playInterface, kUSBOut, kUSBIsoc, false, 0, false,
					  &impl_->playPipe, &impl_->playMaxPacketSize))
		{
			os_log_error(OS_LOG_DEFAULT,
						 "AKAI EIE Pro USB: playback isoch OUT pipe not found");
			return false;
		}
		if (findPipe(impl_->dataInterface, kUSBIn, kUSBIsoc, false, 0, false,
					 &impl_->syncPipe, &impl_->syncMaxPacketSize))
		{
			impl_->syncInterface = impl_->dataInterface;
		}
		else if (findPipe(impl_->playInterface, kUSBIn, kUSBIsoc, false, 0,
						  false, &impl_->syncPipe, &impl_->syncMaxPacketSize))
		{
			impl_->syncInterface = impl_->playInterface;
		}
		findPipe(impl_->dataInterface, kUSBIn, kUSBBulk, true,
				 akai_eie::kKnownCaptureEndpointAddress & 0x0f, false,
				 &impl_->capturePipe, &impl_->captureMaxPacketSize);

		akai_eie::ControlTransfer transfers[akai_eie::kResetControlTransferCount]{};
		const std::size_t transferCount = akai_eie::BuildResetControlTransfers(
			kSampleRateHz, transfers, akai_eie::kResetControlTransferCount);
		if (transferCount == 0)
		{
			os_log_error(OS_LOG_DEFAULT,
						 "AKAI EIE Pro USB: cannot build reset sequence");
			return false;
		}
		for (std::size_t i = 0; i < transferCount; ++i)
		{
			std::array<std::uint8_t, akai_eie::kMaxControlPayloadBytes> payload{};
			std::memcpy(payload.data(), transfers[i].data, payload.size());

			IOUSBDevRequestTO request{};
			request.bmRequestType = transfers[i].request_type;
			request.bRequest = transfers[i].request;
			request.wValue = transfers[i].value;
			request.wIndex = transfers[i].index;
			request.wLength = transfers[i].length;
			request.pData = transfers[i].length == 0 ? nullptr : payload.data();
			request.noDataTimeout = 1000;
			request.completionTimeout = 1000;

			result = (*impl_->device)->DeviceRequestTO(impl_->device, &request);
			if (result != kIOReturnSuccess)
			{
				os_log_error(OS_LOG_DEFAULT,
							 "AKAI EIE Pro USB: reset transfer %{public}zu failed %{public}s (%{public}#x)",
							 i, IOReturnName(result), result);
				return false;
			}
		}

		result = (*impl_->playInterface)->CreateInterfaceAsyncEventSource(impl_->playInterface, &impl_->playAsyncSource);
		if (result != kIOReturnSuccess || impl_->playAsyncSource == nullptr)
		{
			os_log_error(OS_LOG_DEFAULT,
						 "AKAI EIE Pro USB: async source failed %{public}s (%{public}#x)",
						 IOReturnName(result), result);
			return false;
		}
		CFRunLoopAddSource(CFRunLoopGetCurrent(), impl_->playAsyncSource,
						   kCFRunLoopDefaultMode);

		if (impl_->syncPipe != 0 && impl_->syncInterface == impl_->dataInterface)
		{
			result = (*impl_->dataInterface)->CreateInterfaceAsyncEventSource(impl_->dataInterface, &impl_->dataAsyncSource);
			if (result != kIOReturnSuccess || impl_->dataAsyncSource == nullptr)
			{
				os_log_error(OS_LOG_DEFAULT,
							 "AKAI EIE Pro USB: data async source failed %{public}s (%{public}#x)",
							 IOReturnName(result), result);
				return false;
			}
			CFRunLoopAddSource(CFRunLoopGetCurrent(), impl_->dataAsyncSource,
							   kCFRunLoopDefaultMode);
		}

		(*impl_->playInterface)->GetFrameListTime(impl_->playInterface, &impl_->frameListMicros);
		std::uint64_t currentFrame = 0;
		if (!ReadCurrentUsbFrame(&currentFrame))
			currentFrame = 0;
		impl_->nextFrameStart = currentFrame + kUsbScheduleLeadBusFrames;
		if (impl_->syncPipe != 0)
		{
			if (!ReadCurrentDataUsbFrame(&currentFrame))
				currentFrame = 0;
			impl_->syncNextFrameStart =
				currentFrame + kUsbScheduleLeadBusFrames;
		}

		os_log(OS_LOG_DEFAULT,
			   "AKAI EIE Pro USB: opened playback pipe %{public}u maxPacket=%{public}u frameList=%{public}uus startFrame=%{public}llu syncPipe=%{public}u syncOnData=%{public}u capturePipe=%{public}u",
			   impl_->playPipe, impl_->playMaxPacketSize, impl_->frameListMicros,
			   static_cast<unsigned long long>(impl_->nextFrameStart),
			   impl_->syncPipe,
			   impl_->syncInterface == impl_->dataInterface ? 1u : 0u,
			   impl_->capturePipe);
		return true;
	}

	void AkaiEieUsbTransport::CloseUsb() noexcept
	{
		if (impl_->playAsyncSource != nullptr)
		{
			CFRunLoopRemoveSource(CFRunLoopGetCurrent(), impl_->playAsyncSource,
								  kCFRunLoopDefaultMode);
			CFRelease(impl_->playAsyncSource);
			impl_->playAsyncSource = nullptr;
		}
		if (impl_->dataAsyncSource != nullptr)
		{
			CFRunLoopRemoveSource(CFRunLoopGetCurrent(), impl_->dataAsyncSource,
								  kCFRunLoopDefaultMode);
			CFRelease(impl_->dataAsyncSource);
			impl_->dataAsyncSource = nullptr;
		}

		ReleaseInterface(&impl_->dataInterface, &impl_->dataInterfaceOpen);
		ReleaseInterface(&impl_->playInterface, &impl_->playInterfaceOpen);
		impl_->syncInterface = nullptr;

		if (impl_->device != nullptr)
		{
			if (impl_->deviceOpen)
			{
				(*impl_->device)->USBDeviceClose(impl_->device);
				impl_->deviceOpen = false;
			}
			(*impl_->device)->Release(impl_->device);
			impl_->device = nullptr;
		}

		impl_->playPipe = 0;
		impl_->syncPipe = 0;
		impl_->capturePipe = 0;
		impl_->playMaxPacketSize = 0;
		impl_->syncMaxPacketSize = 0;
		impl_->captureMaxPacketSize = 0;
		impl_->nextFrameStart = 0;
		impl_->syncNextFrameStart = 0;
		impl_->syncFeedbackFrames.store(0, std::memory_order_release);
		impl_->playbackTiming = {};
	}

	bool AkaiEieUsbTransport::StartSyncTransfers() noexcept
	{
		if (impl_->syncPipe == 0)
		{
			os_log(OS_LOG_DEFAULT, "AKAI EIE Pro USB: sync feedback endpoint not available");
			return true;
		}

		for (std::size_t i = 0; i < impl_->syncTransfers.size(); ++i)
		{
			if (!SubmitSyncTransfer(i))
				return false;
		}
		return true;
	}

	bool AkaiEieUsbTransport::StartPlaybackTransfers() noexcept
	{
		for (std::size_t i = 0; i < impl_->playbackTransfers.size(); ++i)
		{
			if (!SubmitPlaybackTransfer(i))
				return false;
		}
		return true;
	}

	bool AkaiEieUsbTransport::SubmitSyncTransfer(
		std::size_t transferIndex) noexcept
	{
		if (impl_->syncInterface == nullptr || impl_->syncPipe == 0 || impl_->syncFeedbackDisabled || transferIndex >= impl_->syncTransfers.size() || stopRequested_.load())
		{
			return false;
		}

		auto &transfer = impl_->syncTransfers[transferIndex];
		std::memset(transfer.buffer.data(), 0, transfer.buffer.size());
		transfer.frame.frStatus = kIOReturnSuccess;
		transfer.frame.frReqCount = static_cast<UInt16>(
			std::min<std::size_t>(impl_->syncMaxPacketSize,
								  transfer.buffer.size()));
		transfer.frame.frActCount = 0;

		IOReturn lastResult = kIOReturnSuccess;
		std::uint64_t lastFrameStart = 0;
		std::uint64_t lastCurrentFrame = 0;
		bool lastHaveCurrentFrame = false;
		for (std::uint32_t attempt = 0; attempt < kIsoSubmitAttemptCount; ++attempt)
		{
			std::uint64_t currentFrame = 0;
			const bool haveCurrentFrame = ReadCurrentDataUsbFrame(&currentFrame);
			if (haveCurrentFrame && impl_->syncNextFrameStart < currentFrame + kUsbScheduleLeadBusFrames)
			{
				impl_->syncNextFrameStart =
					currentFrame + kUsbScheduleLeadBusFrames;
			}

			const std::uint64_t frameStart = impl_->syncNextFrameStart;
			++impl_->syncNextFrameStart;
			lastFrameStart = frameStart;
			lastCurrentFrame = currentFrame;
			lastHaveCurrentFrame = haveCurrentFrame;

			transfer.submitted = true;
			inFlightTransfers_.fetch_add(1);
			lastResult = (*impl_->syncInterface)->ReadIsochPipeAsync(impl_->syncInterface, impl_->syncPipe, transfer.buffer.data(), frameStart, 1, &transfer.frame, &AkaiEieUsbTransport::SyncComplete, &transfer);
			if (lastResult == kIOReturnSuccess)
				return true;

			transfer.submitted = false;
			inFlightTransfers_.fetch_sub(1);
			if (lastResult == kIOReturnIsoTooOld || lastResult == kIOReturnIsoTooNew)
			{
				if (ReadCurrentDataUsbFrame(&currentFrame))
					impl_->syncNextFrameStart =
						currentFrame + kUsbScheduleRecoveryLeadBusFrames;
				continue;
			}

			os_log_error(OS_LOG_DEFAULT,
						 "AKAI EIE Pro USB: sync read submit failed %{public}s (%{public}#x) start=%{public}llu current=%{public}llu",
						 IOReturnName(lastResult), lastResult,
						 static_cast<unsigned long long>(lastFrameStart),
						 static_cast<unsigned long long>(
							 lastHaveCurrentFrame ? lastCurrentFrame : 0));
			return false;
		}

		os_log_error(OS_LOG_DEFAULT,
					 "AKAI EIE Pro USB: sync read submit failed %{public}s (%{public}#x) start=%{public}llu current=%{public}llu",
					 IOReturnName(lastResult), lastResult,
					 static_cast<unsigned long long>(lastFrameStart),
					 static_cast<unsigned long long>(
						 lastHaveCurrentFrame ? lastCurrentFrame : 0));
		return false;
	}

	bool AkaiEieUsbTransport::SubmitPlaybackTransfer(
		std::size_t transferIndex) noexcept
	{
		if (impl_->playInterface == nullptr || impl_->playPipe == 0 || transferIndex >= impl_->playbackTransfers.size() || stopRequested_.load())
		{
			return false;
		}

		auto &transfer = impl_->playbackTransfers[transferIndex];
		const std::uint32_t feedbackFrames = impl_->syncFeedbackFrames.exchange(
			0, std::memory_order_acq_rel);
		std::uint32_t framesWanted =
			akai_eie::CalculatePlaybackFramesWanted(kSampleRateHz,
													impl_->playbackTiming, feedbackFrames);
		const std::size_t bufferedFrames = BufferedOutputFrames();
		if (bufferedFrames > kOutputHighWaterFrameCount && framesWanted < kMaxPlaybackFramesPerTransfer)
		{
			++framesWanted;
		}
		else if (bufferedFrames < kOutputLowWaterFrameCount && framesWanted > 1)
		{
			--framesWanted;
		}
		if (framesWanted > kMaxPlaybackFramesPerTransfer)
			return false;

		const std::size_t copiedFrames =
			PopOutputFrames(transfer.buffer.data(), framesWanted);
		if (copiedFrames < framesWanted)
		{
			++impl_->underrunCount;
			if (impl_->underrunCount <= 5)
			{
				os_log(OS_LOG_DEFAULT,
					   "AKAI EIE Pro USB: playback underrun %zu/%u frames, padding silence",
					   copiedFrames, framesWanted);
			}
		}

		akai_eie::IsoPacket packetLayout[kPlaybackIsoPacketCount]{};
		if (!akai_eie::BuildIsoPacketLayout(framesWanted, kPlaybackIsoPacketCount,
											akai_eie::kPlaybackBytesPerFrame, packetLayout))
		{
			return false;
		}

		for (std::size_t i = 0; i < kPlaybackIsoPacketCount; ++i)
		{
			if (packetLayout[i].length_bytes > impl_->playMaxPacketSize)
				return false;
			transfer.frames[i].frStatus = kIOReturnSuccess;
			transfer.frames[i].frReqCount =
				static_cast<UInt16>(packetLayout[i].length_bytes);
			transfer.frames[i].frActCount = 0;
		}

		const std::uint64_t transferDurationBusFrames =
			PlaybackTransferDurationBusFrames(impl_->frameListMicros);
		for (std::uint32_t attempt = 0; attempt < kIsoSubmitAttemptCount; ++attempt)
		{
			std::uint64_t currentFrame = 0;
			const bool haveCurrentFrame = ReadCurrentUsbFrame(&currentFrame);
			if (haveCurrentFrame && impl_->nextFrameStart < currentFrame + kUsbScheduleLeadBusFrames)
			{
				impl_->nextFrameStart = currentFrame + kUsbScheduleLeadBusFrames;
			}

			const std::uint64_t oldNextFrameStart = impl_->nextFrameStart;
			impl_->nextFrameStart += transferDurationBusFrames;

			transfer.submitted = true;
			inFlightTransfers_.fetch_add(1);
			const IOReturn result = (*impl_->playInterface)->WriteIsochPipeAsync(impl_->playInterface, impl_->playPipe, transfer.buffer.data(), oldNextFrameStart, kPlaybackIsoPacketCount, transfer.frames.data(), &AkaiEieUsbTransport::PlaybackComplete, &transfer);
			if (result == kIOReturnSuccess)
				return true;

			transfer.submitted = false;
			inFlightTransfers_.fetch_sub(1);
			impl_->nextFrameStart = oldNextFrameStart;
			if (result == kIOReturnIsoTooOld || result == kIOReturnIsoTooNew)
			{
				if (ReadCurrentUsbFrame(&currentFrame))
					impl_->nextFrameStart =
						currentFrame + kUsbScheduleRecoveryLeadBusFrames;
				continue;
			}

			os_log_error(OS_LOG_DEFAULT,
						 "AKAI EIE Pro USB: isoch write submit failed %{public}s (%{public}#x) start=%{public}llu current=%{public}llu duration=%{public}llu frameList=%{public}u",
						 IOReturnName(result), result,
						 static_cast<unsigned long long>(oldNextFrameStart),
						 static_cast<unsigned long long>(haveCurrentFrame ? currentFrame : 0),
						 static_cast<unsigned long long>(transferDurationBusFrames),
						 impl_->frameListMicros);
			return false;
		}

		return false;
	}

	void AkaiEieUsbTransport::RequestRestart(const char *reason) noexcept
	{
		if (stopRequested_.load())
			return;

		const bool wasRequested = restartRequested_.exchange(true);
		if (!wasRequested)
		{
			os_log_error(OS_LOG_DEFAULT,
						 "AKAI EIE Pro USB: requesting transport restart: %{public}s",
						 reason == nullptr ? "unspecified" : reason);
		}

		CFRunLoopRef runLoop = nullptr;
		{
			std::lock_guard<std::mutex> lock(impl_->runLoopMutex);
			runLoop = impl_->workerRunLoop;
			if (runLoop != nullptr)
				CFRetain(runLoop);
		}
		if (runLoop != nullptr)
		{
			CFRunLoopStop(runLoop);
			CFRelease(runLoop);
		}
	}

	void AkaiEieUsbTransport::HandleSyncComplete(std::size_t transferIndex,
												 IOReturn result) noexcept
	{
		if (transferIndex >= impl_->syncTransfers.size())
			return;

		auto &transfer = impl_->syncTransfers[transferIndex];
		if (transfer.submitted)
		{
			transfer.submitted = false;
			inFlightTransfers_.fetch_sub(1);
		}
		else if (!stopRequested_.load())
		{
			os_log_error(OS_LOG_DEFAULT,
						 "AKAI EIE Pro USB: unexpected sync completion %{public}zu",
						 transferIndex);
			return;
		}
		else
		{
			return;
		}

		bool acceptedSyncFeedback = false;
		if (result == kIOReturnSuccess && transfer.frame.frActCount > 0)
		{
			const std::uint32_t framesElapsed = DecodeSyncFeedbackFrames(
				transfer.buffer.data(), transfer.frame.frActCount);
			if (framesElapsed > 0)
			{
				impl_->syncFeedbackFrames.fetch_add(
					framesElapsed, std::memory_order_acq_rel);
				impl_->consecutiveEmptySyncPacketCount = 0;
				acceptedSyncFeedback = true;
			}
		}

		const bool emptySyncPacket =
			result == kIOReturnSuccess && transfer.frame.frStatus == kIOReturnUnderrun && transfer.frame.frActCount == 0;
		if (!acceptedSyncFeedback && emptySyncPacket)
		{
			++impl_->consecutiveEmptySyncPacketCount;
			if (!impl_->syncFeedbackEmptyLogged)
			{
				impl_->syncFeedbackEmptyLogged = true;
				os_log(OS_LOG_DEFAULT,
					   "AKAI EIE Pro USB: sync feedback endpoint returned empty packets; holding nominal playback timing");
			}
			if (!impl_->syncFeedbackDisabled && impl_->consecutiveEmptySyncPacketCount >= kMaxConsecutiveEmptySyncPackets)
			{
				impl_->syncFeedbackDisabled = true;
				impl_->syncFeedbackFrames.store(0, std::memory_order_release);
				os_log(OS_LOG_DEFAULT,
					   "AKAI EIE Pro USB: sync feedback disabled after %{public}u empty packets; using nominal playback timing",
					   impl_->consecutiveEmptySyncPacketCount);
			}
		}

		if (!acceptedSyncFeedback && !emptySyncPacket && (result != kIOReturnSuccess || transfer.frame.frStatus != kIOReturnSuccess) && !stopRequested_.load() && !restartRequested_.load())
		{
			++impl_->syncErrorCount;
			if (impl_->syncErrorCount <= 5)
			{
				os_log_error(OS_LOG_DEFAULT,
							 "AKAI EIE Pro USB: sync read completion failed %{public}s (%{public}#x) frameStatus=%{public}s (%{public}#x) act=%{public}u",
							 IOReturnName(result), result,
							 IOReturnName(transfer.frame.frStatus),
							 transfer.frame.frStatus,
							 transfer.frame.frActCount);
			}
		}

		if (!impl_->syncFeedbackDisabled && !stopRequested_.load() && !restartRequested_.load() && !SubmitSyncTransfer(transferIndex))
		{
			RequestRestart("sync transfer resubmit failed");
		}
	}

	void AkaiEieUsbTransport::HandlePlaybackComplete(std::size_t transferIndex,
													 IOReturn result) noexcept
	{
		if (transferIndex >= impl_->playbackTransfers.size())
			return;

		auto &transfer = impl_->playbackTransfers[transferIndex];
		if (transfer.submitted)
		{
			transfer.submitted = false;
			inFlightTransfers_.fetch_sub(1);
		}
		else if (!stopRequested_.load())
		{
			os_log_error(OS_LOG_DEFAULT,
						 "AKAI EIE Pro USB: unexpected playback completion %{public}zu",
						 transferIndex);
			return;
		}
		else
		{
			return;
		}

		bool completionError = result != kIOReturnSuccess;
		IOReturn firstFrameStatus = kIOReturnSuccess;
		if (result == kIOReturnSuccess)
		{
			for (const auto &frame : transfer.frames)
			{
				if (frame.frStatus != kIOReturnSuccess)
				{
					completionError = true;
					firstFrameStatus = frame.frStatus;
					break;
				}
			}
		}

		if (completionError && !stopRequested_.load() && !restartRequested_.load())
		{
			++impl_->playbackCompletionErrorCount;
			++impl_->consecutivePlaybackErrorCount;
			if (impl_->playbackCompletionErrorCount <= 8)
			{
				if (result != kIOReturnSuccess)
				{
					os_log_error(OS_LOG_DEFAULT,
								 "AKAI EIE Pro USB: isoch write completion failed %{public}s (%{public}#x)",
								 IOReturnName(result), result);
				}
				else
				{
					os_log_error(OS_LOG_DEFAULT,
								 "AKAI EIE Pro USB: isoch frame completion failed %{public}s (%{public}#x)",
								 IOReturnName(firstFrameStatus), firstFrameStatus);
				}
			}
			if (impl_->consecutivePlaybackErrorCount >= kMaxConsecutivePlaybackErrors)
			{
				RequestRestart("repeated playback completion errors");
				return;
			}
		}
		else if (!completionError)
		{
			impl_->consecutivePlaybackErrorCount = 0;
		}

		if (!stopRequested_.load() && !restartRequested_.load() && !SubmitPlaybackTransfer(transferIndex))
		{
			RequestRestart("playback transfer resubmit failed");
		}
	}

	bool AkaiEieUsbTransport::ReadCurrentUsbFrame(
		std::uint64_t *outFrame) noexcept
	{
		if (impl_->playInterface == nullptr || outFrame == nullptr)
			return false;

		AbsoluteTime atTime{};
		// WriteIsochPipeAsync takes a bus frame start, even for 125us frame lists.
		const IOReturn result = (*impl_->playInterface)->GetBusFrameNumber(impl_->playInterface, outFrame, &atTime);
		return result == kIOReturnSuccess;
	}

	bool AkaiEieUsbTransport::ReadCurrentDataUsbFrame(
		std::uint64_t *outFrame) noexcept
	{
		if (impl_->syncInterface == nullptr || outFrame == nullptr)
			return false;

		AbsoluteTime atTime{};
		const IOReturn result = (*impl_->syncInterface)->GetBusFrameNumber(impl_->syncInterface, outFrame, &atTime);
		return result == kIOReturnSuccess;
	}

	std::size_t AkaiEieUsbTransport::BufferedOutputFrames() noexcept
	{
		std::lock_guard<std::mutex> lock(impl_->outputMutex);
		return impl_->outputFrameCount;
	}

	std::size_t AkaiEieUsbTransport::PopOutputFrames(std::uint8_t *outFrames,
													 std::size_t frameCount) noexcept
	{
		if (outFrames == nullptr || frameCount == 0)
			return 0;

		std::size_t copied = 0;
		{
			std::lock_guard<std::mutex> lock(impl_->outputMutex);

			if (!impl_->outputPrimed && impl_->outputFrameCount >= kOutputReprimeFrameCount)
			{
				impl_->outputPrimed = true;
				++impl_->rebufferCount;
				if (impl_->rebufferCount <= 5)
				{
					os_log(OS_LOG_DEFAULT,
						   "AKAI EIE Pro USB: playback re-primed %{public}zu frames",
						   impl_->outputFrameCount);
				}
			}

			if (impl_->outputPrimed && impl_->outputFrameCount < frameCount)
			{
				impl_->outputPrimed = false;
				impl_->outputReadFrame = 0;
				impl_->outputWriteFrame = 0;
				impl_->outputFrameCount = 0;
			}

			while (impl_->outputPrimed && copied < frameCount && impl_->outputFrameCount > 0)
			{
				const std::size_t contiguousAvailable =
					std::min(impl_->outputFrameCount,
							 kOutputRingFrameCount - impl_->outputReadFrame);
				const std::size_t contiguousNeeded = frameCount - copied;
				const std::size_t framesToCopy =
					std::min(contiguousAvailable, contiguousNeeded);
				const std::size_t bytesToCopy =
					framesToCopy * akai_eie::kPlaybackBytesPerFrame;

				std::memcpy(
					outFrames + copied * akai_eie::kPlaybackBytesPerFrame,
					impl_->outputRing.data() + impl_->outputReadFrame * akai_eie::kPlaybackBytesPerFrame,
					bytesToCopy);
				impl_->outputReadFrame =
					(impl_->outputReadFrame + framesToCopy) % kOutputRingFrameCount;
				impl_->outputFrameCount -= framesToCopy;
				copied += framesToCopy;
			}

			if (impl_->outputPrimed && copied < frameCount && impl_->outputFrameCount == 0)
			{
				impl_->outputPrimed = false;
			}
		}

		// Pad any remaining requested frames with silence
		if (copied < frameCount)
		{
			std::memset(outFrames + copied * akai_eie::kPlaybackBytesPerFrame,
						0, (frameCount - copied) * akai_eie::kPlaybackBytesPerFrame);
		}

		return copied;
	}

	void AkaiEieUsbTransport::ResetOutputRing() noexcept
	{
		if (impl_ == nullptr)
			return;

		std::lock_guard<std::mutex> lock(impl_->outputMutex);
		impl_->outputReadFrame = 0;
		impl_->outputWriteFrame = 0;
		impl_->outputFrameCount = 0;
		impl_->outputPrimed = false;
		impl_->underrunCount = 0;
		impl_->rebufferCount = 0;
		impl_->droppedFrameCount = 0;
		impl_->syncErrorCount = 0;
		impl_->playbackCompletionErrorCount = 0;
		impl_->consecutivePlaybackErrorCount = 0;
		impl_->consecutiveEmptySyncPacketCount = 0;
		impl_->syncFeedbackDisabled = false;
		impl_->syncFeedbackEmptyLogged = false;
		impl_->syncFeedbackFrames.store(0, std::memory_order_release);
	}

	void AkaiEieUsbTransport::SyncComplete(void *refcon, IOReturn result,
										   void *arg0) noexcept
	{
		(void)arg0;
		auto *transfer = static_cast<Impl::SyncTransfer *>(refcon);
		if (transfer == nullptr || transfer->owner == nullptr)
			return;
		transfer->owner->HandleSyncComplete(transfer->index, result);
	}

	void AkaiEieUsbTransport::PlaybackComplete(void *refcon, IOReturn result,
											   void *arg0) noexcept
	{
		(void)arg0;
		auto *transfer = static_cast<Impl::PlaybackTransfer *>(refcon);
		if (transfer == nullptr || transfer->owner == nullptr)
			return;
		transfer->owner->HandlePlaybackComplete(transfer->index, result);
	}

} // namespace akai_eie_hal
