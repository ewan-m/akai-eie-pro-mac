#include "AkaiEieUsbTransport.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/usb/USB.h>
#include <os/log.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <new>
#include <pthread.h>

namespace akai_eie_hal
{
	namespace
	{

		using DeviceInterface = IOUSBDeviceInterface942;
		using InterfaceInterface = IOUSBInterfaceInterface942;

		constexpr std::uint32_t kDefaultSampleRateHz = 48000;
		constexpr std::uint32_t kMaxSampleRateHz = 96000;
		constexpr std::size_t kOutputRingFrameCount = kMaxSampleRateHz / 2;
		constexpr std::size_t kInputRingFrameCount = kMaxSampleRateHz / 2;
		constexpr std::size_t kCaptureTransferCount = 2;
		constexpr std::size_t kCaptureTransferBytes =
			4 * akai_eie::kDefaultCaptureTransferBytes;
		constexpr std::size_t kPlaybackTransferCount = 8;
		constexpr std::uint32_t kPlaybackIsoPacketCount =
			akai_eie::kDefaultPlaybackIsoPacketCount;
		constexpr std::uint32_t kMaxPlaybackFramesPerTransfer = 480;
		constexpr std::size_t kMaxPlaybackTransferBytes =
			kMaxPlaybackFramesPerTransfer * akai_eie::kPlaybackBytesPerFrame;
		constexpr std::uint64_t kUsbScheduleLeadBusFrames = 16;
		constexpr std::uint64_t kUsbScheduleRecoveryLeadBusFrames = 24;
		constexpr std::uint32_t kIsoSubmitAttemptCount = 4;
		constexpr std::uint32_t kMaxConsecutivePlaybackErrors = 3;
		constexpr std::uint32_t kMaxConsecutiveCaptureErrors = 3;
		constexpr double kNominalPlaybackTransferSeconds = 0.005;
		constexpr std::chrono::milliseconds kStartTimeout{12000};
		constexpr std::chrono::milliseconds kRestartBackoff{250};

		double PlaybackTransferSeconds(std::uint32_t frameListMicros) noexcept
		{
			const double transferSeconds =
				(static_cast<double>(kPlaybackIsoPacketCount) *
				 static_cast<double>(std::max<std::uint32_t>(
					 frameListMicros, 1))) /
				1000000.0;
			constexpr double kMaxProtocolTransferSeconds =
				static_cast<double>(kMaxPlaybackFramesPerTransfer) /
				static_cast<double>(kMaxSampleRateHz);
			return transferSeconds > 0.0 &&
						   transferSeconds <= kMaxProtocolTransferSeconds
					   ? transferSeconds
					   : kNominalPlaybackTransferSeconds;
		}

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
			case kIOUSBLowLatencyBufferNotPreviouslyAllocated:
				return "low latency buffer not allocated";
			case kIOUSBLowLatencyFrameListNotPreviouslyAllocated:
				return "low latency frame list not allocated";
			default:
				return "error";
			}
		}

		std::size_t RingDistance(std::size_t readFrame,
								 std::size_t writeFrame) noexcept
		{
			return writeFrame >= readFrame ? writeFrame - readFrame : 0;
		}

		Float32 S24ToFloat32(std::int32_t sample) noexcept
		{
			return sample < 0
					   ? static_cast<Float32>(sample / 8388608.0f)
					   : static_cast<Float32>(sample / 8388607.0f);
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
			std::uint32_t submittedFrames = 0;
			std::uint8_t *buffer = nullptr;
			IOUSBLowLatencyIsocFrame *frames = nullptr;
		};

		struct CaptureTransfer
		{
			AkaiEieUsbTransport *owner = nullptr;
			std::size_t index = 0;
			bool submitted = false;
			std::array<std::uint8_t, kCaptureTransferBytes> buffer{};
			UInt32 requestedBytes = 0;
		};

		DeviceInterface **device = nullptr;
		InterfaceInterface **playInterface = nullptr;
		InterfaceInterface **dataInterface = nullptr;
		CFRunLoopSourceRef playAsyncSource = nullptr;
		CFRunLoopSourceRef dataAsyncSource = nullptr;
		bool deviceOpen = false;
		bool playInterfaceOpen = false;
		bool dataInterfaceOpen = false;
		UInt8 playPipe = 0;
		UInt8 capturePipe = 0;
		bool captureRunning = false;
		UInt16 playMaxPacketSize = 0;
		UInt16 captureMaxPacketSize = 0;
		std::uint32_t frameListMicros = 1000;
		std::uint64_t nextFrameStart = 0;
		std::atomic<std::uint32_t> sampleRateHz{kDefaultSampleRateHz};
		std::array<PlaybackTransfer, kPlaybackTransferCount> playbackTransfers{};
		std::array<CaptureTransfer, kCaptureTransferCount> captureTransfers{};

		std::array<std::uint8_t,
				   kOutputRingFrameCount * akai_eie::kPlaybackBytesPerFrame>
			outputRing{};
		std::atomic<std::size_t> outputReadFrame{0};
		std::atomic<std::size_t> outputWriteFrame{0};
		std::uint32_t consecutivePlaybackErrorCount = 0;
		double playbackFrameAccumulator = 0.0;

		std::mutex inputMutex;
		std::array<Float32, kInputRingFrameCount * akai_eie::kPlaybackChannelCount>
			inputRing{};
		std::size_t inputReadFrame = 0;
		std::size_t inputWriteFrame = 0;
		std::size_t inputFrameCount = 0;
		std::uint64_t inputOverrunCount = 0;
		std::uint32_t consecutiveCaptureErrorCount = 0;

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
			for (std::size_t i = 0; i < impl_->captureTransfers.size(); ++i)
			{
				impl_->captureTransfers[i].owner = this;
				impl_->captureTransfers[i].index = i;
			}
		}
	}

	AkaiEieUsbTransport::~AkaiEieUsbTransport()
	{
		Stop();
		delete impl_;
		impl_ = nullptr;
	}

	bool AkaiEieUsbTransport::Start(std::uint32_t sampleRateHz) noexcept
	{
		if (impl_ == nullptr)
			return false;
		if (!akai_eie::IsSupportedSampleRate(sampleRateHz))
			return false;

		std::unique_lock<std::mutex> lock(startMutex_);
		if (started_.load())
		{
			if (SampleRateHz() == sampleRateHz)
				return true;
			lock.unlock();
			Stop();
			lock.lock();
		}
		if (workerThread_.joinable())
		{
			lock.unlock();
			Stop();
			lock.lock();
		}

		impl_->sampleRateHz.store(sampleRateHz, std::memory_order_release);
		ResetOutputRing();
		ResetInputRing();
		stopRequested_.store(false);
		restartRequested_.store(false);
		captureRequested_.store(false);
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

		started_.store(false, std::memory_order_release);
		stopRequested_.store(true);
		restartRequested_.store(false);
		captureRequested_.store(false);

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

		ResetOutputRing();
		ResetInputRing();
	}

	void AkaiEieUsbTransport::EnqueueOutput(const Float32 *interleaved,
											std::uint32_t frameCount) noexcept
	{
		if (impl_ == nullptr || interleaved == nullptr || frameCount == 0 || !started_.load())
			return;

		std::size_t readFrame =
			impl_->outputReadFrame.load(std::memory_order_acquire);
		const std::size_t initialWriteFrame =
			impl_->outputWriteFrame.load(std::memory_order_relaxed);
		std::size_t writeFrame = initialWriteFrame;
		std::size_t bufferedFrames = RingDistance(readFrame, writeFrame);

		for (std::uint32_t frame = 0; frame < frameCount; ++frame)
		{
			if (bufferedFrames >= kOutputRingFrameCount)
			{
				readFrame =
					impl_->outputReadFrame.load(std::memory_order_acquire);
				bufferedFrames = RingDistance(readFrame, writeFrame);
				if (bufferedFrames >= kOutputRingFrameCount)
					continue;
			}

			akai_eie::EncodePlaybackFrameFromFloat32(
				interleaved + frame * akai_eie::kPlaybackChannelCount,
				impl_->outputRing.data() +
					(writeFrame % kOutputRingFrameCount) *
						akai_eie::kPlaybackBytesPerFrame);
			++writeFrame;
			++bufferedFrames;
		}

		if (writeFrame != initialWriteFrame &&
			started_.load(std::memory_order_acquire))
		{
			std::size_t expectedWriteFrame = initialWriteFrame;
			(void)impl_->outputWriteFrame.compare_exchange_strong(
				expectedWriteFrame, writeFrame, std::memory_order_release,
				std::memory_order_relaxed);
		}
	}

	std::uint32_t AkaiEieUsbTransport::DequeueInput(
		Float32 *interleaved, std::uint32_t frameCount) noexcept
	{
		if (interleaved == nullptr || frameCount == 0)
			return 0;

		std::memset(interleaved, 0,
					static_cast<std::size_t>(frameCount) *
						akai_eie::kPlaybackChannelCount * sizeof(Float32));

		if (impl_ == nullptr || !started_.load())
			return 0;

		captureRequested_.store(true, std::memory_order_release);

		std::uint32_t copied = 0;
		std::lock_guard<std::mutex> lock(impl_->inputMutex);
		while (copied < frameCount && impl_->inputFrameCount > 0)
		{
			const std::size_t contiguousAvailable =
				std::min(impl_->inputFrameCount,
						 kInputRingFrameCount - impl_->inputReadFrame);
			const std::size_t contiguousNeeded = frameCount - copied;
			const std::size_t framesToCopy =
				std::min(contiguousAvailable, contiguousNeeded);
			const std::size_t samplesToCopy =
				framesToCopy * akai_eie::kPlaybackChannelCount;

			std::memcpy(interleaved +
							copied * akai_eie::kPlaybackChannelCount,
						impl_->inputRing.data() +
							impl_->inputReadFrame *
								akai_eie::kPlaybackChannelCount,
						samplesToCopy * sizeof(Float32));
			impl_->inputReadFrame =
				(impl_->inputReadFrame + framesToCopy) % kInputRingFrameCount;
			impl_->inputFrameCount -= framesToCopy;
			copied += static_cast<std::uint32_t>(framesToCopy);
		}

		return copied;
	}

	void AkaiEieUsbTransport::WorkerMain() noexcept
	{
		const int qosResult =
			pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
		if (qosResult != 0)
		{
			os_log_error(OS_LOG_DEFAULT,
						 "AKAI EIE Pro USB: failed to raise worker QoS %{public}d",
						 qosResult);
		}

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
			if (impl_->dataInterface != nullptr && impl_->capturePipe != 0)
				(*impl_->dataInterface)->AbortPipe(impl_->dataInterface, impl_->capturePipe);
		};
		auto clearSubmittedTransfers = [this]() noexcept
		{
			for (auto &transfer : impl_->playbackTransfers)
			{
				transfer.submitted = false;
				transfer.submittedFrames = 0;
			}
			for (auto &transfer : impl_->captureTransfers)
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
		auto startCaptureIfRequested = [this]() noexcept
		{
			if (!captureRequested_.load(std::memory_order_acquire) ||
				impl_->captureRunning || impl_->capturePipe == 0)
			{
				return;
			}

			if (!StartCaptureTransfers())
				RequestRestart("capture transfer start failed");
		};

		while (!stopRequested_.load())
		{
			restartRequested_.store(false);

			const bool usbOpen = OpenUsb();
			const bool open =
				usbOpen && StartPlaybackTransfers();
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
				startCaptureIfRequested();
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
			ResetInputRing();
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
		findPipe(impl_->dataInterface, kUSBIn, kUSBBulk, true,
				 akai_eie::kKnownCaptureEndpointAddress & 0x0f, false,
				 &impl_->capturePipe, &impl_->captureMaxPacketSize);

		const std::uint32_t sampleRateHz = SampleRateHz();
		akai_eie::ControlTransfer transfers[akai_eie::kResetControlTransferCount]{};
		const std::size_t transferCount = akai_eie::BuildResetControlTransfers(
			sampleRateHz, transfers, akai_eie::kResetControlTransferCount);
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

		const bool needsDataAsyncSource = impl_->capturePipe != 0;
		if (needsDataAsyncSource)
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
		if (!CreatePlaybackLowLatencyBuffers())
			return false;

		os_log(OS_LOG_DEFAULT,
			   "AKAI EIE Pro USB: opened %{public}u Hz playback pipe %{public}u maxPacket=%{public}u frameList=%{public}uus startFrame=%{public}llu lowLatency=1 capturePipe=%{public}u",
			   sampleRateHz, impl_->playPipe, impl_->playMaxPacketSize, impl_->frameListMicros,
			   static_cast<unsigned long long>(impl_->nextFrameStart),
			   impl_->capturePipe);
		return true;
	}

	void AkaiEieUsbTransport::CloseUsb() noexcept
	{
		DestroyPlaybackLowLatencyBuffers();

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
		impl_->capturePipe = 0;
		impl_->captureRunning = false;
		impl_->playMaxPacketSize = 0;
		impl_->captureMaxPacketSize = 0;
		impl_->nextFrameStart = 0;
		impl_->playbackFrameAccumulator = 0.0;
	}

	bool AkaiEieUsbTransport::CreatePlaybackLowLatencyBuffers() noexcept
	{
		if (impl_->playInterface == nullptr)
			return false;

		for (auto &transfer : impl_->playbackTransfers)
		{
			if (transfer.buffer != nullptr || transfer.frames != nullptr)
				continue;

			void *dataBuffer = nullptr;
			IOReturn result =
				(*impl_->playInterface)
					->LowLatencyCreateBuffer(
						impl_->playInterface, &dataBuffer,
						kMaxPlaybackTransferBytes,
						kUSBLowLatencyWriteBuffer);
			if (result != kIOReturnSuccess || dataBuffer == nullptr)
			{
				os_log_error(OS_LOG_DEFAULT,
							 "AKAI EIE Pro USB: low-latency playback buffer %{public}zu allocation failed %{public}s (%{public}#x)",
							 transfer.index, IOReturnName(result), result);
				DestroyPlaybackLowLatencyBuffers();
				return false;
			}
			transfer.buffer = static_cast<std::uint8_t *>(dataBuffer);
			std::memset(transfer.buffer, 0, kMaxPlaybackTransferBytes);

			void *frameBuffer = nullptr;
			result =
				(*impl_->playInterface)
					->LowLatencyCreateBuffer(
						impl_->playInterface, &frameBuffer,
						sizeof(IOUSBLowLatencyIsocFrame) *
							kPlaybackIsoPacketCount,
						kUSBLowLatencyFrameListBuffer);
			if (result != kIOReturnSuccess || frameBuffer == nullptr)
			{
				os_log_error(OS_LOG_DEFAULT,
							 "AKAI EIE Pro USB: low-latency playback frame list %{public}zu allocation failed %{public}s (%{public}#x)",
							 transfer.index, IOReturnName(result), result);
				DestroyPlaybackLowLatencyBuffers();
				return false;
			}
			transfer.frames =
				static_cast<IOUSBLowLatencyIsocFrame *>(frameBuffer);
			std::memset(transfer.frames, 0,
						sizeof(IOUSBLowLatencyIsocFrame) *
							kPlaybackIsoPacketCount);
		}

		os_log(OS_LOG_DEFAULT,
			   "AKAI EIE Pro USB: low-latency playback buffers ready %{public}zu transfers",
			   impl_->playbackTransfers.size());
		return true;
	}

	void AkaiEieUsbTransport::DestroyPlaybackLowLatencyBuffers() noexcept
	{
		if (impl_->playInterface == nullptr)
		{
			for (auto &transfer : impl_->playbackTransfers)
			{
				transfer.buffer = nullptr;
				transfer.frames = nullptr;
				transfer.submitted = false;
				transfer.submittedFrames = 0;
			}
			return;
		}

		for (auto &transfer : impl_->playbackTransfers)
		{
			transfer.submitted = false;
			transfer.submittedFrames = 0;
			if (transfer.frames != nullptr)
			{
				const IOReturn result =
					(*impl_->playInterface)
						->LowLatencyDestroyBuffer(
							impl_->playInterface, transfer.frames);
				if (result != kIOReturnSuccess)
				{
					os_log_error(OS_LOG_DEFAULT,
								 "AKAI EIE Pro USB: low-latency playback frame list %{public}zu destroy failed %{public}s (%{public}#x)",
								 transfer.index, IOReturnName(result),
								 result);
				}
				transfer.frames = nullptr;
			}
			if (transfer.buffer != nullptr)
			{
				const IOReturn result =
					(*impl_->playInterface)
						->LowLatencyDestroyBuffer(
							impl_->playInterface, transfer.buffer);
				if (result != kIOReturnSuccess)
				{
					os_log_error(OS_LOG_DEFAULT,
								 "AKAI EIE Pro USB: low-latency playback buffer %{public}zu destroy failed %{public}s (%{public}#x)",
								 transfer.index, IOReturnName(result),
								 result);
				}
				transfer.buffer = nullptr;
			}
		}
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

	bool AkaiEieUsbTransport::StartCaptureTransfers() noexcept
	{
		if (impl_->capturePipe == 0)
			return true;

		for (std::size_t i = 0; i < impl_->captureTransfers.size(); ++i)
		{
			if (!SubmitCaptureTransfer(i))
				return false;
		}
		impl_->captureRunning = true;
		return true;
	}

	bool AkaiEieUsbTransport::SubmitPlaybackTransfer(
		std::size_t transferIndex) noexcept
	{
		if (impl_->playInterface == nullptr || impl_->playPipe == 0 ||
			transferIndex >= impl_->playbackTransfers.size() ||
			stopRequested_.load())
		{
			return false;
		}

		auto &transfer = impl_->playbackTransfers[transferIndex];
		if (transfer.buffer == nullptr || transfer.frames == nullptr)
			return false;

		const std::uint32_t framesWanted = CalculatePlaybackFramesWanted();
		(void)PopOutputFrames(transfer.buffer, framesWanted);

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
			if (haveCurrentFrame &&
				impl_->nextFrameStart <
					currentFrame + kUsbScheduleLeadBusFrames)
			{
				impl_->nextFrameStart = currentFrame + kUsbScheduleLeadBusFrames;
			}

			const std::uint64_t oldNextFrameStart = impl_->nextFrameStart;
			impl_->nextFrameStart += transferDurationBusFrames;

			transfer.submitted = true;
			transfer.submittedFrames = framesWanted;
			inFlightTransfers_.fetch_add(1);
			const IOReturn result =
				(*impl_->playInterface)
					->LowLatencyWriteIsochPipeAsync(
						impl_->playInterface, impl_->playPipe,
						transfer.buffer, oldNextFrameStart,
						kPlaybackIsoPacketCount, 0, transfer.frames,
						&AkaiEieUsbTransport::PlaybackComplete,
						&transfer);
			if (result == kIOReturnSuccess)
				return true;

			transfer.submitted = false;
			transfer.submittedFrames = 0;
			inFlightTransfers_.fetch_sub(1);
			impl_->nextFrameStart = oldNextFrameStart;
			if (result == kIOReturnIsoTooOld || result == kIOReturnIsoTooNew)
			{
				if (ReadCurrentUsbFrame(&currentFrame))
					impl_->nextFrameStart =
						currentFrame + kUsbScheduleRecoveryLeadBusFrames;
				continue;
			}

			return false;
		}

		return false;
	}

	bool AkaiEieUsbTransport::SubmitCaptureTransfer(
		std::size_t transferIndex) noexcept
	{
		if (impl_->dataInterface == nullptr || impl_->capturePipe == 0 ||
			transferIndex >= impl_->captureTransfers.size() ||
			stopRequested_.load())
		{
			return false;
		}

		auto &transfer = impl_->captureTransfers[transferIndex];
		std::memset(transfer.buffer.data(), 0, transfer.buffer.size());
		transfer.requestedBytes =
			static_cast<UInt32>(transfer.buffer.size());

		transfer.submitted = true;
		inFlightTransfers_.fetch_add(1);
		const IOReturn result = (*impl_->dataInterface)
									->ReadPipeAsync(impl_->dataInterface,
													impl_->capturePipe,
													transfer.buffer.data(),
													transfer.requestedBytes,
													&AkaiEieUsbTransport::CaptureComplete,
													&transfer);
		if (result == kIOReturnSuccess)
			return true;

		transfer.submitted = false;
		inFlightTransfers_.fetch_sub(1);
		return false;
	}

	void AkaiEieUsbTransport::RequestRestart(const char *reason) noexcept
	{
		(void)reason;
		if (stopRequested_.load())
			return;

		restartRequested_.store(true);

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

	void AkaiEieUsbTransport::HandlePlaybackComplete(std::size_t transferIndex,
													 IOReturn result) noexcept
	{
		if (transferIndex >= impl_->playbackTransfers.size())
			return;

		auto &transfer = impl_->playbackTransfers[transferIndex];
		if (transfer.submitted)
		{
			transfer.submitted = false;
			transfer.submittedFrames = 0;
			inFlightTransfers_.fetch_sub(1);
		}
		else if (!stopRequested_.load())
		{
			return;
		}
		else
		{
			return;
		}

		bool completionError = result != kIOReturnSuccess;
		if (result == kIOReturnSuccess)
		{
			for (std::size_t i = 0; i < kPlaybackIsoPacketCount; ++i)
			{
				const auto &frame = transfer.frames[i];
				if (frame.frStatus != kIOReturnSuccess)
				{
					completionError = true;
					break;
				}
			}
		}

		if (completionError && !stopRequested_.load() && !restartRequested_.load())
		{
			++impl_->consecutivePlaybackErrorCount;
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

	void AkaiEieUsbTransport::HandleCaptureComplete(
		std::size_t transferIndex, IOReturn result,
		std::uint32_t actualBytes) noexcept
	{
		if (transferIndex >= impl_->captureTransfers.size())
			return;

		auto &transfer = impl_->captureTransfers[transferIndex];
		if (transfer.submitted)
		{
			transfer.submitted = false;
			inFlightTransfers_.fetch_sub(1);
		}
		else if (!stopRequested_.load())
		{
			return;
		}
		else
		{
			return;
		}

		if (result == kIOReturnSuccess)
		{
			if (actualBytes > 0)
			{
				PushCaptureUsbFrames(transfer.buffer.data(),
									 std::min<std::size_t>(
										 actualBytes, transfer.requestedBytes));
			}
			impl_->consecutiveCaptureErrorCount = 0;
		}
		else if (!stopRequested_.load() && !restartRequested_.load())
		{
			++impl_->consecutiveCaptureErrorCount;
			if (impl_->consecutiveCaptureErrorCount >=
				kMaxConsecutiveCaptureErrors)
			{
				RequestRestart("repeated capture completion errors");
				return;
			}
		}

		if (!stopRequested_.load() && !restartRequested_.load() &&
			!SubmitCaptureTransfer(transferIndex))
		{
			RequestRestart("capture transfer resubmit failed");
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

	std::uint32_t AkaiEieUsbTransport::SampleRateHz() const noexcept
	{
		if (impl_ == nullptr)
			return kDefaultSampleRateHz;
		return impl_->sampleRateHz.load(std::memory_order_acquire);
	}

	void AkaiEieUsbTransport::PushCaptureUsbFrames(
		const std::uint8_t *usbFrames, std::size_t byteCount) noexcept
	{
		if (impl_ == nullptr || usbFrames == nullptr)
			return;

		const std::size_t frameCount =
			byteCount / akai_eie::kCaptureBytesPerUsbFrame;
		if (frameCount == 0)
			return;

		std::lock_guard<std::mutex> lock(impl_->inputMutex);
		for (std::size_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
		{
			const akai_eie::CaptureSampleFrame decoded =
				akai_eie::DecodeCaptureUsbFrame(
					usbFrames +
					frameIndex * akai_eie::kCaptureBytesPerUsbFrame);

			if (impl_->inputFrameCount == kInputRingFrameCount)
			{
				impl_->inputReadFrame =
					(impl_->inputReadFrame + 1) % kInputRingFrameCount;
				--impl_->inputFrameCount;
				++impl_->inputOverrunCount;
			}

			Float32 *out =
				impl_->inputRing.data() +
				impl_->inputWriteFrame * akai_eie::kPlaybackChannelCount;
			out[0] = S24ToFloat32(decoded.ch1);
			out[1] = S24ToFloat32(decoded.ch2);
			out[2] = S24ToFloat32(decoded.ch3);
			out[3] = S24ToFloat32(decoded.ch4);

			impl_->inputWriteFrame =
				(impl_->inputWriteFrame + 1) % kInputRingFrameCount;
			++impl_->inputFrameCount;
		}
	}

	std::uint32_t AkaiEieUsbTransport::CalculatePlaybackFramesWanted() noexcept
	{
		impl_->playbackFrameAccumulator +=
			static_cast<double>(SampleRateHz()) *
			PlaybackTransferSeconds(impl_->frameListMicros);

		std::uint32_t framesWanted = static_cast<std::uint32_t>(
			impl_->playbackFrameAccumulator);
		if (framesWanted < 1)
		{
			framesWanted = 1;
			impl_->playbackFrameAccumulator = 0.0;
		}
		else if (framesWanted > kMaxPlaybackFramesPerTransfer)
		{
			framesWanted = kMaxPlaybackFramesPerTransfer;
			impl_->playbackFrameAccumulator = 0.0;
		}
		else
		{
			impl_->playbackFrameAccumulator -=
				static_cast<double>(framesWanted);
		}

		return framesWanted;
	}

	std::size_t AkaiEieUsbTransport::PopOutputFrames(std::uint8_t *outFrames,
													 std::size_t frameCount) noexcept
	{
		if (outFrames == nullptr || frameCount == 0)
			return 0;

		const std::size_t initialReadFrame =
			impl_->outputReadFrame.load(std::memory_order_relaxed);
		const std::size_t writeFrame =
			impl_->outputWriteFrame.load(std::memory_order_acquire);
		const std::size_t availableFrames = std::min(
			RingDistance(initialReadFrame, writeFrame), kOutputRingFrameCount);
		const std::size_t framesToCopy =
			std::min(frameCount, availableFrames);

		std::size_t readFrame = initialReadFrame;
		std::size_t copied = 0;
		while (copied < framesToCopy)
		{
			const std::size_t ringFrame =
				readFrame % kOutputRingFrameCount;
			const std::size_t contiguousAvailable =
				std::min(framesToCopy - copied,
						 kOutputRingFrameCount - ringFrame);
			const std::size_t bytesToCopy =
				contiguousAvailable * akai_eie::kPlaybackBytesPerFrame;

			std::memcpy(
				outFrames + copied * akai_eie::kPlaybackBytesPerFrame,
				impl_->outputRing.data() +
					ringFrame * akai_eie::kPlaybackBytesPerFrame,
				bytesToCopy);
			readFrame += contiguousAvailable;
			copied += contiguousAvailable;
		}
		if (copied > 0)
			impl_->outputReadFrame.store(readFrame,
										 std::memory_order_release);

		if (copied < frameCount)
		{
			std::memset(
				outFrames + copied * akai_eie::kPlaybackBytesPerFrame,
				0,
				(frameCount - copied) * akai_eie::kPlaybackBytesPerFrame);
		}

		return copied;
	}

	void AkaiEieUsbTransport::ResetOutputRing() noexcept
	{
		if (impl_ == nullptr)
			return;

		impl_->outputReadFrame.store(0, std::memory_order_release);
		impl_->outputWriteFrame.store(0, std::memory_order_release);
		impl_->consecutivePlaybackErrorCount = 0;
		impl_->playbackFrameAccumulator = 0.0;
	}

	void AkaiEieUsbTransport::ResetInputRing() noexcept
	{
		if (impl_ == nullptr)
			return;

		std::lock_guard<std::mutex> lock(impl_->inputMutex);
		impl_->inputReadFrame = 0;
		impl_->inputWriteFrame = 0;
		impl_->inputFrameCount = 0;
		impl_->inputOverrunCount = 0;
		impl_->consecutiveCaptureErrorCount = 0;
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

	void AkaiEieUsbTransport::CaptureComplete(void *refcon, IOReturn result,
											  void *arg0) noexcept
	{
		auto *transfer = static_cast<Impl::CaptureTransfer *>(refcon);
		if (transfer == nullptr || transfer->owner == nullptr)
			return;
		const std::uint32_t actualBytes = static_cast<std::uint32_t>(
			reinterpret_cast<std::uintptr_t>(arg0));
		transfer->owner->HandleCaptureComplete(transfer->index, result,
											   actualBytes);
	}

} // namespace akai_eie_hal
